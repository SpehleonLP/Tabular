#include <gtest/gtest.h>
#include "tabular/grid.h"
#include "tabular/rows_csv.h"
#include "tabular/schema_yaml.h"
#include "cityhash/src/city.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

using namespace tabular;

// RFC 4180 quoting, as a spreadsheet's CSV export writes it: tables are
// authored in a spreadsheet, which quotes any cell holding a comma, a double
// quote or a line break and doubles the quotes inside. Every front end reads
// it the same way (one reader).

namespace
{

constexpr std::string_view kSchema = R"(
tables:
  - name: spawn_points
    read_only: true
    columns:
      - {name: package, type: hash, key: true}
      - {name: section, type: int}
      - {name: note, type: string}
    order_by: {columns: [package, section], unique: true}
  - name: moods
    read_only: true
    pick: best
    inputs:
      - {name: mode, type: flags}
    columns:
      - {name: label, type: int, key: true, domain: rows}
      - {name: req, type: flags, input: mode}
      - {name: mood, type: string}
    order_by: {columns: [label]}
    pivot: {headers: req, cells: mood}
  - name: distances
    read_only: true
    grid: {rows: {name: from, type: hash}, columns: {name: to, type: hash}, cells: int}
)";

relation_store Store()
{
	auto s = SchemaFromYaml(kSchema, "schema.yaml");
	EXPECT_TRUE(s.has_value()) << s.error().format();
	return relation_store(std::move(*s));
}

int64_t Hash(std::string_view name)
{
	return static_cast<int64_t>(CityHash32(name.data(), name.size()));
}

bool Mentions(std::string const& message, std::string_view text)
{
	return message.find(text) != std::string::npos;
}

std::string Load(relation_store& store, std::string_view text, uint32_t table = 0)
{
	csv_rows load{table, 1, text, "spawn.csv"};
	load.pivoted = table == 1;
	auto loaded  = LoadCsv(store, load);
	return loaded ? std::string() : loaded.error().format();
}

std::string_view Note(relation_store const& store, size_t row)
{
	auto text = store.StringAt(0, row, 2);
	return text ? std::string_view(*text) : std::string_view("(no string)");
}

}

TEST(CsvQuoting, AQuotedCellHoldsCommasAndDoubledQuotes)
{
	relation_store store = Store();
	ASSERT_EQ(Load(store,
		"package,section,note\n"
		"meadow,1,\"says \"\"hi, there\"\"\"\n"), "");
	EXPECT_EQ(Note(store, 0), "says \"hi, there\"");
}

TEST(CsvQuoting, AQuotedKeyHashesItsTextWithoutTheQuotes)
{
	relation_store store = Store();
	ASSERT_EQ(Load(store, "package,section,note\n\"meadow\",1,x\n"), "");
	EXPECT_EQ(store.Value(0, 0, 0)->i, Hash("meadow"));
}

TEST(CsvQuoting, SpacesOutsideTheQuotesAreTrimmedAndInsideKept)
{
	relation_store store = Store();
	ASSERT_EQ(Load(store, "package,section,note\nmeadow,1,  \"  two  \"  \n"), "");
	EXPECT_EQ(Note(store, 0), "  two  ");
}

TEST(CsvQuoting, AnUnterminatedQuoteNamesTheLineItOpenedOn)
{
	relation_store store = Store();
	std::string const refused = Load(store, "package,section,note\nmeadow,1,x\nmeadow,2,\"never\nclosed\n");
	EXPECT_TRUE(Mentions(refused, "never closed")) << refused;
	EXPECT_TRUE(Mentions(refused, "spawn.csv:3")) << refused;
}

TEST(CsvQuoting, AQuoteInsideAnUnquotedCellIsRefused)
{
	relation_store store = Store();
	std::string const refused = Load(store, "package,section,note\nmeadow,1,say \"hi\"\n");
	EXPECT_TRUE(Mentions(refused, "not quoted")) << refused;
	EXPECT_TRUE(Mentions(refused, "spawn.csv:2")) << refused;
}

TEST(CsvQuoting, TextAfterAClosingQuoteIsRefused)
{
	relation_store store = Store();
	std::string const refused = Load(store, "package,section,note\nmeadow,1,\"hi\"there\n");
	EXPECT_TRUE(Mentions(refused, "closing quote")) << refused;
	EXPECT_TRUE(Mentions(refused, "spawn.csv:2")) << refused;
}

// A line break inside quotes is text, so the row runs over two lines, and a
// refusal on the row after it names that row's own first line.
TEST(CsvQuoting, AQuotedCellMaySpanLinesAndLaterRowsKeepTheirLines)
{
	relation_store store = Store();
	ASSERT_EQ(Load(store, "package,section,note\nmeadow,1,\"first\nsecond\"\nmeadow,2,x\n"), "");
	EXPECT_EQ(Note(store, 0), "first\nsecond");

	std::string const refused = Load(store, "package,section,note\nmeadow,1,\"first\nsecond\"\nmeadow,zero,x\n");
	EXPECT_TRUE(Mentions(refused, "spawn.csv:4")) << refused;
}

// Only a row whose first unquoted character is '#' is a comment: a quoted
// "#1" is a cell, and a spreadsheet's comment row comes with its padding.
TEST(CsvQuoting, OnlyAnUnquotedHashStartsAComment)
{
	relation_store store = Store();
	ASSERT_EQ(Load(store,
		"package,section,note\n"
		"# a note, with commas,,\n"
		"\"#1\",1,x\n"), "");
	ASSERT_EQ(store.RowCount(0), 1u);
	EXPECT_EQ(store.Value(0, 0, 0)->i, Hash("#1"));
}

// The same table saved by LibreOffice (LF, quoting only where needed) and by
// Excel's "CSV UTF-8" (a BOM, CRLF, every row padded to the widest with empty
// cells, and an empty spreadsheet row written as its separators).
TEST(CsvQuoting, LibreOfficeAndExcelExportsReadTheSame)
{
	constexpr std::string_view kLibreOffice =
		"package,section,note\n"
		"meadow,1,\"a, b\"\n"
		"meadow,2,\"say \"\"x\"\"\"\n";
	constexpr std::string_view kExcel =
		"\xEF\xBB\xBFpackage,section,note,,\r\n"
		"meadow,1,\"a, b\",,\r\n"
		",,,,\r\n"
		"meadow,2,\"say \"\"x\"\"\",,\r\n";
	relation_store libre = Store();
	relation_store excel = Store();
	ASSERT_EQ(Load(libre, kLibreOffice), "");
	ASSERT_EQ(Load(excel, kExcel), "");
	ASSERT_EQ(libre.RowCount(0), 2u);
	ASSERT_EQ(excel.RowCount(0), 2u);
	for (size_t r = 0; r < 2; ++r)
	{
		EXPECT_EQ(Note(libre, r), Note(excel, r));
		EXPECT_EQ(libre.Value(0, r, 1)->i, excel.Value(0, r, 1)->i);
	}
	EXPECT_EQ(Note(excel, 0), "a, b");
	EXPECT_EQ(Note(excel, 1), "say \"x\"");
}

TEST(CsvQuoting, PaddingPastTheHeaderMustBeEmpty)
{
	relation_store store = Store();
	std::string const refused = Load(store, "package,section,note\nmeadow,1,x,extra\n");
	EXPECT_TRUE(Mentions(refused, "row has 4 cells, but the header has 3")) << refused;
}

TEST(CsvQuoting, ThePivotedAndGridFrontEndsReadQuotes)
{
	relation_store store = Store();
	ASSERT_EQ(Load(store, "label,None,\"Calm\"\n\"1\",\"calm, mostly\",\"say \"\"purr\"\"\"\n", 1), "");
	ASSERT_EQ(store.RowCount(1), 2u);
	std::vector<std::string> moods;
	for (size_t r = 0; r < 2; ++r)
	{
		moods.emplace_back(*store.StringAt(1, r, 2));
	}
	std::sort(moods.begin(), moods.end());
	EXPECT_EQ(moods, (std::vector<std::string>{"calm, mostly", "say \"purr\""}));

	auto loaded = LoadGridCsv(store, csv_grid{2, 1, "from,\"a\",b\n\"a\",0,\"3\"\nb,3,0\n", "grid.csv"});
	ASSERT_TRUE(loaded.has_value()) << loaded.error().format();
	auto grid = store.Grid(2);
	ASSERT_TRUE(grid.has_value()) << grid.error().format();
	auto at = (*grid)->At(cell::Integer(Hash("a")), cell::Integer(Hash("b")));
	ASSERT_TRUE(at.has_value()) << at.error().format();
	EXPECT_EQ(at->i, 3);
}
