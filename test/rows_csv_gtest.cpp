#include <gtest/gtest.h>
#include "tabular/rows_csv.h"
#include "tabular/schema_yaml.h"
#include "cityhash/src/city.h"
#include "probe_closure.h"

#include <string>
#include <string_view>
#include <vector>

using namespace tabular;
using namespace tabular_test;

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

// Every blank cell is refused except a condition's, which is "no condition".
// A command was the type this rule missed: an empty one loaded as a row that
// does nothing, which no author meant.
TEST(RowsCsv, ABlankCommandCellIsRefused)
{
	relation_store store = Store();
	std::string error = Load(store, "package,section,mask,x,on_spawn\na,1,0,0,\n");
	EXPECT_TRUE(Mentions(error, "'on_spawn'")) << error;
	EXPECT_TRUE(Mentions(error, "is empty")) << error;
	EXPECT_TRUE(Mentions(error, "spawn.csv:2")) << error;
	EXPECT_EQ(store.RowCount(0), 0u);
	EXPECT_TRUE(Mentions(Load(store, "package,section,mask,x,on_spawn\na,1,0,0,   \n"), "'on_spawn'"))
		<< "a cell of spaces trims to empty";
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
	EXPECT_TRUE(Mentions(loaded.error().format(), "condition function")) << loaded.error().format();
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
	for (std::string_view bad : {"1..", "..2", "1..2..3", "1;2", "a..b", "1...2", "1.0...2"})
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

TEST(RowsCsv, AStringCellIsItsTrimmedText)
{
	auto s = SchemaFromYaml(
		"tables:\n  - name: places\n    read_only: true\n    columns:\n"
		"      - {name: name, type: string, key: true}\n      - {name: id, type: int}\n", "places.yaml");
	ASSERT_TRUE(s.has_value()) << s.error().format();
	relation_store store(std::move(*s));
	auto loaded = LoadCsv(store, csv_rows{0, 1, "id,name\n1,  north meadow \n", "places.csv"});
	ASSERT_TRUE(loaded.has_value()) << loaded.error().format();
	EXPECT_EQ(std::string_view(*store.StringAt(0, 0, 0)), "north meadow");
	EXPECT_EQ(*store.Find(0, 0, std::string_view("north meadow")), std::vector<size_t>{0});
}

TEST(RowsCsv, AnEmptyStringCellIsRefused)
{
	auto s = SchemaFromYaml("tables:\n  - name: places\n    read_only: true\n    columns:\n"
		"      - {name: name, type: string}\n      - {name: id, type: int}\n", "places.yaml");
	ASSERT_TRUE(s.has_value()) << s.error().format();
	relation_store store(std::move(*s));
	auto loaded = LoadCsv(store, csv_rows{0, 1, "name,id\n,1\n", "places.csv"});
	std::string error = loaded ? std::string() : loaded.error().format();
	EXPECT_TRUE(Mentions(error, "'name'")) << error;
}

// Padding around a cell is the editor's, not the string's; spaces inside it are the string's.
// A cell of only whitespace trims to empty and is refused like an empty one.
TEST(RowsCsv, StringCellsAreTrimmedButKeepInnerSpaces)
{
	auto s = SchemaFromYaml("tables:\n  - name: places\n    read_only: true\n    columns:\n"
		"      - {name: name, type: string, key: true}\n      - {name: id, type: int}\n", "places.yaml");
	ASSERT_TRUE(s.has_value()) << s.error().format();
	relation_store store(std::move(*s));
	auto loaded = LoadCsv(store, csv_rows{0, 1, "name,id\n  forest  edge \t,1\n", "places.csv"});
	ASSERT_TRUE(loaded.has_value()) << loaded.error().format();
	EXPECT_EQ(std::string_view(*store.StringAt(0, 0, 0)), "forest  edge");
	EXPECT_EQ(*store.Find(0, 0, std::string_view("forest  edge")), std::vector<size_t>{0});

	relation_store blank(SchemaFromYaml("tables:\n  - name: places\n    read_only: true\n    columns:\n"
		"      - {name: name, type: string}\n      - {name: id, type: int}\n", "places.yaml").value());
	auto refused = LoadCsv(blank, csv_rows{0, 1, "name,id\n \t ,1\n", "places.csv"});
	std::string error = refused ? std::string() : refused.error().format();
	EXPECT_TRUE(Mentions(error, "'name'")) << error;
	EXPECT_TRUE(Mentions(error, "places.csv:2")) << error;
}

namespace
{

constexpr std::string_view kRuleSchema = R"(
tables:
  - name: rules
    read_only: true
    columns:
      - {name: priority, type: int}
      - {name: when, type: condition}
)";

relation_store RuleStore()
{
	auto s = SchemaFromYaml(kRuleSchema, "rules.yaml");
	EXPECT_TRUE(s.has_value()) << s.error().format();
	return relation_store(std::move(*s));
}

}

TEST(RowsCsv, ConditionCellsGoThroughTheFunction)
{
	int released = 0;
	std::vector<std::string> seen;
	{
		relation_store store = RuleStore();
		csv_rows rows{0, 1, "priority,when\n1, hp < 5 \n2,always\n", "rules.csv"};
		rows.condition = [&](std::string_view text) -> tl::expected<closure, ErrorContext>
		{
			seen.emplace_back(text);
			return MakeProbe(static_cast<int64_t>(seen.size()) * 10, &released);
		};
		auto loaded = LoadCsv(store, rows);
		ASSERT_TRUE(loaded.has_value()) << loaded.error().format();
		int64_t fifteen = 15;
		EXPECT_TRUE(*store.Holds(0, 0, 1, &fifteen));
		EXPECT_FALSE(*store.Holds(0, 1, 1, &fifteen));
	}
	EXPECT_EQ(seen, (std::vector<std::string>{"hp < 5", "always"}));
	EXPECT_EQ(released, 2);
}

TEST(RowsCsv, AFailingConditionNamesTheLineAndReleasesEarlierCells)
{
	int released = 0;
	relation_store store = RuleStore();
	csv_rows rows{0, 1, "priority,when\n1,fine\n2,broken\n", "rules.csv"};
	rows.condition = [&](std::string_view text) -> tl::expected<closure, ErrorContext>
	{
		if (text == "broken")
		{
			return tl::make_unexpected(ErrorContext("does not compile"));
		}
		return MakeProbe(1, &released);
	};
	auto loaded = LoadCsv(store, rows);
	ASSERT_FALSE(loaded.has_value());
	std::string const error = loaded.error().format();
	EXPECT_TRUE(Mentions(error, "rules.csv:3")) << error;
	EXPECT_TRUE(Mentions(error, "does not compile")) << error;
	EXPECT_EQ(released, 1);
	EXPECT_EQ(store.RowCount(0), 0u);
}

TEST(RowsCsv, AConditionTableWithNoFunctionIsRefused)
{
	relation_store store = RuleStore();
	auto loaded = LoadCsv(store, csv_rows{0, 1, "priority,when\n1,fine\n", "rules.csv"});
	ASSERT_FALSE(loaded.has_value());
	EXPECT_TRUE(Mentions(loaded.error().format(), "condition function")) << loaded.error().format();
}

// The function is Tabular's part in compiling conditions, and a table whose
// condition cells are all blank has nothing to compile.
TEST(RowsCsv, ABlankConditionColumnNeedsNoFunction)
{
	relation_store store = RuleStore();
	auto loaded = LoadCsv(store, csv_rows{0, 1, "priority,when\n1,\n2,   \n", "rules.csv"});
	ASSERT_TRUE(loaded.has_value()) << loaded.error().format();
	EXPECT_EQ(store.RowCount(0), 2u);
	auto refused = LoadCsv(store, csv_rows{0, 1, "priority,when\n1,\n2,fine\n", "rules.csv"});
	ASSERT_FALSE(refused.has_value());
	EXPECT_TRUE(Mentions(refused.error().format(), "condition function")) << refused.error().format();
	EXPECT_TRUE(Mentions(refused.error().format(), "'when'")) << refused.error().format();
	EXPECT_TRUE(Mentions(refused.error().format(), "rules.csv:3")) << refused.error().format();
	EXPECT_EQ(store.RowCount(0), 2u) << "the refused load changes nothing";
}

TEST(RowsCsv, ABlankConditionCellIsNoConditionAndNeverReachesTheFunction)
{
	int released = 0;
	std::vector<std::string> seen;
	relation_store store = RuleStore();
	csv_rows rows{0, 1, "priority,when\n1,fine\n2,\n3,   \n", "rules.csv"};
	rows.condition = [&](std::string_view text) -> tl::expected<closure, ErrorContext>
	{
		seen.emplace_back(text);
		return MakeProbe(100, &released);
	};
	auto loaded = LoadCsv(store, rows);
	ASSERT_TRUE(loaded.has_value()) << loaded.error().format();
	EXPECT_EQ(seen, (std::vector<std::string>{"fine"}));
	int64_t one = 1;
	auto holds = [&](size_t row)
	{
		auto h = store.Holds(0, row, 1, &one);
		EXPECT_TRUE(h.has_value()) << (h ? std::string() : h.error().format());
		return h && *h;
	};
	EXPECT_FALSE(holds(0));
	EXPECT_TRUE(holds(1));
	EXPECT_TRUE(holds(2)) << "cells are trimmed, so spaces are blank";
}

TEST(RowsCsv, AHostClosureWithReleaseButNoFunctionNamesItsLine)
{
	int released = 0;
	relation_store store = RuleStore();
	csv_rows rows{0, 1, "priority,when\n1,fine\n2,odd\n", "rules.csv"};
	rows.condition = [&](std::string_view text) -> tl::expected<closure, ErrorContext>
	{
		return text == "odd" ? closure{nullptr, nullptr, [](void*) {}} : MakeProbe(1, &released);
	};
	auto loaded = LoadCsv(store, rows);
	ASSERT_FALSE(loaded.has_value());
	EXPECT_TRUE(Mentions(loaded.error().format(), "rules.csv:3")) << loaded.error().format();
	EXPECT_EQ(released, 1);
}

TEST(RowsCsv, TwoNamesWithOneHashAreRefusedNamingBothLines)
{
	ASSERT_EQ(Hash("n26963"), Hash("n50336")) << "the pinned collision no longer collides";
	relation_store store = Store();
	std::string error = Load(store,
		"package,section,mask,x,on_spawn\nn26963,1,0,0,(a)\nother,2,0,0,(b)\nn50336,3,0,0,(c)\n");
	EXPECT_TRUE(Mentions(error, "'n50336'")) << error;
	EXPECT_TRUE(Mentions(error, "'n26963'")) << error;
	EXPECT_TRUE(Mentions(error, "line 2")) << error;
	EXPECT_TRUE(Mentions(error, "spawn.csv:4")) << error;
	EXPECT_EQ(store.RowCount(0), 0u);
}

TEST(RowsCsv, TheSameNameOnManyLinesIsNoCollision)
{
	relation_store store = Store();
	EXPECT_EQ(Load(store, "package,section,mask,x,on_spawn\nbase,1,0,0,(a)\nbase,2,0,0,(b)\n"), "");
}

TEST(RowsCsv, AHashCollisionAcrossColumnsIsRefused)
{
	auto s = SchemaFromYaml(R"(
tables:
  - name: links
    read_only: true
    columns:
      - {name: from, type: hash}
      - {name: to, type: hash}
)", "links.yaml");
	ASSERT_TRUE(s.has_value()) << s.error().format();
	relation_store store(std::move(*s));
	auto loaded = LoadCsv(store, csv_rows{0, 1, "from,to\nn26963,a\nb,n50336\n", "links.csv"});
	ASSERT_FALSE(loaded.has_value());
	EXPECT_TRUE(Mentions(loaded.error().format(), "links.csv:3")) << loaded.error().format();
}

TEST(RowsCsv, ADeadRowNamesTheLineThatCoversIt)
{
	auto s = SchemaFromYaml(R"(
tables:
  - name: reaction
    read_only: true
    pick: best
    inputs:
      - {name: energy, type: double, range: 0..1}
    columns:
      - {name: stimulus, type: hash, key: true}
      - {name: energy, type: band, input: energy}
      - {name: reaction, type: string}
    order_by: {columns: [stimulus]}
)", "reaction.yaml");
	ASSERT_TRUE(s.has_value()) << s.error().format();
	relation_store store(std::move(*s));
	auto loaded = LoadCsv(store, csv_rows{0, 1,
		"stimulus,energy,reaction\npredator,0..1,flee\npredator,0.2..1,fight\n", "reaction.csv"});
	ASSERT_FALSE(loaded.has_value());
	std::string const error = loaded.error().format();
	EXPECT_TRUE(Mentions(error, "reaction.csv:3")) << error;
	EXPECT_TRUE(Mentions(error, "covered by line 2")) << error;
	EXPECT_FALSE(Mentions(error, "same key as")) << error;
}

// A run another owner's bare retract stranded names no loaded row, so the
// refusal carries the file's label and no line number that is not the author's.
TEST(RowsCsv, AStrandedRunIsRefusedNamingTheFileAndNoLine)
{
	auto s = SchemaFromYaml(R"(
tables:
  - name: lookup
    read_only: true
    pick: best
    inputs:
      - {name: mode, type: flags}
    columns:
      - {name: label, type: int, key: true}
      - {name: req, type: flags, input: mode}
      - {name: clip, type: string}
    order_by: {columns: [label]}
)", "lookup.yaml");
	ASSERT_TRUE(s.has_value()) << s.error().format();
	relation_store store(std::move(*s));
	ASSERT_TRUE(LoadCsv(store, csv_rows{0, 1, "label,req,clip\n1,0,base\n", "one.csv"}).has_value());
	ASSERT_TRUE(LoadCsv(store, csv_rows{0, 3, "label,req,clip\n1,1,flagged\n", "three.csv"}).has_value());
	store.Retract(retract_owner{1});   // key 1 now has no row with no flags
	auto loaded = LoadCsv(store, csv_rows{0, 2, "label,req,clip\n2,0,other\n", "two.csv"});
	ASSERT_FALSE(loaded.has_value());
	std::string const error = loaded.error().format();
	EXPECT_TRUE(Mentions(error, "two.csv")) << error;
	for (char digit = '0'; digit <= '9'; ++digit)
	{
		EXPECT_FALSE(Mentions(error, std::string("two.csv:") + digit)) << "no loaded row is at fault: " << error;
	}
	EXPECT_TRUE(Mentions(error, "owner 3's row with key (1), flags 1")) << error;
}
