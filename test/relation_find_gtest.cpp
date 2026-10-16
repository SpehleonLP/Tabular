#include <gtest/gtest.h>
#include "tabular/relation.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace tabular;

// The message mentions the text: errors must name what they refuse.
static bool Mentions(std::string const& message, std::string_view text)
{
	return message.find(text) != std::string::npos;
}

namespace
{

add_column const kSpawnColumns[] = {
	{"package", column_type::Hash,   true},
	{"section", column_type::Hash,   true},
	{"x",       column_type::Double, false},
	{"y",       column_type::Double, false},
};

relation_store SpawnPoints()
{
	create_table const table{"spawn_points", kSpawnColumns};
	auto s = BuildSchema(std::span<create_table const>(&table, 1));
	EXPECT_TRUE(s.has_value());
	return relation_store(std::move(*s));
}

// Spawn points ordered by (section, package).
relation_store OrderedSpawnPoints()
{
	static uint32_t const order[] = {1, 0};
	order_by const ordering{order, false};
	create_table const table{"spawn_points", kSpawnColumns, &ordering};
	auto s = BuildSchema(std::span<create_table const>(&table, 1));
	EXPECT_TRUE(s.has_value());
	return relation_store(std::move(*s));
}

tl::expected<void, ErrorContext> Spawn(relation_store& store, uint64_t owner, int64_t package, int64_t section,
	double x, double y)
{
	cell const values[] = {cell::Integer(package), cell::Integer(section), cell::Real(x), cell::Real(y)};
	return store.Insert(insert_row{0, owner, values, {}, {}});
}

int64_t IntAt(relation_store const& store, size_t row, uint32_t column)
{
	auto v = store.Value(0, row, column);
	EXPECT_TRUE(v.has_value());
	return v ? v->i : INT64_MIN;
}

double RealAt(relation_store const& store, size_t row, uint32_t column)
{
	auto v = store.Value(0, row, column);
	EXPECT_TRUE(v.has_value());
	return v ? v->d : -1.0;
}

}

TEST(RelationFind, FindsEveryRowWithTheKeyInRowOrder)
{
	relation_store store = SpawnPoints();
	ASSERT_TRUE(Spawn(store, 1, 10, 500, 0, 0).has_value());
	ASSERT_TRUE(Spawn(store, 1, 10, 600, 0, 0).has_value());
	ASSERT_TRUE(Spawn(store, 2, 20, 500, 0, 0).has_value());

	auto bySection = store.Find(0, 1, cell::Integer(500));
	ASSERT_TRUE(bySection.has_value()) << bySection.error().message;
	EXPECT_EQ(*bySection, (std::vector<size_t>{0, 2}));

	auto byPackage = store.Find(0, 0, cell::Integer(10));
	ASSERT_TRUE(byPackage.has_value());
	EXPECT_EQ(*byPackage, (std::vector<size_t>{0, 1}));
}

TEST(RelationFind, MissingKeyFindsNothing)
{
	relation_store store = SpawnPoints();
	ASSERT_TRUE(Spawn(store, 1, 10, 500, 0, 0).has_value());
	auto found = store.Find(0, 1, cell::Integer(999));
	ASSERT_TRUE(found.has_value());
	EXPECT_TRUE(found->empty());
}

TEST(RelationFind, NonKeyColumnIsRefused)
{
	relation_store store = SpawnPoints();
	auto found = store.Find(0, 2, cell::Real(0));
	ASSERT_FALSE(found.has_value());
	EXPECT_TRUE(Mentions(found.error().message, "'x'")) << found.error().message;
}

TEST(RelationFind, OutOfRangeTableOrColumnIsRefused)
{
	relation_store store = SpawnPoints();
	EXPECT_FALSE(store.Find(3, 0, cell::Integer(0)).has_value());
	EXPECT_FALSE(store.Find(0, 9, cell::Integer(0)).has_value());
}

TEST(RelationFind, FindAfterRetractReturnsNewPositions)
{
	relation_store store = SpawnPoints();
	ASSERT_TRUE(Spawn(store, 1, 10, 500, 0, 0).has_value());
	ASSERT_TRUE(Spawn(store, 2, 20, 500, 0, 0).has_value());
	ASSERT_TRUE(Spawn(store, 3, 30, 500, 0, 0).has_value());

	store.Retract(retract_owner{1});

	auto found = store.Find(0, 1, cell::Integer(500));
	ASSERT_TRUE(found.has_value());
	EXPECT_EQ(*found, (std::vector<size_t>{0, 1}));
	EXPECT_EQ(IntAt(store, (*found)[0], 0), 20);
	EXPECT_EQ(IntAt(store, (*found)[1], 0), 30);
}

TEST(RelationFind, ExtremeValuesLookUpAndRetract)
{
	relation_store store = SpawnPoints();
	int64_t const highBitHash = INT64_C(0x80000000);   // a CityHash32 with bit 31 set
	int64_t const maxHash     = INT64_C(0xFFFFFFFF);
	ASSERT_TRUE(Spawn(store, UINT64_MAX, highBitHash, maxHash, 0, 0).has_value());
	ASSERT_TRUE(Spawn(store, 1, 7, 7, 0, 0).has_value());

	auto byHash = store.Find(0, 0, cell::Integer(highBitHash));
	ASSERT_TRUE(byHash.has_value());
	EXPECT_EQ(*byHash, (std::vector<size_t>{0}));
	auto byMax = store.Find(0, 1, cell::Integer(maxHash));
	ASSERT_TRUE(byMax.has_value());
	EXPECT_EQ(*byMax, (std::vector<size_t>{0}));
	EXPECT_EQ(store.OwnerOf(0, 0), UINT64_MAX);

	store.Retract(retract_owner{UINT64_MAX});
	EXPECT_EQ(store.RowCount(0), 1u);
	auto gone = store.Find(0, 0, cell::Integer(highBitHash));
	ASSERT_TRUE(gone.has_value());
	EXPECT_TRUE(gone->empty());
}

TEST(RelationFind, NegativeIntKeysLookUp)
{
	static add_column const columns[] = {{"offset", column_type::Int, true}};
	create_table const table{"offsets", columns};
	auto s = BuildSchema(std::span<create_table const>(&table, 1));
	ASSERT_TRUE(s.has_value()) << s.error().message;
	relation_store store(std::move(*s));
	for (int64_t v : {INT64_MIN, int64_t(-5), int64_t(5)})
	{
		cell const values[] = {cell::Integer(v)};
		ASSERT_TRUE(store.Insert(insert_row{0, 1, values, {}, {}}).has_value());
	}
	auto found = store.Find(0, 0, cell::Integer(-5));
	ASSERT_TRUE(found.has_value()) << found.error().message;
	EXPECT_EQ(*found, (std::vector<size_t>{1}));
	auto lowest = store.Find(0, 0, cell::Integer(INT64_MIN));
	ASSERT_TRUE(lowest.has_value());
	EXPECT_EQ(*lowest, (std::vector<size_t>{0}));
}

TEST(RelationFind, OutOfRangeHashLookupIsRefused)
{
	relation_store store = OrderedSpawnPoints();
	ASSERT_TRUE(Spawn(store, 1, INT64_C(0x80000000), 500, 0, 0).has_value());

	// The sign-extended spelling of the stored hash: refused, not a silent miss.
	auto byKey = store.Find(0, 0, cell::Integer(INT64_C(-2147483648)));
	ASSERT_FALSE(byKey.has_value());
	EXPECT_TRUE(Mentions(byKey.error().message, "'package'")) << byKey.error().message;

	cell const prefix[] = {cell::Integer(-1)};
	auto byOrder = store.FindOrdered(0, prefix);
	ASSERT_FALSE(byOrder.has_value());
	EXPECT_TRUE(Mentions(byOrder.error().message, "'section'")) << byOrder.error().message;
}

TEST(RelationFind, KeyLookupIsRightAfterAnInsertInTheMiddle)
{
	static add_column const columns[] = {
		{"rank", column_type::Int,  false},
		{"tag",  column_type::Hash, true},
	};
	static uint32_t const order[] = {0};
	order_by const ordering{order, false};
	create_table const table{"t", columns, &ordering};
	auto s = BuildSchema(std::span<create_table const>(&table, 1));
	ASSERT_TRUE(s.has_value()) << s.error().message;
	relation_store store(std::move(*s));

	for (int64_t rank : {10, 30, 20})
	{
		cell const values[] = {cell::Integer(rank), cell::Integer(7)};
		ASSERT_TRUE(store.Insert(insert_row{0, 1, values, {}, {}}).has_value());
	}

	auto found = store.Find(0, 1, cell::Integer(7));
	ASSERT_TRUE(found.has_value());
	EXPECT_EQ(*found, (std::vector<size_t>{0, 1, 2}));
	EXPECT_EQ(IntAt(store, 1, 0), 20);
}

TEST(RelationFind, OrderedPrefixFindsAContiguousRange)
{
	relation_store store = OrderedSpawnPoints();
	ASSERT_TRUE(Spawn(store, 1, 10, 600, 0, 0).has_value());
	ASSERT_TRUE(Spawn(store, 1, 10, 500, 0, 0).has_value());
	ASSERT_TRUE(Spawn(store, 2, 20, 500, 0, 0).has_value());
	ASSERT_TRUE(Spawn(store, 2, 20, 700, 0, 0).has_value());

	cell const meadow[] = {cell::Integer(500)};
	auto inMeadow = store.FindOrdered(0, meadow);
	ASSERT_TRUE(inMeadow.has_value()) << inMeadow.error().message;
	EXPECT_EQ(inMeadow->first, 0u);
	EXPECT_EQ(inMeadow->last, 2u);

	cell const exact[] = {cell::Integer(500), cell::Integer(20)};
	auto one = store.FindOrdered(0, exact);
	ASSERT_TRUE(one.has_value());
	EXPECT_EQ(one->last - one->first, 1u);
	EXPECT_EQ(IntAt(store, one->first, 0), 20);

	cell const nowhere[] = {cell::Integer(550)};
	auto none = store.FindOrdered(0, nowhere);
	ASSERT_TRUE(none.has_value());
	EXPECT_EQ(none->first, none->last);
}

TEST(RelationFind, EmptyPrefixMatchesEveryRow)
{
	relation_store store = OrderedSpawnPoints();
	ASSERT_TRUE(Spawn(store, 1, 10, 600, 0, 0).has_value());
	ASSERT_TRUE(Spawn(store, 1, 10, 500, 0, 0).has_value());
	auto all = store.FindOrdered(0, {});
	ASSERT_TRUE(all.has_value());
	EXPECT_EQ(all->first, 0u);
	EXPECT_EQ(all->last, 2u);
}

TEST(RelationFind, BadOrderedLookupsAreRefused)
{
	relation_store unordered = SpawnPoints();
	cell const key[] = {cell::Integer(1)};
	auto noOrder = unordered.FindOrdered(0, key);
	ASSERT_FALSE(noOrder.has_value());
	EXPECT_TRUE(Mentions(noOrder.error().message, "no order")) << noOrder.error().message;

	relation_store ordered = OrderedSpawnPoints();
	cell const tooLong[] = {cell::Integer(1), cell::Integer(2), cell::Integer(3)};
	auto longer = ordered.FindOrdered(0, tooLong);
	ASSERT_FALSE(longer.has_value());
	EXPECT_TRUE(Mentions(longer.error().message, "3")) << longer.error().message;

	EXPECT_FALSE(ordered.FindOrdered(5, key).has_value());
}

// The owner's example: a package contributes spawn points, is reloaded with
// different ones, and a consumer notices through the generation and re-reads.
TEST(RelationFind, SpawnPointsSurvivePackageReload)
{
	relation_store store = OrderedSpawnPoints();
	uint64_t const grazers = 1, plants = 2;
	int64_t const  meadow  = 500;

	ASSERT_TRUE(Spawn(store, grazers, 10, meadow, 1.0, 2.0).has_value());
	ASSERT_TRUE(Spawn(store, grazers, 10, meadow, 3.0, 4.0).has_value());
	ASSERT_TRUE(Spawn(store, plants,  20, meadow, 5.0, 6.0).has_value());
	uint64_t seen = store.Generation(0);

	// Reload the grazer package with one spawn point instead of two.
	store.Retract(retract_owner{grazers});
	ASSERT_TRUE(Spawn(store, grazers, 10, meadow, 9.0, 9.5).has_value());
	EXPECT_NE(store.Generation(0), seen);

	cell const inMeadowKey[] = {cell::Integer(meadow)};
	auto inMeadow = store.FindOrdered(0, inMeadowKey);
	ASSERT_TRUE(inMeadow.has_value());
	ASSERT_EQ(inMeadow->last - inMeadow->first, 2u);
	// Ordered by (section, package): package 10 (grazers) sorts before 20 (plants).
	EXPECT_EQ(store.OwnerOf(0, inMeadow->first), grazers);
	EXPECT_EQ(RealAt(store, inMeadow->first, 2), 9.0);
	EXPECT_EQ(RealAt(store, inMeadow->first, 3), 9.5);
	EXPECT_EQ(store.OwnerOf(0, inMeadow->first + 1), plants);
	EXPECT_EQ(RealAt(store, inMeadow->first + 1, 2), 5.0);
}
