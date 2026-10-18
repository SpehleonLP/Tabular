#include <gtest/gtest.h>
#include "tabular/grid.h"
#include "tabular/relation.h"
#include "tabular/schema_yaml.h"

#include <string>
#include <string_view>
#include <vector>

using namespace tabular;

// "Rows changed" is the generation moving, and there is no callback: a consumer
// compares it with the value it last saw. So it must rise on every visible
// change and stay put when nothing changed, in every kind of table.

namespace
{

constexpr std::string_view kSchema = R"(
tables:
  - name: live
    columns:
      - {name: id, type: int}
  - name: unique_live
    columns:
      - {name: id, type: int}
    order_by: {columns: [id], unique: true}
  - name: file
    read_only: true
    columns:
      - {name: id, type: int}
    order_by: {columns: [id], unique: true}
  - name: array
    grid:
      rows: {name: r, type: int}
      columns: {name: c, type: int}
      cells: int
  - name: ranked
    pick: best
    inputs:
      - {name: t, type: double, range: 0..1}
    columns:
      - {name: id, type: int, key: true, domain: rows}
      - {name: window, type: band, input: t}
    order_by: {columns: [id]}
)";

enum table_id : uint32_t { Live, UniqueLive, File, Array, Ranked };

relation_store Store()
{
	auto s = SchemaFromYaml(kSchema, "generation.yaml");
	EXPECT_TRUE(s.has_value()) << (s ? std::string() : s.error().format());
	return relation_store(std::move(*s));
}

tl::expected<void, ErrorContext> InsertId(relation_store& store, uint32_t table, uint64_t owner, int64_t id)
{
	cell const values[] = {cell::Integer(id)};
	return store.Insert(insert_row{table, owner, values, {}, {}});
}

tl::expected<void, ErrorContext> LoadIds(relation_store& store, uint32_t table, uint64_t owner, std::vector<cell> const& ids)
{
	return store.Load(load_rows{table, owner, ids.size(), ids});
}

tl::expected<void, ErrorContext> LoadArray(relation_store& store, uint64_t owner, int64_t value)
{
	cell const keys[]  = {cell::Integer(1)};
	cell const cells[] = {cell::Integer(value)};
	return store.LoadGrid(load_grid{Array, owner, keys, keys, cells});
}

// The generation of every table, for comparing before and after.
std::vector<uint64_t> Generations(relation_store const& store)
{
	std::vector<uint64_t> out;
	for (uint32_t t = 0; t < 5; ++t)
	{
		out.push_back(store.Generation(t));
	}
	return out;
}

}

TEST(RelationGeneration, AnAcceptedInsertRaisesOnlyItsTable)
{
	relation_store store  = Store();
	auto           before = Generations(store);
	ASSERT_TRUE(InsertId(store, Live, 1, 5).has_value());
	auto after = Generations(store);
	EXPECT_GT(after[Live], before[Live]);
	for (uint32_t t = UniqueLive; t <= Ranked; ++t)
	{
		EXPECT_EQ(after[t], before[t]) << "table " << t;
	}
}

TEST(RelationGeneration, ARefusedInsertChangesNothing)
{
	relation_store store = Store();
	ASSERT_TRUE(InsertId(store, UniqueLive, 1, 5).has_value());
	auto before = Generations(store);
	EXPECT_FALSE(InsertId(store, UniqueLive, 2, 5).has_value()) << "a unique clash";
	EXPECT_FALSE(InsertId(store, File, 2, 5).has_value()) << "a read-only table";
	EXPECT_FALSE(InsertId(store, Array, 2, 5).has_value()) << "a grid";
	EXPECT_EQ(Generations(store), before);
}

TEST(RelationGeneration, ARetractThatRemovedRowsRaisesEveryTableItChanged)
{
	relation_store store = Store();
	ASSERT_TRUE(InsertId(store, Live, 1, 5).has_value());
	ASSERT_TRUE(InsertId(store, UniqueLive, 2, 6).has_value());
	ASSERT_TRUE(LoadIds(store, File, 1, {cell::Integer(7)}).has_value());
	ASSERT_TRUE(LoadArray(store, 1, 9).has_value());
	auto before = Generations(store);
	store.Retract(retract_owner{1});
	auto after = Generations(store);
	EXPECT_GT(after[Live], before[Live]);
	EXPECT_GT(after[File], before[File]);
	EXPECT_GT(after[Array], before[Array]);
	EXPECT_EQ(after[UniqueLive], before[UniqueLive]) << "owner 1 held nothing there";
	EXPECT_EQ(after[Ranked], before[Ranked]);
}

TEST(RelationGeneration, ARetractOfAnOwnerWithNoRowsChangesNothing)
{
	relation_store store = Store();
	ASSERT_TRUE(InsertId(store, Live, 1, 5).has_value());
	ASSERT_TRUE(LoadIds(store, File, 1, {cell::Integer(7)}).has_value());
	ASSERT_TRUE(LoadArray(store, 1, 9).has_value());
	auto before = Generations(store);
	store.Retract(retract_owner{99});
	EXPECT_EQ(Generations(store), before);
	store.Retract(retract_owner{1});
	auto emptied = Generations(store);
	store.Retract(retract_owner{1});
	EXPECT_EQ(Generations(store), emptied) << "the second retract has nothing left to remove";
}

TEST(RelationGeneration, AnAcceptedLoadPublishesAVersionAndRaisesItsGeneration)
{
	relation_store store = Store();
	auto           before = Generations(store);
	ASSERT_TRUE(LoadIds(store, File, 1, {cell::Integer(7)}).has_value());
	EXPECT_EQ(store.Generation(File), before[File] + 1);
	// The same rows again are still a new published version: a consumer holding
	// the old one has to learn it was replaced.
	ASSERT_TRUE(LoadIds(store, File, 1, {cell::Integer(7)}).has_value());
	EXPECT_EQ(store.Generation(File), before[File] + 2);
	auto version = store.Version(File);
	ASSERT_TRUE(version.has_value());
	EXPECT_EQ((*version)->Generation(), store.Generation(File));
}

TEST(RelationGeneration, ARefusedLoadChangesNothing)
{
	relation_store store = Store();
	ASSERT_TRUE(LoadIds(store, File, 1, {cell::Integer(7)}).has_value());
	auto before = Generations(store);
	EXPECT_FALSE(LoadIds(store, File, 2, {cell::Integer(7)}).has_value()) << "a unique clash with owner 1";
	EXPECT_FALSE(LoadIds(store, File, 2, {cell::Integer(1), cell::Integer(1)}).has_value()) << "a clash inside the load";
	EXPECT_FALSE(LoadIds(store, UniqueLive, 2, {cell::Integer(1)}).has_value()) << "a writable table";
	EXPECT_EQ(Generations(store), before);

	// A best table's totality refusal is the same: nothing published.
	std::vector<cell> hole = {cell::Integer(1), cell::Vector(0.0f, 0.5f)};
	EXPECT_FALSE(store.Load(load_rows{Ranked, 1, 1, hole}).has_value());
	EXPECT_EQ(Generations(store), before);
}

TEST(RelationGeneration, ALoadOfNoRowsForAnOwnerWithNoneChangesNothing)
{
	relation_store store = Store();
	ASSERT_TRUE(LoadIds(store, File, 1, {cell::Integer(7)}).has_value());
	auto before = Generations(store);
	ASSERT_TRUE(LoadIds(store, File, 2, {}).has_value());
	EXPECT_EQ(Generations(store), before);
	ASSERT_TRUE(LoadIds(store, File, 1, {}).has_value());
	EXPECT_GT(store.Generation(File), before[File]) << "emptying the owner's own rows is a change";
}

TEST(RelationGeneration, AGridRisesOnAcceptedLoadsAndStaysOnRefusedOnes)
{
	relation_store store = Store();
	ASSERT_TRUE(LoadArray(store, 1, 9).has_value());
	uint64_t const loaded = store.Generation(Array);
	EXPECT_GT(loaded, 0u);
	EXPECT_FALSE(LoadArray(store, 2, 9).has_value()) << "a grid has one contributor";
	EXPECT_EQ(store.Generation(Array), loaded);
	cell const keys[]  = {cell::Integer(1)};
	cell const wrong[] = {cell::Integer(1), cell::Integer(2)};
	EXPECT_FALSE(store.LoadGrid(load_grid{Array, 1, keys, keys, wrong}).has_value()) << "the cell count is wrong";
	EXPECT_EQ(store.Generation(Array), loaded);
	ASSERT_TRUE(LoadArray(store, 1, 10).has_value());
	EXPECT_GT(store.Generation(Array), loaded);
	auto grid = store.Grid(Array);
	ASSERT_TRUE(grid.has_value());
	EXPECT_EQ((*grid)->Generation(), store.Generation(Array));
}
