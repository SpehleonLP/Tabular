#include <gtest/gtest.h>
#include "tabular/relation.h"
#include "probe_closure.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace tabular;
using namespace tabular_test;

static bool Mentions(std::string const& message, std::string_view text)
{
	return message.find(text) != std::string::npos;
}

namespace
{

// rules(priority int, when condition, then command), read-only, ordered by priority.
relation_store MakeRules(bool unique)
{
	static add_column const columns[] = {
		{"priority", column_type::Int,       false},
		{"when",     column_type::Condition, false},
		{"then",     column_type::Command,   false},
	};
	static uint32_t const order[] = {0};
	order_by const ordering{order, unique};
	create_table const table{"rules", columns, &ordering, true};
	auto s = BuildSchema(std::span<create_table const>(&table, 1));
	EXPECT_TRUE(s.has_value()) << s.error().message;
	return relation_store(std::move(*s));
}

// Loads (priority, condition) rows for owner; every command is "(act)".
tl::expected<void, ErrorContext> LoadRules(relation_store& store, uint64_t owner,
	std::vector<std::pair<int64_t, closure>> const& rows, load_refusal* refusal = nullptr)
{
	std::vector<cell>             values;
	std::vector<closure>          conditions;
	std::vector<std::string_view> commands;
	for (auto const& [priority, when] : rows)
	{
		values.push_back(cell::Integer(priority));
		conditions.push_back(when);
		commands.push_back("(act)");
	}
	return store.Load(load_rows{0, owner, rows.size(), values, commands, refusal, conditions});
}

}

TEST(RelationLoadConditions, ALoadedConditionIsCalledWithThisIn)
{
	int released = 0;
	{
		relation_store store = MakeRules(false);
		ASSERT_TRUE(LoadRules(store, 1, {{10, MakeProbe(5, &released)}}).has_value());
		int64_t low = 4, high = 5;
		EXPECT_FALSE(*store.Holds(0, 0, 1, &low));
		EXPECT_TRUE(*store.Holds(0, 0, 1, &high));
		EXPECT_EQ(released, 0);
	}
	EXPECT_EQ(released, 1);
}

TEST(RelationLoadConditions, ASurvivorSharedByTwoVersionsIsReleasedOnce)
{
	int survivor = 0, replaced = 0, added = 0;
	{
		relation_store store = MakeRules(false);
		ASSERT_TRUE(LoadRules(store, 1, {{10, MakeProbe(1, &survivor)}}).has_value());
		ASSERT_TRUE(LoadRules(store, 2, {{20, MakeProbe(1, &replaced)}}).has_value());
		auto before = store.Version(0);
		ASSERT_TRUE(before.has_value());
		ASSERT_TRUE(LoadRules(store, 2, {{30, MakeProbe(1, &added)}}).has_value());
		EXPECT_EQ(replaced, 0) << "the held version still has owner 2's old row";
		*before = nullptr;
		EXPECT_EQ(replaced, 1);
		EXPECT_EQ(survivor, 0) << "the current version still holds owner 1's row";
	}
	EXPECT_EQ(survivor, 1);
	EXPECT_EQ(added, 1);
}

TEST(RelationLoadConditions, ARefusedLoadReleasesItsClosures)
{
	int released = 0;
	relation_store store = MakeRules(false);
	cell const    values[]     = {cell::Integer(1)};
	closure const conditions[] = {MakeProbe(1, &released), MakeProbe(1, &released)};   // two for one row
	std::string_view const commands[] = {"(act)"};
	auto loaded = store.Load(load_rows{0, 1, 1, values, commands, nullptr, conditions});
	ASSERT_FALSE(loaded.has_value());
	EXPECT_TRUE(Mentions(loaded.error().message, "condition")) << loaded.error().message;
	EXPECT_EQ(released, 2);
	EXPECT_EQ(store.RowCount(0), 0u);
}

TEST(RelationLoadConditions, ANullFunctionIsNoConditionAndHolds)
{
	int released = 0;
	relation_store store = MakeRules(false);
	ASSERT_TRUE(LoadRules(store, 1, {{1, MakeProbe(5, &released)}, {2, closure{}}}).has_value());
	int64_t one = 1;
	auto first = store.Holds(0, 0, 1, &one);
	ASSERT_TRUE(first.has_value()) << first.error().format();
	EXPECT_FALSE(*first);
	auto second = store.Holds(0, 1, 1, &one);
	ASSERT_TRUE(second.has_value()) << second.error().format();
	EXPECT_TRUE(*second) << "a row with no script condition holds";
}

TEST(RelationLoadConditions, ANullFunctionWithDataIsRefusedAndNamesItsRow)
{
	int released = 0;
	relation_store store = MakeRules(false);
	load_refusal refusal;
	auto loaded = LoadRules(store, 1, {{1, MakeProbe(1, &released)}, {2, closure{nullptr, &released, nullptr}}}, &refusal);
	ASSERT_FALSE(loaded.has_value());
	EXPECT_TRUE(Mentions(loaded.error().format(), "no function")) << loaded.error().format();
	EXPECT_EQ(refusal.row, 1u);
	EXPECT_EQ(released, 1);
}

TEST(RelationLoadConditions, ARefusedReloadReleasesOnlyItsOwnClosures)
{
	int kept = 0, refused = 0;
	relation_store store = MakeRules(true);
	ASSERT_TRUE(LoadRules(store, 1, {{10, MakeProbe(1, &kept)}}).has_value());
	auto clash = LoadRules(store, 2, {{10, MakeProbe(1, &refused)}});
	ASSERT_FALSE(clash.has_value());
	EXPECT_EQ(refused, 1);
	EXPECT_EQ(kept, 0);
	int64_t five = 5;
	EXPECT_TRUE(*store.Holds(0, 0, 1, &five));
}

TEST(RelationLoadConditions, RetractClearsAConditionTable)
{
	int released = 0;
	relation_store store = MakeRules(false);
	ASSERT_TRUE(LoadRules(store, 1, {{10, MakeProbe(1, &released)}}).has_value());
	store.Retract(retract_owner{1});
	EXPECT_EQ(store.RowCount(0), 0u);
	EXPECT_EQ(released, 1);
}

TEST(RelationLoadConditions, AVersionHeldPastItsStoreReleasesWhenDropped)
{
	int released = 0;
	counted_ptr<TableVersion const> held;
	{
		relation_store store = MakeRules(false);
		ASSERT_TRUE(LoadRules(store, 1, {{10, MakeProbe(1, &released)}}).has_value());
		held = *store.Version(0);
	}
	EXPECT_EQ(released, 0);
	int64_t five = 5;
	EXPECT_TRUE(*held->Holds(0, 1, &five));
	held = nullptr;
	EXPECT_EQ(released, 1);
}

TEST(RelationLoadConditions, HoldsOnAVersionReportsTheClosuresError)
{
	int released = 0;
	relation_store store = MakeRules(false);
	ASSERT_TRUE(LoadRules(store, 1, {{10, MakeProbe(1, &released)}}).has_value());
	auto version = *store.Version(0);
	int64_t negative = -1;
	auto holds = version->Holds(0, 1, &negative);
	ASSERT_FALSE(holds.has_value());
	EXPECT_TRUE(Mentions(holds.error().message, "negative")) << holds.error().message;
	EXPECT_FALSE(version->Holds(0, 0, &negative).has_value()) << "column 0 is not a condition";
}
