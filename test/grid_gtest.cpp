#include <gtest/gtest.h>
#include "tabular/grid.h"
#include "tabular/pick.h"
#include "tabular/relation.h"
#include "tabular/rows_csv.h"
#include "tabular/schema_yaml.h"

#include <string>
#include <string_view>
#include <vector>

using namespace tabular;

static bool Mentions(std::string const& message, std::string_view text)
{
	return message.find(text) != std::string::npos;
}

namespace
{

relation_store GridStore(column_type cells, column_type axes = column_type::Int)
{
	grid_shape const   shape{{"creature", axes}, {"kind", axes}, cells};
	create_table       t{"g"};
	t.grid = &shape;
	auto s = BuildSchema(std::span<create_table const>(&t, 1));
	EXPECT_TRUE(s.has_value()) << (s ? std::string() : s.error().message);
	return relation_store(std::move(*s));
}

std::vector<cell> Ints(std::vector<int64_t> const& v)
{
	std::vector<cell> out;
	for (int64_t x : v)
	{
		out.push_back(cell::Integer(x));
	}
	return out;
}

}

TEST(Grid, LooksUpByKeysAndByIndices)
{
	relation_store store = GridStore(column_type::Int);
	auto rows    = Ints({10, 20});
	auto columns = Ints({1, 2, 3});
	auto cells   = Ints({11, 12, 13, 21, 22, 23});
	ASSERT_TRUE(store.LoadGrid(load_grid{0, 1, rows, columns, cells}).has_value());
	auto grid = store.Grid(0);
	ASSERT_TRUE(grid.has_value()) << grid.error().message;
	EXPECT_EQ((*grid)->Rows(), 2u);
	EXPECT_EQ((*grid)->Columns(), 3u);
	EXPECT_EQ((*grid)->At(cell::Integer(20), cell::Integer(3))->i, 23);
	uint32_t r = *(*grid)->RowOf(cell::Integer(10));
	uint32_t c = *(*grid)->ColumnOf(cell::Integer(2));
	EXPECT_EQ((*grid)->At(r, c).i, 12);
	EXPECT_FALSE((*grid)->At(cell::Integer(30), cell::Integer(1)).has_value()) << "an unknown key is an error";
	EXPECT_FALSE((*grid)->StringAt(cell::Integer(10), cell::Integer(1)).has_value()) << "a value grid has no strings";
}

TEST(Grid, NegativeIntKeysAreFound)
{
	relation_store store = GridStore(column_type::Int);
	auto rows    = Ints({5, -3, 0});
	auto columns = Ints({-1});
	auto cells   = Ints({50, -30, 0});
	ASSERT_TRUE(store.LoadGrid(load_grid{0, 1, rows, columns, cells}).has_value());
	auto grid = *store.Grid(0);
	EXPECT_EQ(grid->At(cell::Integer(-3), cell::Integer(-1))->i, -30);
	EXPECT_EQ(grid->At(cell::Integer(5), cell::Integer(-1))->i, 50);
	EXPECT_EQ(grid->At(cell::Integer(0), cell::Integer(-1))->i, 0);
}

TEST(Grid, StringAndCommandGridsHoldText)
{
	for (column_type type : {column_type::String, column_type::Command})
	{
		relation_store store = GridStore(type);
		auto rows    = Ints({1});
		auto columns = Ints({1, 2});
		string_in const cells[] = {std::string_view("anim/walk.glb"), std::string_view("(play run)")};
		ASSERT_TRUE(store.LoadGrid(load_grid{0, 1, rows, columns, {}, cells}).has_value());
		auto grid = *store.Grid(0);
		EXPECT_EQ(std::string_view(*grid->StringAt(cell::Integer(1), cell::Integer(1))), "anim/walk.glb");
		EXPECT_EQ(std::string_view(grid->StringAt(0, 1)), "(play run)");
		EXPECT_FALSE(grid->At(cell::Integer(1), cell::Integer(1)).has_value()) << "a text grid has no values";
	}
}

TEST(Grid, AHeldVersionKeepsItsCells)
{
	relation_store store = GridStore(column_type::Int);
	auto rows    = Ints({1});
	auto columns = Ints({1});
	auto first   = Ints({100});
	auto second  = Ints({200});
	ASSERT_TRUE(store.LoadGrid(load_grid{0, 1, rows, columns, first}).has_value());
	auto held = *store.Grid(0);
	ASSERT_TRUE(store.LoadGrid(load_grid{0, 1, rows, columns, second}).has_value());
	EXPECT_EQ(held->At(0, 0).i, 100);
	EXPECT_EQ((*store.Grid(0))->At(0, 0).i, 200);
	EXPECT_GT((*store.Grid(0))->Generation(), held->Generation());
}

TEST(Grid, DuplicateKeysAndWrongSizesAreRefused)
{
	relation_store store = GridStore(column_type::Int);
	auto rows    = Ints({1, 2, 1});
	auto columns = Ints({1});
	auto cells   = Ints({1, 2, 3});
	grid_refusal refusal;
	auto dupRow = store.LoadGrid(load_grid{0, 1, rows, columns, cells, {}, &refusal});
	ASSERT_FALSE(dupRow.has_value());
	EXPECT_EQ(refusal.row, 2u);
	EXPECT_EQ(refusal.earlier_row, 0u);

	auto oneRow  = Ints({1});
	auto dupCols = Ints({4, 4});
	auto two     = Ints({1, 2});
	grid_refusal colRefusal;
	ASSERT_FALSE(store.LoadGrid(load_grid{0, 1, oneRow, dupCols, two, {}, &colRefusal}).has_value());
	EXPECT_EQ(colRefusal.column, 1u);
	EXPECT_EQ(colRefusal.earlier_column, 0u);

	auto three = Ints({1, 2, 3});
	auto wrong = store.LoadGrid(load_grid{0, 1, oneRow, two, three});
	ASSERT_FALSE(wrong.has_value());
	EXPECT_TRUE(Mentions(wrong.error().message, "3 cells")) << wrong.error().message;
	EXPECT_EQ((*store.Grid(0))->Rows(), 0u) << "refusals change nothing";
}

TEST(Grid, RetractEmptiesTheOwnersGrid)
{
	relation_store store = GridStore(column_type::Int);
	auto rows = Ints({1});
	auto cols = Ints({1});
	auto cell1 = Ints({9});
	ASSERT_TRUE(store.LoadGrid(load_grid{0, 7, rows, cols, cell1}).has_value());
	store.Retract(retract_owner{8});
	EXPECT_EQ((*store.Grid(0))->Rows(), 1u) << "another owner's retract leaves it";
	store.Retract(retract_owner{7});
	EXPECT_EQ((*store.Grid(0))->Rows(), 0u);
	EXPECT_FALSE((*store.Grid(0))->At(cell::Integer(1), cell::Integer(1)).has_value());
}

TEST(Grid, TableCallsRefuseAGridAndGridCallsRefuseATable)
{
	grid_shape const shape{{"r", column_type::Int}, {"c", column_type::Int}, column_type::Int};
	add_column const columns[] = {{"x", column_type::Int}};
	create_table tables[2] = {{"g"}, {"t", columns, nullptr, true}};
	tables[0].grid = &shape;
	auto s = BuildSchema(tables);
	ASSERT_TRUE(s.has_value()) << s.error().message;
	relation_store store(std::move(*s));
	cell const one[] = {cell::Integer(1)};
	EXPECT_TRUE(Mentions(store.Insert(insert_row{0, 1, one}).error().message, "is a grid"));
	EXPECT_TRUE(Mentions(store.Load(load_rows{0, 1, 1, one}).error().message, "is a grid"));
	EXPECT_TRUE(Mentions(store.Version(0).error().message, "is a grid"));
	EXPECT_FALSE(PickBest(store, 0, pick_query{}).has_value());
	EXPECT_TRUE(Mentions(store.Grid(1).error().message, "not a grid"));
	EXPECT_TRUE(Mentions(store.LoadGrid(load_grid{1, 1, one, one, one}).error().message, "not a grid"));
}

TEST(Grid, AGridIsDeclaredWithAxesAndOneCellType)
{
	auto build = [](grid_shape const& shape, std::span<add_column const> columns = {})
	{
		create_table t{"g", columns};
		t.grid = &shape;
		return BuildSchema(std::span<create_table const>(&t, 1));
	};
	EXPECT_FALSE(build({{"r", column_type::Double}, {"c", column_type::Int}, column_type::Int}).has_value());
	EXPECT_FALSE(build({{"r", column_type::Int}, {"c", column_type::Int}, column_type::Condition}).has_value());
	EXPECT_FALSE(build({{"r", column_type::Int}, {"c", column_type::Int}, column_type::Band}).has_value());
	EXPECT_FALSE(build({{"", column_type::Int}, {"c", column_type::Int}, column_type::Int}).has_value());
	add_column const extra[] = {{"x", column_type::Int}};
	EXPECT_FALSE(build({{"r", column_type::Int}, {"c", column_type::Int}, column_type::Int}, extra).has_value());
	auto ok = build({{"r", column_type::Hash}, {"c", column_type::Int}, column_type::String});
	ASSERT_TRUE(ok.has_value()) << ok.error().message;
	EXPECT_TRUE(ok->tables()[0].is_grid);
	EXPECT_TRUE(ok->tables()[0].read_only);
}

namespace
{

constexpr std::string_view kClipByKind = R"(
tables:
  - name: clip_by_kind
    grid: {rows: {name: creature, type: hash}, columns: {name: kind, type: int}, cells: string}
)";

relation_store YamlGridStore(std::string_view yaml)
{
	auto s = SchemaFromYaml(yaml, "grid.yaml");
	EXPECT_TRUE(s.has_value()) << (s ? std::string() : s.error().format());
	return relation_store(std::move(*s));
}

std::string GridError(relation_store& store, std::string_view text)
{
	auto r = LoadGridCsv(store, csv_grid{0, 1, text, "grid.csv"});
	return r ? std::string() : r.error().format();
}

}

TEST(GridCsv, LoadsADenseStringGridFromYamlAndCsv)
{
	relation_store store = YamlGridStore(kClipByKind);
	ASSERT_TRUE(store.definition().tables()[0].is_grid);
	ASSERT_EQ(GridError(store, "creature,1,2\ngrendel, anim/g_walk.glb ,anim/g_run.glb\nnorn,anim/n_walk.glb,anim/n_run.glb\n"), "");
	auto grid = *store.Grid(0);
	EXPECT_EQ(grid->Rows(), 2u);
	// Row indices are line order; row keys are CityHash32 of the names, so read by index.
	uint32_t const run = *grid->ColumnOf(cell::Integer(2));
	EXPECT_EQ(std::string_view(grid->StringAt(0, run)), "anim/g_run.glb");
	EXPECT_EQ(std::string_view(grid->StringAt(1, run)), "anim/n_run.glb");
	EXPECT_EQ(std::string_view(grid->StringAt(0, 0)), "anim/g_walk.glb") << "cells are trimmed";
}

TEST(GridCsv, BlankCellsAreErrorsNamingLineAndHeader)
{
	relation_store store = YamlGridStore(kClipByKind);
	std::string error = GridError(store, "creature,1,2\ngrendel,a,b\nnorn,c,\n");
	EXPECT_TRUE(Mentions(error, "'2'")) << error;
	EXPECT_TRUE(Mentions(error, "grid.csv:3")) << error;
}

TEST(GridCsv, RaggedLinesDuplicatesAndBadCellsAreRefused)
{
	relation_store store = YamlGridStore(kClipByKind);
	EXPECT_TRUE(Mentions(GridError(store, "creature,1,2\ngrendel,a\n"), "grid.csv:2"));
	std::string dupRow = GridError(store, "creature,1\ngrendel,a\nnorn,b\ngrendel,c\n");
	EXPECT_TRUE(Mentions(dupRow, "grid.csv:4")) << dupRow;
	EXPECT_TRUE(Mentions(dupRow, "line 2")) << dupRow;
	std::string dupColumn = GridError(store, "creature,1,1\ngrendel,a,b\n");
	EXPECT_TRUE(Mentions(dupColumn, "'1'")) << dupColumn;
	EXPECT_TRUE(Mentions(GridError(store, "creature,x\ngrendel,a\n"), "'kind'")) << "an int axis key that is not an int";

	relation_store ints = YamlGridStore(R"(
tables:
  - name: g
    grid: {rows: {name: r, type: int}, columns: {name: c, type: int}, cells: double}
)");
	EXPECT_TRUE(Mentions(GridError(ints, "r,1\n1,fast\n"), "grid.csv:2"));
	EXPECT_EQ(GridError(ints, "r,-1,2\n-5,0.5,1e3\n"), "");
	EXPECT_EQ((*ints.Grid(0))->At(cell::Integer(-5), cell::Integer(2))->d, 1000.0);
}

TEST(GridCsv, AGridIsNotLoadedAsRowsNorRowsAsAGrid)
{
	relation_store store = YamlGridStore(kClipByKind);
	EXPECT_FALSE(LoadCsv(store, csv_rows{0, 1, "creature\nx\n", "g.csv"}).has_value());
	relation_store rows = YamlGridStore(R"(
tables:
  - name: t
    read_only: true
    columns:
      - {name: x, type: int}
)");
	EXPECT_TRUE(Mentions(GridError(rows, "x,1\n1,2\n"), "not a grid"));
}
