#include <gtest/gtest.h>
#include "tabular/relation.h"

#include <string>
#include <string_view>
#include <tuple>
#include <vector>

using namespace tabular;

static bool Mentions(std::string const& message, std::string_view text)
{
	return message.find(text) != std::string::npos;
}

namespace
{

// spawn_points(package hash key, section int, x double, note command), ordered by (package, section).
add_column const kColumns[] = {
	{"package", column_type::Hash,    true},
	{"section", column_type::Int,     false},
	{"x",       column_type::Double,  false},
	{"note",    column_type::Command, false},
};
uint32_t const kOrder[] = {0, 1};

relation_store Store(bool unique, bool read_only)
{
	static order_by orders[2] = {{kOrder, false}, {kOrder, true}};
	create_table const table{"spawn_points", kColumns, &orders[unique ? 1 : 0], read_only};
	auto s = BuildSchema(std::span<create_table const>(&table, 1));
	EXPECT_TRUE(s.has_value()) << s.error().message;
	return relation_store(std::move(*s));
}

// Rows of (package, section, x, note); returns the load command over the given buffers.
load_rows Rows(uint64_t owner, std::vector<cell>& values, std::vector<std::string_view>& commands,
	std::initializer_list<std::tuple<int64_t, int64_t, double, std::string_view>> rows)
{
	for (auto const& [package, section, x, note] : rows)
	{
		values.push_back(cell::Integer(package));
		values.push_back(cell::Integer(section));
		values.push_back(cell::Real(x));
		commands.push_back(note);
	}
	return load_rows{0, owner, rows.size(), values, commands};
}

std::vector<int64_t> Sections(relation_store const& store)
{
	std::vector<int64_t> out;
	for (size_t r = 0; r < store.RowCount(0); ++r)
	{
		out.push_back(store.Value(0, r, 1)->i);
	}
	return out;
}

}

TEST(RelationLoad, SchemaRecordsReadOnly)
{
	relation_store store = Store(false, true);
	EXPECT_TRUE(store.definition().tables()[0].read_only);
	EXPECT_FALSE(Store(false, false).definition().tables()[0].read_only);
}

TEST(RelationLoad, ReadOnlyTableRefusesInsert)
{
	relation_store store = Store(false, true);
	cell const values[] = {cell::Integer(1), cell::Integer(2), cell::Real(0.5)};
	std::string_view const commands[] = {"(none)"};
	auto inserted = store.Insert(insert_row{0, 7, values, {}, commands});
	ASSERT_FALSE(inserted.has_value());
	EXPECT_TRUE(Mentions(inserted.error().message, "read-only")) << inserted.error().message;
	EXPECT_EQ(store.RowCount(0), 0u);
}

TEST(RelationLoad, LoadSortsByTheOrderAndKeepsTiesInFileOrder)
{
	relation_store store = Store(false, true);
	std::vector<cell> v;
	std::vector<std::string_view> c;
	auto loaded = store.Load(Rows(1, v, c, {{5, 3, 0.0, "a"}, {5, 1, 0.0, "b"}, {2, 9, 0.0, "c"}, {5, 1, 0.0, "d"}}));
	ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
	EXPECT_EQ(Sections(store), (std::vector<int64_t>{9, 1, 1, 3}));
	EXPECT_EQ(*store.CommandAt(0, 1, 3), "b");
	EXPECT_EQ(*store.CommandAt(0, 2, 3), "d");
}

TEST(RelationLoad, ReloadReplacesOnlyThatOwnersRows)
{
	relation_store store = Store(false, true);
	std::vector<cell> v1, v2, v3;
	std::vector<std::string_view> c1, c2, c3;
	ASSERT_TRUE(store.Load(Rows(1, v1, c1, {{1, 1, 0.0, "one"}, {1, 2, 0.0, "one"}})).has_value());
	ASSERT_TRUE(store.Load(Rows(2, v2, c2, {{1, 3, 0.0, "two"}})).has_value());
	ASSERT_TRUE(store.Load(Rows(1, v3, c3, {{1, 4, 0.0, "one again"}})).has_value());
	EXPECT_EQ(Sections(store), (std::vector<int64_t>{3, 4}));
	EXPECT_EQ(store.OwnerOf(0, 0), 2u);
	EXPECT_EQ(store.OwnerOf(0, 1), 1u);
}

TEST(RelationLoad, LoadRebuildsTheKeyIndex)
{
	relation_store store = Store(false, true);
	std::vector<cell> v1, v2;
	std::vector<std::string_view> c1, c2;
	ASSERT_TRUE(store.Load(Rows(1, v1, c1, {{7, 1, 0.0, ""}, {8, 2, 0.0, ""}})).has_value());
	ASSERT_TRUE(store.Load(Rows(1, v2, c2, {{8, 5, 0.0, ""}, {7, 6, 0.0, ""}})).has_value());
	auto found = store.Find(0, 0, cell::Integer(8));
	ASSERT_TRUE(found.has_value()) << found.error().message;
	ASSERT_EQ(found->size(), 1u);
	EXPECT_EQ(store.Value(0, (*found)[0], 1)->i, 5);
}

TEST(RelationLoad, GenerationChangesOncePerLoadAndNotForAnEmptyOne)
{
	relation_store store = Store(false, true);
	uint64_t before = store.Generation(0);
	std::vector<cell> v, none;
	std::vector<std::string_view> c, noCommands;
	ASSERT_TRUE(store.Load(Rows(1, none, noCommands, {})).has_value());
	EXPECT_EQ(store.Generation(0), before);
	ASSERT_TRUE(store.Load(Rows(1, v, c, {{1, 1, 0.0, ""}, {1, 2, 0.0, ""}})).has_value());
	EXPECT_EQ(store.Generation(0), before + 1);
}

TEST(RelationLoad, LoadIsAllOrNothing)
{
	relation_store store = Store(true, true);
	std::vector<cell> v1, v2;
	std::vector<std::string_view> c1, c2;
	ASSERT_TRUE(store.Load(Rows(1, v1, c1, {{1, 1, 0.0, "kept"}})).has_value());
	uint64_t before = store.Generation(0);
	// Owner 2's second row clashes with owner 1's row: nothing of owner 2's lands.
	auto loaded = store.Load(Rows(2, v2, c2, {{1, 5, 0.0, ""}, {1, 1, 0.0, ""}}));
	ASSERT_FALSE(loaded.has_value());
	EXPECT_TRUE(Mentions(loaded.error().message, "owner 1")) << loaded.error().message;
	EXPECT_EQ(store.RowCount(0), 1u);
	EXPECT_EQ(store.Generation(0), before);
	EXPECT_EQ(store.Find(0, 0, cell::Integer(1))->size(), 1u);
}

TEST(RelationLoad, DuplicateKeyWithinOneLoadIsRefused)
{
	relation_store store = Store(true, true);
	std::vector<cell> v;
	std::vector<std::string_view> c;
	auto loaded = store.Load(Rows(3, v, c, {{4, 4, 0.0, ""}, {4, 4, 1.0, ""}}));
	ASSERT_FALSE(loaded.has_value());
	EXPECT_TRUE(Mentions(loaded.error().message, "owner 3")) << loaded.error().message;
	EXPECT_EQ(store.RowCount(0), 0u);
}

TEST(RelationLoad, ReloadingTheSameKeyUnderTheSameOwnerIsNotAClash)
{
	relation_store store = Store(true, true);
	std::vector<cell> v1, v2;
	std::vector<std::string_view> c1, c2;
	ASSERT_TRUE(store.Load(Rows(1, v1, c1, {{4, 4, 0.0, "old"}})).has_value());
	ASSERT_TRUE(store.Load(Rows(1, v2, c2, {{4, 4, 0.0, "new"}})).has_value());
	EXPECT_EQ(*store.CommandAt(0, 0, 3), "new");
}

TEST(RelationLoad, BadLoadsAreRefused)
{
	relation_store store = Store(false, true);
	cell const values[] = {cell::Integer(1), cell::Integer(1)};
	std::string_view const commands[] = {""};
	EXPECT_FALSE(store.Load(load_rows{0, 1, 1, values, commands}).has_value());   // short a value
	EXPECT_FALSE(store.Load(load_rows{9, 1, 0, {}, {}}).has_value());             // no such table

	cell const negativeHash[] = {cell::Integer(-1), cell::Integer(1), cell::Real(0)};
	auto hash = store.Load(load_rows{0, 1, 1, negativeHash, commands});
	ASSERT_FALSE(hash.has_value());
	EXPECT_TRUE(Mentions(hash.error().message, "'package'")) << hash.error().message;

	add_column const rules[] = {{"when", column_type::Condition, false}};
	create_table const table{"rules", rules, nullptr, true};
	relation_store withCondition(std::move(*BuildSchema(std::span<create_table const>(&table, 1))));
	auto conditions = withCondition.Load(load_rows{0, 1, 0, {}, {}});
	ASSERT_FALSE(conditions.has_value());
	EXPECT_TRUE(Mentions(conditions.error().message, "'when'")) << conditions.error().message;
}

TEST(RelationLoad, AHeldVersionIsNeverModified)
{
	relation_store store = Store(false, true);
	std::vector<cell> v1, v2;
	std::vector<std::string_view> c1, c2;
	ASSERT_TRUE(store.Load(Rows(1, v1, c1, {{1, 1, 0.0, "old"}, {1, 2, 0.0, "old"}})).has_value());
	auto held = store.Version(0);
	ASSERT_TRUE(held.has_value()) << held.error().message;

	ASSERT_TRUE(store.Load(Rows(1, v2, c2, {{1, 9, 0.0, "new"}})).has_value());
	store.Retract(retract_owner{1});

	// The reload and the retract made new versions; the held one still reads as loaded.
	ASSERT_EQ((*held)->RowCount(), 2u);
	EXPECT_EQ((*held)->Value(1, 1)->i, 2);
	EXPECT_EQ(*(*held)->CommandAt(0, 3), "old");
	EXPECT_EQ((*held)->Find(0, cell::Integer(1))->size(), 2u);
	EXPECT_EQ(store.RowCount(0), 0u);
}

TEST(RelationLoad, AWritableTableHasNoVersionAndRefusesLoad)
{
	relation_store store = Store(false, false);
	EXPECT_FALSE(store.Version(0).has_value());
	std::vector<cell> v;
	std::vector<std::string_view> c;
	auto loaded = store.Load(Rows(1, v, c, {{1, 1, 0.0, ""}}));
	ASSERT_FALSE(loaded.has_value());
	EXPECT_TRUE(Mentions(loaded.error().message, "not read-only")) << loaded.error().message;
}

TEST(RelationLoad, RetractStillRemovesLoadedRows)
{
	relation_store store = Store(false, true);
	std::vector<cell> v;
	std::vector<std::string_view> c;
	ASSERT_TRUE(store.Load(Rows(1, v, c, {{1, 1, 0.0, ""}})).has_value());
	store.Retract(retract_owner{1});
	EXPECT_EQ(store.RowCount(0), 0u);
}

TEST(RelationLoad, ACountThatOverflowsTheSizeCheckIsRefused)
{
	// Two value columns and no others: count * 2 wraps to 0, which an unguarded
	// size check would take for a match with empty spans.
	add_column const columns[] = {{"a", column_type::Int, false}, {"b", column_type::Int, false}};
	create_table const table{"pairs", columns, nullptr, true};
	relation_store store(std::move(*BuildSchema(std::span<create_table const>(&table, 1))));
	auto loaded = store.Load(load_rows{0, 1, size_t(1) << 63, {}, {}});
	ASSERT_FALSE(loaded.has_value());
	EXPECT_EQ(store.RowCount(0), 0u);
}

TEST(RelationLoad, AUniqueClashReportsTheLoadRows)
{
	relation_store store = Store(true, true);
	std::vector<cell> v;
	std::vector<std::string_view> c;
	load_refusal refusal;
	load_rows rows = Rows(1, v, c, {{1, 1, 0.0, ""}, {2, 2, 0.0, ""}, {1, 1, 0.0, ""}});
	rows.refusal   = &refusal;
	ASSERT_FALSE(store.Load(rows).has_value());
	EXPECT_EQ(refusal.row, 2u);
	EXPECT_EQ(refusal.earlier, 0u);
}
