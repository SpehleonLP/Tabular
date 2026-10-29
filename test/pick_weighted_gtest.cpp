#include <gtest/gtest.h>
#include "tabular/document.h"
#include "tabular/draw.h"
#include "tabular/pick.h"
#include "tabular/relation.h"
#include "tabular/rows_csv.h"

#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace tabular;

namespace
{

bool Mentions(std::string const& message, std::string_view text)
{
	return message.find(text) != std::string::npos;
}

template<typename T>
std::string RefusalOf(tl::expected<T, ErrorContext> const& e)
{
	return e ? std::string("(accepted)") : e.error().format();
}

// kind (key), weight, item.
constexpr std::string_view kLoot = R"(
[table loot
  (key kind int)
  (column w double)
  (column item int)
  (pick weighted w)
  (order kind)]
)";

// No keys: the whole table is one distribution.
constexpr std::string_view kKeyless = R"(
[table coin
  (column w double)
  (column face int)
  (pick weighted w)]
)";

relation_store StoreFrom(std::string_view source)
{
	auto s = ReadSchema(source, "weighted.tabular");
	EXPECT_TRUE(s.has_value()) << (s ? std::string() : s.error().format());
	return relation_store(std::move(*s));
}

struct loot_row
{
	int64_t kind;
	double  weight;
	int64_t item;
};

tl::expected<void, ErrorContext> LoadLoot(relation_store& store, std::vector<loot_row> const& rows, uint64_t owner = 1,
	load_refusal* refusal = nullptr)
{
	std::vector<cell> values;
	for (loot_row const& r : rows)
	{
		values.push_back(cell::Integer(r.kind));
		values.push_back(cell::Real(r.weight));
		values.push_back(cell::Integer(r.item));
	}
	return store.Load(load_rows{0, owner, rows.size(), values, {}, refusal});
}

// The item drawn for a kind at (seed, site); -1 for no run.
int64_t Drawn(relation_store const& store, int64_t kind, uint64_t seed, uint64_t site)
{
	cell const keys[] = {cell::Integer(kind)};
	auto       picked = PickWeighted(store, 0, pick_query{keys, {}}, seed, site);
	EXPECT_TRUE(picked.has_value()) << RefusalOf(picked);
	if (!picked || !*picked)
	{
		return -1;
	}
	return store.Value(0, **picked, 2)->i;
}

}

TEST(PickWeighted, ADrawIsDeterministicInSeedAndSite)
{
	relation_store store = StoreFrom(kLoot);
	ASSERT_EQ(RefusalOf(LoadLoot(store, {{1, 1, 10}, {1, 1, 20}, {1, 1, 30}})), "(accepted)");
	for (uint64_t site = 0; site < 200; ++site)
	{
		int64_t const first = Drawn(store, 1, 99, site);
		EXPECT_EQ(Drawn(store, 1, 99, site), first);
		EXPECT_EQ(Drawn(store, 1, 99, site), first);
	}
	// A different seed is a different sequence of draws.
	int differing = 0;
	for (uint64_t site = 0; site < 200; ++site)
	{
		differing += Drawn(store, 1, 99, site) != Drawn(store, 1, 100, site) ? 1 : 0;
	}
	EXPECT_GT(differing, 0);
}

TEST(PickWeighted, RowsLandInProportionToTheirWeights)
{
	relation_store store = StoreFrom(kLoot);
	ASSERT_EQ(RefusalOf(LoadLoot(store, {{1, 1, 10}, {1, 2, 20}, {1, 7, 30}})), "(accepted)");
	constexpr int kSites = 20000;
	std::array<int, 3> counts{};
	for (int site = 0; site < kSites; ++site)
	{
		int64_t const item = Drawn(store, 1, 5, static_cast<uint64_t>(site));
		ASSERT_TRUE(item == 10 || item == 20 || item == 30) << item;
		++counts[item / 10 - 1];
	}
	// Expected 2000, 4000, 14000; a binomial's standard deviation is at most
	// about 65 here, so +-300 is over four of them, and the draws are fixed.
	EXPECT_NEAR(counts[0], 2000, 300);
	EXPECT_NEAR(counts[1], 4000, 300);
	EXPECT_NEAR(counts[2], 14000, 300);
}

// Row order is the order the weights are laid end to end in, so the draw is
// readable: a point in the first half of a two-row, equal-weight run is row 0.
TEST(PickWeighted, RowOrderIsTheCumulativeOrder)
{
	relation_store forward = StoreFrom(kLoot);
	relation_store reverse = StoreFrom(kLoot);
	ASSERT_EQ(RefusalOf(LoadLoot(forward, {{1, 1, 10}, {1, 1, 20}})), "(accepted)");
	ASSERT_EQ(RefusalOf(LoadLoot(reverse, {{1, 1, 20}, {1, 1, 10}})), "(accepted)");
	int low  = 0;
	int high = 0;
	for (uint64_t site = 0; site < 500; ++site)
	{
		bool const firstHalf = DrawUnit(Draw(3, site)) < 0.5;
		EXPECT_EQ(Drawn(forward, 1, 3, site), firstHalf ? 10 : 20) << "site " << site;
		EXPECT_EQ(Drawn(reverse, 1, 3, site), firstHalf ? 20 : 10) << "site " << site;
		(firstHalf ? low : high) += 1;
	}
	EXPECT_GT(low, 0);
	EXPECT_GT(high, 0);
}

TEST(PickWeighted, KeysChooseWhichDistributionApplies)
{
	relation_store store = StoreFrom(kLoot);
	ASSERT_EQ(RefusalOf(LoadLoot(store, {{1, 1, 10}, {1, 1, 11}, {2, 5, 20}, {3, 3, 30}, {3, 1e-300, 31}})), "(accepted)");
	for (uint64_t site = 0; site < 300; ++site)
	{
		int64_t const one = Drawn(store, 1, 8, site);
		EXPECT_TRUE(one == 10 || one == 11);
		EXPECT_EQ(Drawn(store, 2, 8, site), 20) << "a single row is always drawn";
		EXPECT_EQ(Drawn(store, 3, 8, site), 30) << "a weight 1e-300 against 3 never lands in 300 sites";
	}
	EXPECT_EQ(Drawn(store, 9, 8, 0), -1) << "a key with no rows answers nothing";
}

TEST(PickWeighted, ATableWithNoKeysIsOneDistribution)
{
	relation_store store = StoreFrom(kKeyless);
	std::vector<cell> values = {cell::Real(1), cell::Integer(0), cell::Real(3), cell::Integer(1)};
	ASSERT_EQ(RefusalOf(store.Load(load_rows{0, 1, 2, values})), "(accepted)");
	int heads = 0;
	for (uint64_t site = 0; site < 8000; ++site)
	{
		auto picked = PickWeighted(store, 0, pick_query{{}, {}}, 11, site);
		ASSERT_TRUE(picked.has_value() && *picked) << RefusalOf(picked);
		heads += store.Value(0, **picked, 1)->i == 0 ? 1 : 0;
	}
	EXPECT_NEAR(heads, 2000, 200);
}

TEST(PickWeighted, TheVersionAndTheStoreAgree)
{
	relation_store store = StoreFrom(kLoot);
	ASSERT_EQ(RefusalOf(LoadLoot(store, {{1, 1, 10}, {1, 3, 20}})), "(accepted)");
	auto version = store.Version(0);
	ASSERT_TRUE(version.has_value());
	cell const keys[] = {cell::Integer(1)};
	for (uint64_t site = 0; site < 100; ++site)
	{
		auto fromVersion = PickWeighted(**version, pick_query{keys, {}}, 4, site);
		auto fromStore   = PickWeighted(store, 0, pick_query{keys, {}}, 4, site);
		ASSERT_TRUE(fromVersion.has_value() && fromStore.has_value());
		EXPECT_EQ(*fromVersion, *fromStore);
	}
}

TEST(PickWeighted, AnotherPolicyIsRefusedBothWays)
{
	relation_store weighted = StoreFrom(kLoot);
	cell const     keys[]   = {cell::Integer(1)};
	std::string const best  = RefusalOf(PickBest(weighted, 0, pick_query{keys, {}}));
	EXPECT_TRUE(Mentions(best, "weighted")) << best;
	EXPECT_TRUE(Mentions(RefusalOf(PickAll(weighted, 0, pick_query{keys, {}})), "weighted"));
	EXPECT_TRUE(Mentions(RefusalOf(PickOr(weighted, 0, pick_query{keys, {}})), "weighted"));

	constexpr std::string_view bestSchema = R"(
[table plain (key kind int (domain rows)) (order kind)]
)";
	relation_store plain = StoreFrom(bestSchema);
	std::string const refused = RefusalOf(PickWeighted(plain, 0, pick_query{keys, {}}, 1, 1));
	EXPECT_TRUE(Mentions(refused, "best")) << refused;
}

TEST(PickWeighted, AWrongKeyCountIsRefused)
{
	relation_store store = StoreFrom(kLoot);
	std::string const refused = RefusalOf(PickWeighted(store, 0, pick_query{{}, {}}, 1, 1));
	EXPECT_TRUE(Mentions(refused, "keys")) << refused;
}

// A version is a function of the owners' loads, not of their order: an owner
// reloading identical rows changes nothing any reader can see. Rows with equal
// order keys sit by owner id, then by index within the owner's load.
TEST(PickWeighted, AnIdenticalReloadChangesNoAnswer)
{
	relation_store store = StoreFrom(kLoot);
	ASSERT_EQ(RefusalOf(LoadLoot(store, {{1, 1, 10}}, 1)), "(accepted)");
	ASSERT_EQ(RefusalOf(LoadLoot(store, {{1, 1, 11}}, 2)), "(accepted)");
	std::array<int64_t, 64> before{};
	for (uint64_t site = 0; site < before.size(); ++site)
	{
		before[site] = Drawn(store, 1, 5, site);
	}
	ASSERT_EQ(RefusalOf(LoadLoot(store, {{1, 1, 10}}, 1)), "(accepted)");
	int tens = 0;
	int elevens = 0;
	for (uint64_t site = 0; site < before.size(); ++site)
	{
		EXPECT_EQ(Drawn(store, 1, 5, site), before[site]) << "site " << site;
		(before[site] == 10 ? tens : elevens) += 1;
	}
	EXPECT_GT(tens, 0);
	EXPECT_GT(elevens, 0) << "the fixed sites exercise both halves, so a reorder would show";
}

TEST(PickWeighted, OwnersRowsOfOneKeyLayOutByOwnerIdThenLoadIndex)
{
	relation_store store = StoreFrom(kLoot);
	ASSERT_EQ(RefusalOf(LoadLoot(store, {{1, 1, 21}, {1, 1, 22}}, 2)), "(accepted)");
	ASSERT_EQ(RefusalOf(LoadLoot(store, {{1, 1, 11}, {1, 1, 12}}, 1)), "(accepted)");
	std::vector<int64_t> items;
	for (size_t r = 0; r < store.RowCount(0); ++r)
	{
		items.push_back(store.Value(0, r, 2)->i);
	}
	EXPECT_EQ(items, (std::vector<int64_t>{11, 12, 21, 22}));
}

// ---- Load refusals ----------------------------------------------------------

TEST(PickWeightedLoad, AZeroNegativeOrNonFiniteWeightIsRefusedNamingItsRow)
{
	double const bad[] = {0.0, -0.0, -1.0, std::numeric_limits<double>::denorm_min(),
		std::numeric_limits<double>::min() / 2, std::numeric_limits<double>::quiet_NaN(),
		std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
	for (double weight : bad)
	{
		relation_store store = StoreFrom(kLoot);
		load_refusal   refusal;
		std::string const refused = RefusalOf(LoadLoot(store, {{1, 1, 10}, {1, weight, 20}}, 1, &refusal));
		EXPECT_TRUE(Mentions(refused, "weight 'w'")) << weight << ": " << refused;
		EXPECT_EQ(refusal.row, 1u) << weight;
		EXPECT_EQ(store.RowCount(0), 0u) << "all or none";
	}
}

// The smallest normal weight is the floor: below it a total can be subnormal,
// and the draw's product could round up to the total.
TEST(PickWeightedLoad, ASmallestNormalWeightIsAccepted)
{
	relation_store store = StoreFrom(kLoot);
	EXPECT_EQ(RefusalOf(LoadLoot(store, {{1, std::numeric_limits<double>::min(), 10}})), "(accepted)");
	for (uint64_t site = 0; site < 200; ++site)
	{
		EXPECT_EQ(Drawn(store, 1, 1, site), 10);
	}
}

// R7: the total of a run is positive because every weight is. It must also be
// finite, or "a point on the line" has no meaning.
TEST(PickWeightedLoad, ARunWhoseTotalOverflowsIsRefused)
{
	double const huge = std::numeric_limits<double>::max();
	{
		relation_store store = StoreFrom(kLoot);
		load_refusal   refusal;
		std::string const refused = RefusalOf(LoadLoot(store, {{1, huge, 10}, {2, 1, 20}, {1, huge, 11}}, 1, &refusal));
		EXPECT_TRUE(Mentions(refused, "total")) << refused;
		EXPECT_TRUE(Mentions(refused, "key (1)")) << refused;
		EXPECT_EQ(refusal.row, 2u);
		EXPECT_EQ(store.RowCount(0), 0u);
	}
	{
		// The run is made of two owners' rows; neither load alone overflows.
		relation_store store = StoreFrom(kLoot);
		ASSERT_EQ(RefusalOf(LoadLoot(store, {{1, huge, 10}}, 1)), "(accepted)");
		load_refusal refusal;
		std::string const refused = RefusalOf(LoadLoot(store, {{1, huge, 11}}, 2, &refusal));
		EXPECT_TRUE(Mentions(refused, "total")) << refused;
		EXPECT_EQ(refusal.row, 0u);
		EXPECT_EQ(store.RowCount(0), 1u);
	}
}

TEST(PickWeightedLoad, CsvNamesTheLineOfABadWeight)
{
	relation_store store = StoreFrom(kLoot);
	auto loaded = LoadCsv(store, csv_rows{0, 1, "kind, w, item\n1, 2, 10\n1, 0, 20\n", "loot.csv"});
	ASSERT_FALSE(loaded.has_value());
	EXPECT_TRUE(Mentions(loaded.error().format(), "loot.csv:3")) << loaded.error().format();
	auto nan = LoadCsv(store, csv_rows{0, 1, "kind, w, item\n1, nan, 10\n", "loot.csv"});
	ASSERT_FALSE(nan.has_value());
	EXPECT_TRUE(Mentions(nan.error().format(), "loot.csv:2")) << nan.error().format();
	auto fine = LoadCsv(store, csv_rows{0, 1, "kind, w, item\n1, 2, 10\n1, 6, 20\n", "loot.csv"});
	ASSERT_TRUE(fine.has_value()) << fine.error().format();
	EXPECT_EQ(store.RowCount(0), 2u);
}

TEST(PickWeightedLoad, ARetractIsNeverRefusedAndTheOtherOwnersRunStillDraws)
{
	relation_store store = StoreFrom(kLoot);
	ASSERT_EQ(RefusalOf(LoadLoot(store, {{1, 1, 10}}, 1)), "(accepted)");
	ASSERT_EQ(RefusalOf(LoadLoot(store, {{1, 1, 11}}, 2)), "(accepted)");
	store.Retract(retract_owner{1});
	for (uint64_t site = 0; site < 50; ++site)
	{
		EXPECT_EQ(Drawn(store, 1, 2, site), 11);
	}
}

// ---- Schema refusals --------------------------------------------------------

namespace
{

std::string SchemaRefusal(std::string_view source)
{
	auto s = ReadSchema(source, "bad.tabular");
	return s ? std::string("(accepted)") : s.error().format();
}

// A table the file format cannot spell, because the format makes it
// unwritable (a weight on a table that is not weighted, a writable weighted
// table), refused by the builder.
std::string BuildRefusal(create_table const& table)
{
	auto s = BuildSchema(std::span<create_table const>(&table, 1));
	return s ? std::string("(accepted)") : s.error().format();
}

}

TEST(PickWeightedSchema, AWeightedTableResolvesItsShape)
{
	auto s = ReadSchema(kLoot, "loot.tabular");
	ASSERT_TRUE(s.has_value()) << s.error().format();
	EXPECT_EQ(s->tables()[0].pick, pick_policy::Weighted);
	EXPECT_EQ(s->tables()[0].weight, 1u);
	EXPECT_EQ(s->tables()[0].key_count, 1u);
}

TEST(PickWeightedSchema, ABandIsRefused)
{
	std::string const refused = SchemaRefusal(R"(
[table loot
  (input t double 0..1)
  (key kind int)
  (column w double)
  (column window band (input t))
  (pick weighted w)
  (order kind)]
)");
	EXPECT_TRUE(Mentions(refused, "weighted")) << refused;
	EXPECT_TRUE(Mentions(refused, "inputs")) << refused;

	// A band with no input to test is refused before the pick is considered.
	std::string const bare = SchemaRefusal(R"(
[table loot
  (key kind int)
  (column w double)
  (column window band)
  (pick weighted w)
  (order kind)]
)");
	EXPECT_TRUE(Mentions(bare, "window")) << bare;
}

TEST(PickWeightedSchema, ARankingFlagsColumnIsRefused)
{
	std::string const refused = SchemaRefusal(R"(
[table loot
  (input mode flags)
  (key kind int)
  (column w double)
  (column req flags (input mode))
  (pick weighted w)
  (order kind)]
)");
	EXPECT_TRUE(Mentions(refused, "weighted")) << refused;
	EXPECT_TRUE(Mentions(refused, "ranking")) << refused;
}

TEST(PickWeightedSchema, AConditionColumnIsRefused)
{
	std::string const refused = SchemaRefusal(R"(
[table loot
  (key kind int)
  (column w double)
  (column when condition)
  (pick weighted w)
  (order kind)]
)");
	EXPECT_TRUE(Mentions(refused, "when")) << refused;
	EXPECT_TRUE(Mentions(refused, "weighted")) << refused;
}

TEST(PickWeightedSchema, ADeclaredInputIsRefused)
{
	std::string const refused = SchemaRefusal(R"(
[table loot
  (input t double)
  (key kind int)
  (column w double)
  (pick weighted w)
  (order kind)]
)");
	EXPECT_TRUE(Mentions(refused, "input")) << refused;
	EXPECT_TRUE(Mentions(refused, "weighted")) << refused;
}

TEST(PickWeightedSchema, TheWeightColumnIsRequiredAndMustBeADouble)
{
	std::string const none = SchemaRefusal(R"(
[table loot
  (key kind int)
  (column w double)
  (pick weighted)
  (order kind)]
)");
	EXPECT_TRUE(Mentions(none, "(pick weighted ..) names the column it reads")) << none;

	std::string const notDouble = SchemaRefusal(R"(
[table loot
  (key kind int)
  (column w int)
  (pick weighted w)
  (order kind)]
)");
	EXPECT_TRUE(Mentions(notDouble, "double")) << notDouble;

	std::string const missing = SchemaRefusal(R"(
[table loot
  (key kind int)
  (column w double)
  (pick weighted nope)
  (order kind)]
)");
	EXPECT_TRUE(Mentions(missing, "nope")) << missing;
}

TEST(PickWeightedSchema, OnlyAWeightedTableNamesAWeight)
{
	static add_column const columns[] = {{"kind", column_type::Int, true, UINT32_MAX, key_domain::Rows()},
	                                     {"w", column_type::Double}};
	static uint32_t const   byKind[]  = {0};
	static order_by const   order{byKind, false};
	create_table            table{"lookup", columns, &order, true, {}, pick_policy::Best};
	table.weight = 1;
	std::string const refused = BuildRefusal(table);
	EXPECT_TRUE(Mentions(refused, "weight")) << refused;
	EXPECT_TRUE(Mentions(refused, "weighted")) << refused;

	// A table with no pick would otherwise return before the check and leave the
	// weight written and never read.
	static add_column const unpickedColumns[] = {{"kind", column_type::Int, true}, {"w", column_type::Double}};
	create_table            unpickedTable{"lookup", unpickedColumns, &order, false};
	unpickedTable.weight = 1;
	std::string const unpicked = BuildRefusal(unpickedTable);
	EXPECT_TRUE(Mentions(unpicked, "weight")) << unpicked;
	EXPECT_TRUE(Mentions(unpicked, "weighted")) << unpicked;
}

TEST(PickWeightedSchema, AWeightedTableIsReadOnlyAndKeyedLikeAnyPick)
{
	static add_column const columns[] = {{"kind", column_type::Int, true}, {"w", column_type::Double}};
	static uint32_t const   byKind[]  = {0};
	static order_by const   order{byKind, false};
	create_table            table{"loot", columns, &order, false, {}, pick_policy::Weighted};
	table.weight = 1;
	std::string const writable = BuildRefusal(table);
	EXPECT_TRUE(Mentions(writable, "read-only")) << writable;

	std::string const unordered = SchemaRefusal(R"(
[table loot
  (key kind int)
  (column w double)
  (pick weighted w)]
)");
	EXPECT_TRUE(Mentions(unordered, "order")) << unordered;
}

TEST(PickWeightedSchema, AWeightedTableCannotBePivoted)
{
	std::string const refused = SchemaRefusal(R"(
[table loot
  (key kind int)
  (column w double)
  (column item int)
  (pick weighted w)
  (order kind)
  (pivot item w)]
)");
	EXPECT_TRUE(Mentions(refused, "pivot")) << refused;
}

TEST(PickWeightedSchema, AGridCannotNameAWeight)
{
	grid_shape const shape{{"r", column_type::Int}, {"c", column_type::Int}, column_type::Int};
	create_table     t{"array", {}, nullptr, true, {}, pick_policy::None, UINT32_MAX, nullptr, &shape};
	t.weight = 0;
	auto s = BuildSchema(std::span<create_table const>(&t, 1));
	ASSERT_FALSE(s.has_value());
	EXPECT_TRUE(Mentions(s.error().message, "grid")) << s.error().message;
}
