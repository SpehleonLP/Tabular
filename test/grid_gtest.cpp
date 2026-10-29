#include <gtest/gtest.h>
#include "tabular/grid.h"
#include "tabular/pick.h"
#include "tabular/relation.h"
#include "tabular/rows_csv.h"
#include "tabular/document.h"

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
	t.grid      = &shape;
	t.read_only = true;
	auto s = BuildSchema(std::span<create_table const>(&t, 1));
	EXPECT_TRUE(s.has_value()) << (s ? std::string() : s.error().message);
	return relation_store(std::move(*s));
}

// A wrong answer must fail one test, not abort the run: tl::expected asserts
// on a bad access, and asserts are live.
template<typename T>
std::string ErrorOf(tl::expected<T, ErrorContext> const& e)
{
	return e ? std::string() : e.error().format();
}

template<typename T>
std::string RefusalOf(tl::expected<T, ErrorContext> const& e)
{
	return e ? std::string("(accepted)") : e.error().format();
}

// The value cell at two keys, or INT64_MIN after recording the failure.
int64_t IntAt(GridVersion const& grid, int64_t row, int64_t column)
{
	auto c = grid.At(cell::Integer(row), cell::Integer(column));
	EXPECT_TRUE(c.has_value()) << ErrorOf(c);
	return c ? c->i : INT64_MIN;
}

std::string TextAt(GridVersion const& grid, int64_t row, int64_t column)
{
	auto c = grid.StringAt(cell::Integer(row), cell::Integer(column));
	EXPECT_TRUE(c.has_value()) << ErrorOf(c);
	return c ? std::string(std::string_view(*c)) : std::string("(none)");
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
	EXPECT_EQ(IntAt(**grid, 20, 3), 23);
	auto r = (*grid)->RowOf(cell::Integer(10));
	auto c = (*grid)->ColumnOf(cell::Integer(2));
	ASSERT_TRUE(r.has_value() && c.has_value());
	EXPECT_EQ((*grid)->At(*r, *c).i, 12);
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
	auto grid = store.Grid(0);
	ASSERT_TRUE(grid.has_value()) << ErrorOf(grid);
	EXPECT_EQ(IntAt(**grid, -3, -1), -30);
	EXPECT_EQ(IntAt(**grid, 5, -1), 50);
	EXPECT_EQ(IntAt(**grid, 0, -1), 0);
}

TEST(Grid, StringAndCommandGridsHoldText)
{
	for (column_type type : {column_type::String, column_type::Command})
	{
		relation_store store = GridStore(type);
		auto rows    = Ints({1});
		auto columns = Ints({1, 2});
		// A command cell is a list of commands, as every command column holds.
		std::string_view const first  = type == column_type::String ? "anim/walk.glb" : "((play walk))";
		std::string_view const second = type == column_type::String ? "(play run)" : "((play run))";
		string_in const cells[] = {first, second};
		ASSERT_TRUE(store.LoadGrid(load_grid{0, 1, rows, columns, {}, cells}).has_value());
		auto grid = store.Grid(0);
		ASSERT_TRUE(grid.has_value()) << ErrorOf(grid);
		ASSERT_EQ((*grid)->Columns(), 2u);
		EXPECT_EQ(TextAt(**grid, 1, 1), first);
		EXPECT_EQ(std::string_view((*grid)->StringAt(0, 1)), second);
		EXPECT_FALSE((*grid)->At(cell::Integer(1), cell::Integer(1)).has_value()) << "a text grid has no values";
	}
}

// Every command column is exactly one list, a grid's cells too: the store
// checks whatever surface wrote them, and the CSV reads a cell as a declared
// column's is read.
TEST(Grid, ACommandGridsCellsAreEachOneList)
{
	relation_store store = GridStore(column_type::Command);
	auto rows    = Ints({1});
	auto columns = Ints({1, 2});
	string_in const two[] = {std::string_view("((play walk))"), std::string_view("(a) (b)")};
	std::string const refused = RefusalOf(store.LoadGrid(load_grid{0, 1, rows, columns, {}, two}));
	EXPECT_TRUE(Mentions(refused, "is not one list")) << refused;
	EXPECT_TRUE(Mentions(refused, "grid 'g'")) << refused;
	string_in const word[] = {std::string_view("((play walk))"), std::string_view("idle")};
	EXPECT_TRUE(Mentions(RefusalOf(store.LoadGrid(load_grid{0, 1, rows, columns, {}, word})), "is not one list"))
		<< "a bare word is the CSV's to wrap, not the store's";

	ASSERT_EQ(RefusalOf(LoadGridCsv(store, csv_grid{0, 1, "creature,1,2\n1,play walk,()\n", "grid.csv"})), "(accepted)");
	auto grid = store.Grid(0);
	ASSERT_TRUE(grid.has_value()) << ErrorOf(grid);
	EXPECT_EQ(TextAt(**grid, 1, 1), "((play walk))") << "a cell of several items is one command";
	EXPECT_EQ(TextAt(**grid, 1, 2), "()");
	std::string const split = RefusalOf(LoadGridCsv(store, csv_grid{0, 1, "creature,1,2\n1,a) (b,()\n", "grid.csv"}));
	EXPECT_TRUE(Mentions(split, "grid.csv:2")) << split;
	EXPECT_TRUE(Mentions(split, "not whole")) << split;
}

TEST(Grid, AHeldVersionKeepsItsCells)
{
	relation_store store = GridStore(column_type::Int);
	auto rows    = Ints({1});
	auto columns = Ints({1});
	auto first   = Ints({100});
	auto second  = Ints({200});
	ASSERT_TRUE(store.LoadGrid(load_grid{0, 1, rows, columns, first}).has_value());
	auto held = store.Grid(0);
	ASSERT_TRUE(held.has_value()) << ErrorOf(held);
	ASSERT_TRUE(store.LoadGrid(load_grid{0, 1, rows, columns, second}).has_value());
	auto now = store.Grid(0);
	ASSERT_TRUE(now.has_value()) << ErrorOf(now);
	EXPECT_EQ(IntAt(**held, 1, 1), 100);
	EXPECT_EQ(IntAt(**now, 1, 1), 200);
	EXPECT_GT((*now)->Generation(), (*held)->Generation());
	EXPECT_EQ(store.Generation(0), (*now)->Generation()) << "the store reports the grid's generation";
	EXPECT_EQ(store.RowCount(0), 1u) << "and its rows";
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
	EXPECT_EQ(store.RowCount(0), 0u) << "refusals change nothing";
}

// A grid is one contributor's whole array, so a second owner's load would
// silently replace the first's, and the first owner's Retract would then do
// nothing. Every row table keeps other owners' rows; a grid refuses instead.
TEST(Grid, ASecondOwnersLoadIsRefusedNamingBothOwners)
{
	relation_store store = GridStore(column_type::Int);
	auto rows  = Ints({1});
	auto cols  = Ints({1});
	auto first = Ints({100});
	auto other = Ints({200});
	ASSERT_EQ(RefusalOf(store.LoadGrid(load_grid{0, 7, rows, cols, first})), "(accepted)");
	std::string const refused = RefusalOf(store.LoadGrid(load_grid{0, 8, rows, cols, other}));
	EXPECT_TRUE(Mentions(refused, "owner 7")) << refused;
	EXPECT_TRUE(Mentions(refused, "owner 8")) << refused;
	auto grid = store.Grid(0);
	ASSERT_TRUE(grid.has_value()) << ErrorOf(grid);
	EXPECT_EQ(IntAt(**grid, 1, 1), 100) << "a refused load changes nothing";
	EXPECT_EQ(RefusalOf(store.LoadGrid(load_grid{0, 7, rows, cols, other})), "(accepted)") << "the owner reloads its own";
	store.Retract(retract_owner{7});
	EXPECT_EQ(RefusalOf(store.LoadGrid(load_grid{0, 8, rows, cols, other})), "(accepted)")
		<< "an emptied grid has no contributor to protect";
}

// OwnerOf indexed the row layout, which a grid leaves empty: out-of-bounds.
TEST(Grid, OwnerOfAGridIsTheGridsOwner)
{
	relation_store store = GridStore(column_type::Int);
	auto rows  = Ints({1, 2});
	auto cols  = Ints({1});
	auto cells = Ints({100, 200});
	ASSERT_EQ(RefusalOf(store.LoadGrid(load_grid{0, 7, rows, cols, cells})), "(accepted)");
	EXPECT_EQ(store.OwnerOf(0, 0), 7u);
	EXPECT_EQ(store.OwnerOf(0, 1), 7u);
}

TEST(Grid, RetractEmptiesTheOwnersGrid)
{
	relation_store store = GridStore(column_type::Int);
	auto rows = Ints({1});
	auto cols = Ints({1});
	auto cell1 = Ints({9});
	ASSERT_TRUE(store.LoadGrid(load_grid{0, 7, rows, cols, cell1}).has_value());
	store.Retract(retract_owner{8});
	EXPECT_EQ(store.RowCount(0), 1u) << "another owner's retract leaves it";
	store.Retract(retract_owner{7});
	EXPECT_EQ(store.RowCount(0), 0u);
	auto grid = store.Grid(0);
	ASSERT_TRUE(grid.has_value()) << ErrorOf(grid);
	EXPECT_FALSE((*grid)->At(cell::Integer(1), cell::Integer(1)).has_value());
}

TEST(Grid, RetractEmptiesAGridOfColumnsAndNoRows)
{
	relation_store store = GridStore(column_type::Int);
	auto cols = Ints({1, 2});
	ASSERT_TRUE(store.LoadGrid(load_grid{0, 7, {}, cols, {}}).has_value());
	auto before = store.Grid(0);
	ASSERT_TRUE(before.has_value()) << ErrorOf(before);
	ASSERT_EQ((*before)->Columns(), 2u);
	store.Retract(retract_owner{7});
	auto after = store.Grid(0);
	ASSERT_TRUE(after.has_value()) << ErrorOf(after);
	EXPECT_EQ((*after)->Columns(), 0u);
	EXPECT_NE((*after)->Owner(), 7u);
}

TEST(Grid, TableCallsRefuseAGridAndGridCallsRefuseATable)
{
	grid_shape const shape{{"r", column_type::Int}, {"c", column_type::Int}, column_type::Int};
	add_column const columns[] = {{"x", column_type::Int}};
	create_table tables[2] = {{"g"}, {"t", columns, nullptr, true}};
	tables[0].grid      = &shape;
	tables[0].read_only = true;
	auto s = BuildSchema(tables);
	ASSERT_TRUE(s.has_value()) << s.error().message;
	relation_store store(std::move(*s));
	cell const one[] = {cell::Integer(1)};
	EXPECT_TRUE(Mentions(RefusalOf(store.Insert(insert_row{0, 1, one})), "is a grid"));
	EXPECT_TRUE(Mentions(RefusalOf(store.Load(load_rows{0, 1, 1, one})), "is a grid"));
	EXPECT_TRUE(Mentions(RefusalOf(store.Version(0)), "is a grid"));
	EXPECT_TRUE(Mentions(RefusalOf(PickBest(store, 0, pick_query{})), "is a grid"));
	EXPECT_TRUE(Mentions(RefusalOf(store.Value(0, 0, 0)), "is a grid"));
	EXPECT_TRUE(Mentions(RefusalOf(store.FindOrdered(0, one)), "is a grid"));
	EXPECT_TRUE(Mentions(RefusalOf(store.Find(0, 0, cell::Integer(1))), "is a grid"));
	EXPECT_TRUE(Mentions(RefusalOf(store.Grid(1)), "not a grid"));
	EXPECT_TRUE(Mentions(RefusalOf(store.LoadGrid(load_grid{1, 1, one, one, one})), "not a grid"));
}

TEST(Grid, AGridIsReadOnlyWhateverIsWritten)
{
	grid_shape const shape{{"r", column_type::Int}, {"c", column_type::Int}, column_type::Int};
	create_table t{"g"};
	t.grid = &shape;   // read_only left false
	auto built = BuildSchema(std::span<create_table const>(&t, 1));
	ASSERT_FALSE(built.has_value());
	EXPECT_TRUE(Mentions(built.error().message, "read-only")) << built.error().message;

	// The file format has no read_only to state false, so a grid declared there is
	// read-only by construction.
	auto stated = ReadSchema(R"(
[grid g (rows r int) (columns c int) (cells int)]
)", "g.tabular");
	EXPECT_TRUE(stated.has_value()) << (stated ? std::string() : stated.error().format());
}

TEST(Grid, AGridIsDeclaredWithAxesAndOneCellType)
{
	auto build = [](grid_shape const& shape, std::span<add_column const> columns = {})
	{
		create_table t{"g", columns};
		t.grid      = &shape;
		t.read_only = true;
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
[grid clip_by_kind (rows creature hash) (columns kind int) (cells string)]
)";

relation_store GridStoreFrom(std::string_view source)
{
	auto s = ReadSchema(source, "grid.tabular");
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
	relation_store store = GridStoreFrom(kClipByKind);
	ASSERT_TRUE(store.definition().tables()[0].is_grid);
	ASSERT_EQ(GridError(store, "creature,1,2\ngrendel, anim/g_walk.glb ,anim/g_run.glb\nnorn,anim/n_walk.glb,anim/n_run.glb\n"), "");
	auto grid = store.Grid(0);
	ASSERT_TRUE(grid.has_value()) << ErrorOf(grid);
	ASSERT_EQ((*grid)->Rows(), 2u);
	// Row indices are line order; row keys are CityHash32 of the names, so read by index.
	auto run = (*grid)->ColumnOf(cell::Integer(2));
	ASSERT_TRUE(run.has_value()) << ErrorOf(run);
	EXPECT_EQ(std::string_view((*grid)->StringAt(0, *run)), "anim/g_run.glb");
	EXPECT_EQ(std::string_view((*grid)->StringAt(1, *run)), "anim/n_run.glb");
	EXPECT_EQ(std::string_view((*grid)->StringAt(0, 0)), "anim/g_walk.glb") << "cells are trimmed";
}

TEST(GridCsv, BlankCellsAreErrorsNamingLineAndHeader)
{
	relation_store store = GridStoreFrom(kClipByKind);
	std::string error = GridError(store, "creature,1,2\ngrendel,a,b\nnorn,c,\n");
	EXPECT_TRUE(Mentions(error, "'2'")) << error;
	EXPECT_TRUE(Mentions(error, "grid.csv:3")) << error;
}

TEST(GridCsv, RaggedLinesDuplicatesAndBadCellsAreRefused)
{
	relation_store store = GridStoreFrom(kClipByKind);
	EXPECT_TRUE(Mentions(GridError(store, "creature,1,2\ngrendel,a\n"), "grid.csv:2"));
	std::string dupRow = GridError(store, "creature,1\ngrendel,a\nnorn,b\ngrendel,c\n");
	EXPECT_TRUE(Mentions(dupRow, "grid.csv:4")) << dupRow;
	EXPECT_TRUE(Mentions(dupRow, "line 2")) << dupRow;
	std::string dupColumn = GridError(store, "creature,1,1\ngrendel,a,b\n");
	EXPECT_TRUE(Mentions(dupColumn, "'1'")) << dupColumn;
	EXPECT_TRUE(Mentions(GridError(store, "creature,x\ngrendel,a\n"), "'kind'")) << "an int axis key that is not an int";

	relation_store ints = GridStoreFrom(R"(
[grid g (rows r int) (columns c int) (cells double)]
)");
	EXPECT_TRUE(Mentions(GridError(ints, "r,1\n1,fast\n"), "grid.csv:2"));
	EXPECT_EQ(GridError(ints, "r,-1,2\n-5,0.5,1e3\n"), "");
	auto grid = ints.Grid(0);
	ASSERT_TRUE(grid.has_value()) << ErrorOf(grid);
	auto big = (*grid)->At(cell::Integer(-5), cell::Integer(2));
	ASSERT_TRUE(big.has_value()) << ErrorOf(big);
	EXPECT_EQ(big->d, 1000.0);
}

TEST(GridCsv, AGridIsNotLoadedAsRowsNorRowsAsAGrid)
{
	relation_store store = GridStoreFrom(kClipByKind);
	EXPECT_FALSE(LoadCsv(store, csv_rows{0, 1, "creature\nx\n", "g.csv"}).has_value());
	relation_store rows = GridStoreFrom(R"(
[table t (column x int) (pick none)]
)");
	EXPECT_TRUE(Mentions(GridError(rows, "x,1\n1,2\n"), "not a grid"));
}

TEST(GridCsv, AxisKeysThatCollideAreRefused)
{
	auto s = ReadSchema(R"(
[grid g (rows r hash) (columns c hash) (cells int)]
)", "g.tabular");
	ASSERT_TRUE(s.has_value()) << s.error().format();
	relation_store store(std::move(*s));
	auto across = LoadGridCsv(store, csv_grid{0, 1, ",n26963,x\nn50336,1,2\n", "g.csv"});
	ASSERT_FALSE(across.has_value()) << "a row key colliding with a column key";
	std::string const error = across.error().format();
	EXPECT_TRUE(error.find("g.csv:2") != std::string::npos) << error;
	EXPECT_TRUE(error.find("'n50336' has the same hash as 'n26963' on line 1") != std::string::npos) << error;

	auto s2 = ReadSchema(R"(
[grid g (rows r int) (columns c int) (cells hash)]
)", "g.tabular");
	ASSERT_TRUE(s2.has_value()) << s2.error().format();
	relation_store cells(std::move(*s2));
	auto inCells = LoadGridCsv(cells, csv_grid{0, 1, ",1,2\n1,n26963,a\n2,b,n50336\n", "g.csv"});
	ASSERT_FALSE(inCells.has_value()) << "two hash cells colliding";
	EXPECT_TRUE(inCells.error().format().find("g.csv:3") != std::string::npos) << inCells.error().format();
}
