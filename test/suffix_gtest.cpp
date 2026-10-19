#include <gtest/gtest.h>
#include "tabular/relation.h"
#include "tabular/rows_csv.h"
#include "tabular/schema_yaml.h"
#include "cityhash/src/city.h"

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

int64_t Hash(std::string_view name)
{
	return static_cast<int64_t>(CityHash32(name.data(), name.size()));
}

// An entry table: the state keys the row, and the cells after it are its command.
constexpr std::string_view kSchema = R"(
tables:
  - name: enter
    read_only: true
    pick: best
    columns:
      - {name: mode, type: hash, key: true, domain: rows}
    order_by: {columns: [mode]}
    suffix: command
)";

relation_store Store()
{
	auto s = SchemaFromYaml(kSchema, "suffix.yaml");
	EXPECT_TRUE(s.has_value()) << (s ? std::string() : s.error().format());
	return relation_store(std::move(*s));
}

std::string Load(relation_store& store, std::string_view text, uint64_t owner = 1)
{
	auto loaded = LoadCsv(store, csv_rows{0, owner, text, "enter.csv"});
	return loaded ? std::string() : loaded.error().format();
}

std::string CommandOf(relation_store const& store, size_t row)
{
	auto command = store.CommandAt(0, row, 1);
	EXPECT_TRUE(command.has_value()) << (command ? std::string() : command.error().format());
	return command ? std::string(*command) : std::string();
}

}

// The suffix's cells are one sequence of commands, and the store keeps it as
// one list: the row's command is the list, whatever the cells were.
TEST(SuffixCsv, TheCellsAreOneListOfCommands)
{
	relation_store store = Store();
	ASSERT_EQ(Load(store,
		"mode, command\n"
		"eat, (anim chew), \"(message food \"\"eat\"\")\"\n"
		"idle, (anim stand)\n"
		"panic, (anim run), \"(message this \"\"calm\"\" :delay random(0.4, 1.0))\", (sound gasp)\n"), "");
	ASSERT_EQ(store.RowCount(0), 3u);
	// Rows sort by hash; find each by its key.
	for (size_t r = 0; r < 3; ++r)
	{
		int64_t const mode = store.Value(0, r, 0)->i;
		if (mode == Hash("eat"))
		{
			EXPECT_EQ(CommandOf(store, r), "((anim chew) (message food \"eat\"))");
		}
		else if (mode == Hash("idle"))
		{
			EXPECT_EQ(CommandOf(store, r), "((anim stand))");
		}
		else
		{
			EXPECT_EQ(mode, Hash("panic"));
			EXPECT_EQ(CommandOf(store, r), "((anim run) (message this \"calm\" :delay random(0.4, 1.0)) (sound gasp))")
				<< "three commands are a three-item list";
		}
	}
}

// Cell boundaries mean nothing inside an open parenthesis: authors spread a
// command over cells to keep the spreadsheet's columns neat.
TEST(SuffixCsv, ACommandAcrossTwoCellsReadsAsInOne)
{
	relation_store one = Store();
	relation_store two = Store();
	ASSERT_EQ(Load(one, "mode,command\neat,(emit sound),(play  particle)\n"), "");
	ASSERT_EQ(Load(two, "mode,command\neat,(emit,sound),(play,particle)\n"), "");
	EXPECT_EQ(CommandOf(two, 0), "((emit sound) (play particle))");
	EXPECT_EQ(CommandOf(one, 0), "((emit sound) (play  particle))") << "spacing inside a cell is the author's";
}

TEST(SuffixCsv, AnEmptySuffixIsTheEmptyList)
{
	relation_store store = Store();
	ASSERT_EQ(Load(store, "mode, command\neat, (anim chew)\nidle,\nwalk\nrun, ,  ,\n"), "");
	ASSERT_EQ(store.RowCount(0), 4u);
	for (size_t r = 0; r < 4; ++r)
	{
		if (store.Value(0, r, 0)->i != Hash("eat"))
		{
			EXPECT_EQ(CommandOf(store, r), "()") << "no suffix cell, a blank one or only padding: nothing to do";
		}
	}
}

// A cell written without its parentheses is one command, and a single word
// stays a word for the consumer to read (a clip, a :directive).
TEST(SuffixCsv, ACellOfSeveralItemsIsWrappedAndAWordStaysAWord)
{
	relation_store store = Store();
	ASSERT_EQ(Load(store, "mode,command\nfall,message this hard-land, idle, :lock, \"\"\"a b\"\"\", \"x y\"\n"), "");
	EXPECT_EQ(CommandOf(store, 0), "((message this hard-land) idle :lock \"a b\" (x y))")
		<< "a string is one item, so it is not wrapped; CSV's own quotes are not a string";
}

TEST(SuffixCsv, UnbalancedParenthesesAreRefusedNamingTheLine)
{
	relation_store store = Store();
	std::string const open = Load(store, "mode,command\nidle,(anim stand)\neat,(anim chew\n");
	EXPECT_TRUE(Mentions(open, "enter.csv:3")) << open;
	EXPECT_TRUE(Mentions(open, "'(' that is never closed")) << open;
	EXPECT_EQ(store.RowCount(0), 0u) << "all or none";
	std::string const closed = Load(store, "mode,command\neat,(anim chew))\n");
	EXPECT_TRUE(Mentions(closed, "enter.csv:2")) << closed;
	EXPECT_TRUE(Mentions(closed, "')' that closes nothing")) << closed;
}

TEST(SuffixCsv, AnUnterminatedStringIsRefusedNamingTheLine)
{
	relation_store store = Store();
	std::string const refused = Load(store, "mode,command\neat,\"(say \"\"hi)\"\n");
	EXPECT_TRUE(Mentions(refused, "enter.csv:2")) << refused;
	EXPECT_TRUE(Mentions(refused, "string that is never closed")) << refused;
	ASSERT_EQ(Load(store, "mode,command\neat,\"(say \"\"a \\\"\"quoted\\\"\" \\\\ word\"\")\"\n"), "")
		<< "\\\" and \\\\ are a string's escapes";
	EXPECT_EQ(CommandOf(store, 0), "((say \"a \\\"quoted\\\" \\\\ word\"))");
}

TEST(SuffixCsv, AHashInsideTheSuffixIsTextAndOnlyALineStartingWithOneIsAComment)
{
	relation_store store = Store();
	ASSERT_EQ(Load(store,
		"mode, command\n"
		"# a comment\n"
		"eat, (tag #1), (tag #2)\n"), "");
	ASSERT_EQ(store.RowCount(0), 1u);
	EXPECT_EQ(CommandOf(store, 0), "((tag #1) (tag #2))");
}

// The animation system's bridge table (spec Part 2, Bridge tables) as an
// ordinary table with a suffix: a clip is a bare word and a message is a
// (message ...) list, so quoting carries no meaning.
TEST(SuffixCsv, ABridgeRowIsOneListOfStepsAndMessages)
{
	constexpr std::string_view schema = R"(
tables:
  - name: bridge
    read_only: true
    columns:
      - {name: from,  type: hash}
      - {name: phase, type: hash}
      - {name: mode,  type: hash}
      - {name: to,    type: hash}
    suffix: steps
)";
	auto s = SchemaFromYaml(schema, "bridge.yaml");
	ASSERT_TRUE(s.has_value()) << s.error().format();
	relation_store store(std::move(*s));
	auto loaded = LoadCsv(store, csv_rows{0, 1,
		"from,    phase, mode, to,    steps\n"
		"falling, hard,  _,    idle,  message this hard-land, :lock, hard-land, :unlock, stand-from-hard-land, idle\n"
		"_,       _,     _,    eat,   HeadDown\n"
		"eat,     _,     _,    chew,  HeadUp,     \"(message this \"\"chewed\"\")\"\n",
		"bridge.csv"});
	ASSERT_TRUE(loaded.has_value()) << loaded.error().format();
	ASSERT_EQ(store.RowCount(0), 3u);
	EXPECT_EQ(*store.CommandAt(0, 0, 4), "((message this hard-land) :lock hard-land :unlock stand-from-hard-land idle)");
	EXPECT_EQ(*store.CommandAt(0, 1, 4), "(HeadDown)");
	EXPECT_EQ(*store.CommandAt(0, 2, 4), "(HeadUp (message this \"chewed\"))");
}

// The store checks the shape whichever surface writes the row.
TEST(SuffixStore, TheBuilderSurfaceIsShapeCheckedToo)
{
	relation_store store = Store();
	cell const                   values[]   = {cell::Integer(Hash("idle"))};
	std::string_view const       bad[]      = {"((anim stand)"};
	std::string const refused = [&]()
	{
		auto loaded = store.Load(load_rows{0, 1, 1, values, bad});
		return loaded ? std::string() : loaded.error().format();
	}();
	EXPECT_TRUE(Mentions(refused, "never closed")) << refused;
	std::string_view const good[] = {"((anim stand))"};
	auto loaded = store.Load(load_rows{0, 1, 1, values, good});
	ASSERT_TRUE(loaded.has_value()) << loaded.error().format();
	EXPECT_EQ(CommandOf(store, 0), "((anim stand))") << "the builder writes the list as the store keeps it";
}

// The header names the declared columns and then the suffix column, which is
// where the rest of every line begins. The reader needs it to know how many
// cells are columns.
TEST(SuffixCsv, TheHeaderMustEndWithTheSuffixColumn)
{
	relation_store store = Store();
	std::string const missing = Load(store, "mode\nidle, (a)\n");
	EXPECT_TRUE(Mentions(missing, "command")) << missing;
	EXPECT_TRUE(Mentions(missing, "enter.csv:1")) << missing;

	std::string const first = Load(store, "command, mode\n(a), idle\n");
	EXPECT_TRUE(Mentions(first, "last")) << first;
	EXPECT_TRUE(Mentions(first, "enter.csv:1")) << first;
}

TEST(SuffixCsv, AHeaderNamingAnExtraColumnIsRefused)
{
	relation_store store = Store();
	std::string const refused = Load(store, "mode, command, extra\nidle, (a), 5\n");
	EXPECT_TRUE(Mentions(refused, "enter.csv:1")) << refused;
	EXPECT_TRUE(Mentions(refused, "extra")) << refused;
	EXPECT_EQ(store.RowCount(0), 0u);
}

TEST(SuffixCsv, TheSuffixFollowsSeveralDeclaredColumnsInHeaderOrder)
{
	constexpr std::string_view schema = R"(
tables:
  - name: moves
    read_only: true
    columns:
      - {name: id, type: int}
      - {name: speed, type: double}
    suffix: steps
)";
	auto s = SchemaFromYaml(schema, "moves.yaml");
	ASSERT_TRUE(s.has_value()) << s.error().format();
	relation_store store(std::move(*s));
	auto loaded = LoadCsv(store, csv_rows{0, 1, "speed, id, steps\n1.5, 7, (go, fast), (stop)\n", "moves.csv"});
	ASSERT_TRUE(loaded.has_value()) << loaded.error().format();
	ASSERT_EQ(store.RowCount(0), 1u);
	EXPECT_EQ(store.Value(0, 0, 0)->i, 7);
	EXPECT_EQ(store.Value(0, 0, 1)->d, 1.5);
	EXPECT_EQ(*store.CommandAt(0, 0, 2), "((go fast) (stop))") << "the cell boundary inside (go fast) means nothing";
}

TEST(SuffixSchema, TheSuffixIsOneMoreCommandColumnAfterTheDeclaredOnes)
{
	auto s = SchemaFromYaml(kSchema, "suffix.yaml");
	ASSERT_TRUE(s.has_value()) << s.error().format();
	table_def const& def = s->tables()[0];
	ASSERT_EQ(def.columns.size(), 2u);
	EXPECT_EQ(def.columns[1].name, "command");
	EXPECT_EQ(def.columns[1].type, column_type::Command);
	EXPECT_TRUE(def.suffix);
}

TEST(SuffixSchema, TheBuilderTakesTheSameDeclaration)
{
	static add_column const columns[] = {{"id", column_type::Int}};
	create_table t{"table", columns};
	t.suffix = "rest";
	auto s = BuildSchema(std::span<create_table const>(&t, 1));
	ASSERT_TRUE(s.has_value()) << s.error().message;
	EXPECT_EQ(s->tables()[0].columns.size(), 2u);
	EXPECT_EQ(s->tables()[0].columns[1].name, "rest");
}

TEST(SuffixSchema, ASuffixNamingADeclaredColumnIsRefused)
{
	constexpr std::string_view schema = R"(
tables:
  - name: clash
    columns:
      - {name: command, type: int}
    suffix: command
)";
	auto s = SchemaFromYaml(schema, "clash.yaml");
	ASSERT_FALSE(s.has_value());
	EXPECT_TRUE(Mentions(s.error().format(), "command")) << s.error().format();
}

TEST(SuffixSchema, AnEmptySuffixNameIsRefused)
{
	constexpr std::string_view schema = R"(
tables:
  - name: blank
    columns:
      - {name: id, type: int}
    suffix: ""
)";
	auto s = SchemaFromYaml(schema, "blank.yaml");
	ASSERT_FALSE(s.has_value());
	EXPECT_TRUE(Mentions(s.error().format(), "suffix")) << s.error().format();
}

TEST(SuffixSchema, AGridOrAPivotedTableCannotHaveOne)
{
	constexpr std::string_view grid = R"(
tables:
  - name: array
    grid:
      rows: {name: r, type: int}
      columns: {name: c, type: int}
      cells: int
    suffix: command
)";
	auto g = SchemaFromYaml(grid, "grid.yaml");
	ASSERT_FALSE(g.has_value());
	EXPECT_TRUE(Mentions(g.error().format(), "suffix")) << g.error().format();

	constexpr std::string_view pivoted = R"(
tables:
  - name: lookup
    pick: best
    inputs:
      - {name: mode, type: flags}
    columns:
      - {name: label, type: int, key: true, domain: rows}
      - {name: req, type: flags, input: mode}
      - {name: clip, type: int}
    order_by: {columns: [label]}
    pivot: {headers: req, cells: clip}
    suffix: command
)";
	auto p = SchemaFromYaml(pivoted, "pivot.yaml");
	ASSERT_FALSE(p.has_value());
	EXPECT_TRUE(Mentions(p.error().format(), "suffix")) << p.error().format();
}

TEST(SuffixCsv, APivotedLoadIntoASuffixTableIsRefused)
{
	relation_store store = Store();
	csv_rows       command{0, 1, "mode, command\nidle, (a)\n", "enter.csv"};
	command.pivoted = true;
	auto loaded = LoadCsv(store, command);
	ASSERT_FALSE(loaded.has_value());
	EXPECT_EQ(store.RowCount(0), 0u);
}

TEST(SuffixCsv, ATableWithoutASuffixStillTakesACommaAsACellBoundary)
{
	constexpr std::string_view schema = R"(
tables:
  - name: plain
    read_only: true
    columns:
      - {name: id, type: int}
      - {name: on_spawn, type: command}
)";
	auto s = SchemaFromYaml(schema, "plain.yaml");
	ASSERT_TRUE(s.has_value()) << s.error().format();
	relation_store store(std::move(*s));
	auto loaded = LoadCsv(store, csv_rows{0, 1, "id, on_spawn\n1, (a), (b)\n", "plain.csv"});
	ASSERT_FALSE(loaded.has_value()) << "unquoted, the comma is a cell boundary and the extra cell a width error";
	EXPECT_TRUE(Mentions(loaded.error().format(), "plain.csv:2")) << loaded.error().format();
}
