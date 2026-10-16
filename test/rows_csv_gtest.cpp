#include <gtest/gtest.h>
#include "tabular/rows_csv.h"
#include "tabular/schema_yaml.h"
#include "cityhash/src/city.h"

#include <string>
#include <string_view>

using namespace tabular;

static bool Mentions(std::string const& message, std::string_view text)
{
	return message.find(text) != std::string::npos;
}

namespace
{

constexpr std::string_view kSchema = R"(
tables:
  - name: spawn_points
    read_only: true
    columns:
      - {name: package, type: hash, key: true}
      - {name: section, type: int}
      - {name: mask, type: flags}
      - {name: x, type: double}
      - {name: on_spawn, type: command}
    order_by: {columns: [package, section], unique: true}
  - name: rules
    columns:
      - {name: when, type: condition}
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

std::string Load(relation_store& store, std::string_view text, uint64_t owner = 1, uint32_t firstLine = 1)
{
	auto loaded = LoadCsv(store, csv_rows{0, owner, text, "spawn.csv", firstLine});
	return loaded ? std::string() : loaded.error().format();
}

}

TEST(RowsCsv, ReadsEveryTypeInHeaderOrder)
{
	relation_store store = Store();
	ASSERT_EQ(Load(store,
		"x, on_spawn, package, section, mask\n"
		"1.5, (spawn grazer), meadow, 2, 0x10\n"
		"-0.25, (spawn moth), meadow, -1, 3\n"), "");
	ASSERT_EQ(store.RowCount(0), 2u);
	// Sorted by (package, section): section -1 first.
	EXPECT_EQ(store.Value(0, 0, 0)->i, Hash("meadow"));
	EXPECT_EQ(store.Value(0, 0, 1)->i, -1);
	EXPECT_EQ(store.Value(0, 0, 2)->i, 3);
	EXPECT_EQ(store.Value(0, 0, 3)->d, -0.25);
	EXPECT_EQ(*store.CommandAt(0, 0, 4), "(spawn moth)");
	EXPECT_EQ(store.Value(0, 1, 2)->i, 16);
	EXPECT_EQ(store.Value(0, 1, 3)->d, 1.5);
}

TEST(RowsCsv, ReloadReplacesTheOwnersRows)
{
	relation_store store = Store();
	std::string_view header = "package,section,mask,x,on_spawn\n";
	ASSERT_EQ(Load(store, std::string(header) + "a,1,0,0,()\na,2,0,0,()\n"), "");
	uint64_t before = store.Generation(0);
	ASSERT_EQ(Load(store, std::string(header) + "a,3,0,0,()\n"), "");
	EXPECT_EQ(store.RowCount(0), 1u);
	EXPECT_EQ(store.Value(0, 0, 1)->i, 3);
	EXPECT_EQ(store.Generation(0), before + 1);
	EXPECT_EQ(store.Find(0, 0, cell::Integer(Hash("a")))->size(), 1u);
}

TEST(RowsCsv, BadCellsNameTheLineAndColumn)
{
	relation_store store = Store();
	std::string_view header = "package,section,mask,x,on_spawn\n";
	struct bad { std::string_view row; std::string_view mentions; };
	bad const cases[] = {
		{"a,12abc,0,0,()", "'section'"},
		{"a,1,0,1.5.2,()", "'x'"},
		{"a,1,0x100000000,0,()", "'mask'"},
		{"a,1,-1,0,()", "'mask'"},
		{"a,,0,0,()", "'section'"},
		{",1,0,0,()", "'package'"},
	};
	for (bad const& b : cases)
	{
		std::string error = Load(store, std::string(header) + "# a comment line\n" + std::string(b.row) + "\n", 1, 10);
		EXPECT_TRUE(Mentions(error, b.mentions)) << b.row << " -> " << error;
		EXPECT_TRUE(Mentions(error, "spawn.csv:12")) << b.row << " -> " << error;
		EXPECT_EQ(store.RowCount(0), 0u) << b.row;
	}
}

TEST(RowsCsv, RaggedRowsNameTheirLine)
{
	relation_store store = Store();
	std::string error = Load(store, "package,section,mask,x,on_spawn\na,1,0,0,()\n\na,2,0\n");
	EXPECT_TRUE(Mentions(error, "spawn.csv:4")) << error;
	EXPECT_TRUE(Mentions(error, "3 cells")) << error;
	EXPECT_EQ(store.RowCount(0), 0u);
}

TEST(RowsCsv, HeaderMustNameEveryColumnOnce)
{
	relation_store store = Store();
	EXPECT_TRUE(Mentions(Load(store, "package,section,mask,x\n"), "'on_spawn'"));
	EXPECT_TRUE(Mentions(Load(store, "package,section,mask,x,on_spawn,extra\n"), "'extra'"));
	EXPECT_TRUE(Mentions(Load(store, "package,package,section,mask,x,on_spawn\n"), "'package'"));
	EXPECT_TRUE(Mentions(Load(store, ""), "header"));
}

TEST(RowsCsv, AHeaderWithNoRowsEmptiesTheOwner)
{
	relation_store store = Store();
	ASSERT_EQ(Load(store, "package,section,mask,x,on_spawn\na,1,0,0,()\n"), "");
	ASSERT_EQ(Load(store, "package,section,mask,x,on_spawn\n"), "");
	EXPECT_EQ(store.RowCount(0), 0u);
}

TEST(RowsCsv, UniqueClashNamesTheFile)
{
	relation_store store = Store();
	std::string error = Load(store, "package,section,mask,x,on_spawn\na,1,0,0,()\na,1,0,0,()\n");
	EXPECT_TRUE(Mentions(error, "spawn.csv")) << error;
	EXPECT_TRUE(Mentions(error, "unique")) << error;
}

TEST(RowsCsv, TablesWithConditionsAreRefused)
{
	relation_store store = Store();
	auto loaded = LoadCsv(store, csv_rows{1, 1, "when\nx\n", "rules.csv"});
	ASSERT_FALSE(loaded.has_value());
	EXPECT_TRUE(Mentions(loaded.error().format(), "'when'")) << loaded.error().format();
}

TEST(RowsCsv, AUniqueClashNamesBothLines)
{
	relation_store store = Store();
	std::string error = Load(store, "package,section,mask,x,on_spawn\nmeadow,3,0,0,()\nmoor,1,0,0,()\nmeadow,3,0,0,()\n");
	EXPECT_TRUE(Mentions(error, "spawn.csv:4")) << error;
	EXPECT_TRUE(Mentions(error, "line 2")) << error;
}

TEST(RowsCsv, AByteOrderMarkIsNotPartOfTheHeader)
{
	relation_store store = Store();
	ASSERT_EQ(Load(store, "\xEF\xBB\xBFpackage,section,mask,x,on_spawn\na,1,0,0,()\n"), "");
	EXPECT_EQ(store.RowCount(0), 1u);
}

TEST(RowsCsv, AHashIsOfTheBytesAsWrittenLikeNameHash)
{
	// "ﬁeld" with the U+FB01 ligature: Unicode normalisation would turn it into
	// "field" and hash a different name from the one a script's literal hashes.
	relation_store store = Store();
	std::string_view const name = "\xEF\xAC\x81" "eld";
	ASSERT_EQ(Load(store, "package,section,mask,x,on_spawn\n" + std::string(name) + ",1,0,0,()\n"), "");
	EXPECT_EQ(store.Value(0, 0, 0)->i, Hash(name));
}

namespace
{

constexpr std::string_view kVecSchema = R"(
tables:
  - name: marks
    read_only: true
    columns:
      - {name: id, type: int}
      - {name: at, type: vec2}
)";

relation_store VecStore()
{
	auto s = SchemaFromYaml(kVecSchema, "marks.yaml");
	EXPECT_TRUE(s.has_value()) << s.error().format();
	return relation_store(std::move(*s));
}

std::string LoadMarks(relation_store& store, std::string_view text)
{
	auto loaded = LoadCsv(store, csv_rows{0, 1, text, "marks.csv"});
	return loaded ? std::string() : loaded.error().format();
}

}

TEST(RowsCsv, Vec2IsWrittenAsARange)
{
	relation_store store = VecStore();
	ASSERT_EQ(LoadMarks(store, "id,at\n1,0.25..0.5\n"), "");
	EXPECT_EQ(store.Value(0, 0, 1)->v[0], 0.25f);
	EXPECT_EQ(store.Value(0, 0, 1)->v[1], 0.5f);
}

TEST(RowsCsv, MalformedVec2IsRefusedNamingTheColumn)
{
	for (std::string_view bad : {"1..", "..2", "1..2..3", "1;2", "a..b"})
	{
		relation_store store = VecStore();
		std::string error = LoadMarks(store, std::string("id,at\n1,") + std::string(bad) + "\n");
		EXPECT_TRUE(Mentions(error, "'at'")) << bad << ": " << error;
		EXPECT_TRUE(Mentions(error, "marks.csv:2")) << bad << ": " << error;
	}
}

TEST(RowsCsv, Vec2AcceptsSignsAndExponentsAndRefusesInnerSpaces)
{
	relation_store store = VecStore();
	ASSERT_EQ(LoadMarks(store, "id,at\n1,-1..-0.5\n2,1e3..2\n"), "");
	EXPECT_EQ(store.Value(0, 0, 1)->v[1], -0.5f);
	EXPECT_EQ(store.Value(0, 1, 1)->v[0], 1000.0f);
	relation_store spaced = VecStore();
	EXPECT_TRUE(Mentions(LoadMarks(spaced, "id,at\n1,1 .. 2\n"), "'at'"));
}
