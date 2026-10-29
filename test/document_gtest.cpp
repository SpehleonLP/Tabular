#include <gtest/gtest.h>
#include "tabular/document.h"
#include "tabular/grid.h"
#include "tabular/machine.h"
#include "tabular/pick.h"
#include "cityhash/src/city.h"

#include <functional>
#include <sstream>
#include <string>
#include <string_view>

using namespace tabular;

static bool Mentions(std::string const& message, std::string_view text)
{
	return message.find(text) != std::string::npos;
}

namespace
{

constexpr std::string_view kSpawnPoints = R"(
[table spawn_points
  (pick none)
  (order section package unique)]
>package, section, x: double, y: double
[table rules
  (pick none)]
when, then: command, mask: flags, count: int
)";

// The context's first layer is where the file says the fault is.
std::string Where(ErrorContext const& error)
{
	return error.context.empty() ? std::string() : error.context.front();
}

}

TEST(Document, BuildsEveryTableColumnAndOrder)
{
	auto s = ReadSchema(kSpawnPoints, "spawn.tabular");
	ASSERT_TRUE(s.has_value()) << s.error().format();
	ASSERT_EQ(s->tables().size(), 2u);

	table_def const& spawn = s->tables()[0];
	EXPECT_EQ(spawn.name, "spawn_points");
	EXPECT_TRUE(spawn.read_only);
	ASSERT_EQ(spawn.columns.size(), 4u);
	EXPECT_EQ(spawn.columns[0].type, column_type::Hash);
	EXPECT_TRUE(spawn.columns[0].key);
	EXPECT_FALSE(spawn.columns[1].key);
	EXPECT_EQ(spawn.columns[3].type, column_type::Double);
	ASSERT_EQ(spawn.order.size(), 2u);
	EXPECT_EQ(spawn.order[0], 1u);   // by name: section is column 1
	EXPECT_EQ(spawn.order[1], 0u);
	EXPECT_TRUE(spawn.unique);

	table_def const& rules = s->tables()[1];
	EXPECT_TRUE(rules.read_only) << "a file's tables are all read-only";
	EXPECT_EQ(rules.order.size(), 0u);
	EXPECT_EQ(rules.columns[0].type, column_type::Condition);
	EXPECT_EQ(rules.columns[1].type, column_type::Command);
	EXPECT_EQ(rules.columns[2].type, column_type::Flags);
	EXPECT_EQ(rules.columns[3].type, column_type::Int);
}

TEST(Document, AnOrderNamingAMissingColumnIsRefused)
{
	auto s = ReadSchema("[table t (pick none) (order b)]\na: int\n", "t.tabular");
	ASSERT_FALSE(s.has_value());
	EXPECT_TRUE(Mentions(s.error().format(), "'b'")) << s.error().format();
	EXPECT_TRUE(Mentions(s.error().format(), "t.tabular")) << s.error().format();
}

TEST(Document, AnUnknownTypeIsRefused)
{
	auto s = ReadSchema("[table t (pick none)]\na: vec3\n", "t.tabular");
	ASSERT_FALSE(s.has_value());
	EXPECT_TRUE(Mentions(s.error().format(), "vec3")) << s.error().format();
}

TEST(Document, AMisspelledOptionIsRefusedNotIgnored)
{
	auto s = ReadSchema("[table t (pick none) (uniqe a)]\na: int\n", "t.tabular");
	ASSERT_FALSE(s.has_value());
	EXPECT_TRUE(Mentions(s.error().format(), "uniqe")) << s.error().format();

	// Read as a column name, a misspelled flag is a missing column, not a quiet
	// non-unique table.
	auto flag = ReadSchema("[table t (pick none) (order a uniqe)]\na: int\n", "t.tabular");
	ASSERT_FALSE(flag.has_value());
	EXPECT_TRUE(Mentions(flag.error().format(), "uniqe")) << flag.error().format();
}

TEST(Document, MissingRequiredPartsAreRefused)
{
	EXPECT_FALSE(ReadSchema("[table (pick none)]\na: int\n", "t.tabular").has_value());
	EXPECT_FALSE(ReadSchema("[table]", "t.tabular").has_value());
	EXPECT_FALSE(ReadSchema("[table \"\" (pick none)]\na: int\n", "t.tabular").has_value());
	EXPECT_FALSE(ReadSchema("[table t (pick none)]", "t.tabular").has_value());
	EXPECT_FALSE(ReadSchema("[table t (pick none)]\na:\n", "t.tabular").has_value());
	EXPECT_FALSE(ReadSchema("[table t (pick none)]\n>\n", "t.tabular").has_value());
	EXPECT_FALSE(ReadSchema("[something t]", "t.tabular").has_value());
}

TEST(Document, CoreRefusalsCarryTheLabel)
{
	// Two tables named t: BuildSchema refuses it, and the label says which file.
	auto s = ReadSchema(R"(
[table t (pick none)]
a: int
[table t (pick none)]
a: int
)", "dup.tabular");
	ASSERT_FALSE(s.has_value());
	EXPECT_TRUE(Mentions(s.error().format(), "dup.tabular")) << s.error().format();
	EXPECT_TRUE(Mentions(s.error().format(), "'t'")) << s.error().format();
}

TEST(Document, MalformedBracketsAreRefused)
{
	auto s = ReadSchema("[table t (input a double\n", "bad.tabular");
	ASSERT_FALSE(s.has_value());
	EXPECT_TRUE(Mentions(s.error().format(), "bad.tabular")) << s.error().format();
}

TEST(Document, ReadsTheVec2Type)
{
	auto s = ReadSchema("[table t (pick none)]\nat: vec2\n", "t.tabular");
	ASSERT_TRUE(s.has_value()) << s.error().format();
	EXPECT_EQ(s->tables()[0].columns[0].type, column_type::Vec2);
}

TEST(Document, ReadsTheStringType)
{
	auto s = ReadSchema("[table t (pick none)]\n>n: string\n", "t.tabular");
	ASSERT_TRUE(s.has_value()) << s.error().format();
	EXPECT_EQ(s->tables()[0].columns[0].type, column_type::String);
}

TEST(Document, APickedTableIsDeclaredByName)
{
	auto s = ReadSchema(R"(
[table phase
  (pick or bits)
  (input window double)
  (order clip)]
>clip: int, bits: flags, window: band
)", "phase.tabular");
	ASSERT_TRUE(s.has_value()) << s.error().format();
	table_def const& def = s->tables()[0];
	EXPECT_EQ(def.pick, pick_policy::Or);
	EXPECT_EQ(def.output, 1u);
	EXPECT_EQ(def.columns[2].type, column_type::Band);
	EXPECT_EQ(def.columns[2].input, 0u);
}

TEST(Document, NoPickIsBest)
{
	constexpr std::string_view kBody = " (order clip)]\n>clip: int, v: int\n";
	auto none = ReadSchema(std::string("[table t") + std::string(kBody), "t.tabular");
	ASSERT_TRUE(none.has_value()) << none.error().format();
	EXPECT_EQ(none->tables()[0].pick, pick_policy::Best);

	auto spelled = ReadSchema(std::string("[table t (pick best)") + std::string(kBody), "t.tabular");
	ASSERT_TRUE(spelled.has_value()) << spelled.error().format();
	EXPECT_EQ(spelled->tables()[0].pick, pick_policy::Best);

	auto explicitNone = ReadSchema("[table t (pick none)]\nv: int\n", "t.tabular");
	ASSERT_TRUE(explicitNone.has_value()) << explicitNone.error().format();
	EXPECT_EQ(explicitNone->tables()[0].pick, pick_policy::None);
}

TEST(Document, EveryFileTableIsReadOnly)
{
	auto s = ReadSchema(R"(
[table a (pick none)]
v: int
[table b (pick all) (order clip)]
>clip: int
[table c (order clip)]
>clip: int
[grid g (rows r int) (columns c int) (cells int)]
)", "t.tabular");
	ASSERT_TRUE(s.has_value()) << s.error().format();
	ASSERT_EQ(s->tables().size(), 4u);
	for (table_def const& def : s->tables())
	{
		EXPECT_TRUE(def.read_only) << def.name;
	}
	EXPECT_EQ(s->tables()[0].pick, pick_policy::None);
	EXPECT_EQ(s->tables()[1].pick, pick_policy::All);
}

TEST(Document, MutableIsRefused)
{
	auto s = ReadSchema("[table t (pick none) (mutable)]\na: int\n", "t.tabular");
	ASSERT_FALSE(s.has_value());
	EXPECT_TRUE(Mentions(s.error().format(), "mutable")) << s.error().format();
	EXPECT_FALSE(ReadSchema("[table t (pick none) (read_only)]\na: int\n", "t.tabular").has_value());
	EXPECT_FALSE(ReadSchema("[table t (pick none) mutable]\na: int\n", "t.tabular").has_value());
}

TEST(Document, AnUndeclaredInputOrOutputIsRefused)
{
	auto input = ReadSchema("[table t (pick none)]\nspeed: band\n", "t.tabular");
	ASSERT_FALSE(input.has_value());
	EXPECT_TRUE(Mentions(input.error().format(), "input 'speed'")) << input.error().format();

	auto output = ReadSchema("[table t (pick or bits)]\nclip: int\n", "t.tabular");
	ASSERT_FALSE(output.has_value());
	EXPECT_TRUE(Mentions(output.error().format(), "output 'bits'")) << output.error().format();
}

TEST(Document, AGridHasAxesAndCellsAndNoColumns)
{
	auto ok = ReadSchema("[grid g (rows creature hash) (columns kind hash) (cells command)]", "g.tabular");
	ASSERT_TRUE(ok.has_value()) << ok.error().format();
	table_def const& def = ok->tables()[0];
	EXPECT_TRUE(def.is_grid);
	EXPECT_EQ(def.grid_rows.name, "creature");
	EXPECT_EQ(def.grid_columns.name, "kind");
	EXPECT_EQ(def.grid_cells, column_type::Command);

	auto both = ReadSchema("[grid g (rows r int) (columns c int) (cells int) (input x double)]", "g.tabular");
	EXPECT_FALSE(both.has_value());

	auto lacking = ReadSchema("[grid g (rows r int) (cells int)]", "g.tabular");
	ASSERT_FALSE(lacking.has_value());
	EXPECT_TRUE(Mentions(lacking.error().format(), "columns")) << lacking.error().format();

	auto neither = ReadSchema("[table t (pick none)]", "t.tabular");
	ASSERT_FALSE(neither.has_value());
	EXPECT_TRUE(Mentions(neither.error().format(), "no columns")) << neither.error().format();
}

TEST(Document, AnEmptyInputOrOutputIsRefusedNotNone)
{
	auto output = ReadSchema("[table t (pick or \"\") (order clip)]\n>clip: int\n", "t.tabular");
	ASSERT_FALSE(output.has_value());
	EXPECT_TRUE(Mentions(output.error().format(), "output ''")) << output.error().format();
}

TEST(Document, ABandIsNamedInRefusals)
{
	auto s = ReadSchema("[table t (pick none) (input window double)]\n>window: band\n", "t.tabular");
	ASSERT_FALSE(s.has_value());
	EXPECT_TRUE(Mentions(s.error().format(), "band")) << s.error().format();
	EXPECT_FALSE(Mentions(s.error().format(), "unknown type")) << s.error().format();
}

TEST(Document, AnInputReadsItsRange)
{
	auto s = ReadSchema(R"(
[table phase
  (pick or bits)
  (input window double -0.5..2)
  (order clip)]
>clip: int, bits: flags, window: band
)", "phase.tabular");
	ASSERT_TRUE(s.has_value()) << s.error().format();
	input_def const& t = s->tables()[0].inputs[0];
	EXPECT_TRUE(t.ranged);
	EXPECT_EQ(t.lo, -0.5f);
	EXPECT_EQ(t.hi, 2.0f);

	auto bad = ReadSchema("[table phase (pick none) (input t double \"0-1\")]\nclip: int\n", "phase.tabular");
	ASSERT_FALSE(bad.has_value());
	EXPECT_TRUE(Mentions(bad.error().format(), "written lo..hi")) << bad.error().format();
}

TEST(Document, EveryOptionIsRead)
{
	auto s = ReadSchema(R"(
[table keys
  (order a b n unique)
  (domain a x y)
  (domain n 1 2)]
>a, >b?, >n: int, v: double, then: command
[table foreign
  (order f)
  (domain f keys.a)]
>f, v: int
[table bands
  (input win double 0..1 cyclic)
  (input m flags)
  (pick all)
  (order clip)]
>clip: int, ^win
[table ors
  (pick or bits)
  (order clip)]
>clip: int, bits: flags
[table weights
  (pick weighted w)
  (order clip)]
>clip: int, w: double
[table pivoted
  (input mood flags)
  (order label)
  (pivot mood cell: string)]
>label: int
[table ranked
  (input rank flags)
  (order clip)]
>clip: int, rank: flags
)", "all.tabular");
	ASSERT_TRUE(s.has_value()) << s.error().format();
	ASSERT_EQ(s->tables().size(), 7u);

	table_def const& keys = s->tables()[0];
	EXPECT_EQ(keys.pick, pick_policy::Best);
	EXPECT_EQ(keys.key_count, 3u);
	EXPECT_TRUE(keys.unique);
	EXPECT_TRUE(keys.suffix);
	ASSERT_EQ(keys.domains.size(), 3u);
	EXPECT_EQ(keys.domains[0].kind, domain_kind::Members);
	EXPECT_EQ(keys.domains[1].kind, domain_kind::FallThrough);
	EXPECT_EQ(keys.domains[2].kind, domain_kind::Members);
	EXPECT_EQ(keys.domains[2].members.size(), 2u);

	EXPECT_EQ(s->tables()[1].domains[0].kind, domain_kind::Column);
	EXPECT_EQ(s->tables()[1].domains[0].table, 0u);
	EXPECT_EQ(s->tables()[1].domains[0].column, 0u);

	table_def const& bands = s->tables()[2];
	EXPECT_EQ(bands.pick, pick_policy::All);
	ASSERT_EQ(bands.inputs.size(), 2u);
	EXPECT_TRUE(bands.inputs[0].ranged);
	EXPECT_TRUE(bands.inputs[0].cyclic);
	EXPECT_EQ(bands.inputs[0].lo, 0.0f);
	EXPECT_EQ(bands.inputs[0].hi, 1.0f);
	EXPECT_EQ(bands.inputs[1].type, column_type::Flags);
	EXPECT_TRUE(bands.columns[1].edge);
	EXPECT_EQ(bands.columns[1].input, 0u);
	EXPECT_TRUE(bands.edges);

	EXPECT_EQ(s->tables()[3].pick, pick_policy::Or);
	EXPECT_EQ(s->tables()[3].output, 1u);
	EXPECT_EQ(s->tables()[4].pick, pick_policy::Weighted);
	EXPECT_EQ(s->tables()[4].weight, 1u);
	EXPECT_EQ(s->tables()[5].pivot_headers, 1u);
	EXPECT_EQ(s->tables()[5].pivot_cells, 2u);
	EXPECT_EQ(s->tables()[6].columns[1].input, 0u);
	EXPECT_EQ(s->tables()[6].ranking, 1u);
	for (table_def const& def : s->tables())
	{
		EXPECT_TRUE(def.read_only) << def.name;
	}
}

TEST(Document, RepeatedOptionIsRefused)
{
	auto pick = ReadSchema("[table t (pick best) (pick best)]\na: int\n", "t.tabular");
	ASSERT_FALSE(pick.has_value());
	EXPECT_TRUE(Mentions(pick.error().format(), "pick")) << pick.error().format();

	auto order = ReadSchema("[table t (pick none) (order a) (order a)]\na: int\n", "t.tabular");
	ASSERT_FALSE(order.has_value());
	EXPECT_TRUE(Mentions(order.error().format(), "order")) << order.error().format();

	auto domain = ReadSchema("[table t (domain a x) (domain a y)]\n>a, v: int\n", "t.tabular");
	ASSERT_FALSE(domain.has_value());
	EXPECT_TRUE(Mentions(domain.error().format(), "domain")) << domain.error().format();

	auto grid = ReadSchema("[grid g (rows r int) (rows r int) (columns c int) (cells int)]", "g.tabular");
	ASSERT_FALSE(grid.has_value());
	EXPECT_TRUE(Mentions(grid.error().format(), "rows")) << grid.error().format();
}

TEST(Document, OptionOnWrongHeadIsRefused)
{
	auto table = ReadSchema("[table t (pick none) (cells int)]\na: int\n", "t.tabular");
	ASSERT_FALSE(table.has_value());
	EXPECT_TRUE(Mentions(table.error().format(), "cells")) << table.error().format();

	auto grid = ReadSchema("[grid g (rows r int) (columns c int) (cells int) (pick all)]", "g.tabular");
	ASSERT_FALSE(grid.has_value());
	EXPECT_TRUE(Mentions(grid.error().format(), "pick")) << grid.error().format();
}

TEST(Document, ABadPickShapeIsRefused)
{
	EXPECT_FALSE(ReadSchema("[table t (pick)]\na: int\n", "t.tabular").has_value());
	EXPECT_FALSE(ReadSchema("[table t (pick sorted)]\na: int\n", "t.tabular").has_value());
	EXPECT_FALSE(ReadSchema("[table t (pick or)]\na: int\n", "t.tabular").has_value());
	EXPECT_FALSE(ReadSchema("[table t (pick all bits)]\na: int\n", "t.tabular").has_value());
	EXPECT_FALSE(ReadSchema("[table t (pick weighted)]\na: int\n", "t.tabular").has_value());
}

TEST(Document, AFallThroughKeyDeclaresNoDomain)
{
	auto s = ReadSchema("[table t (domain a x)]\n>a?, v: int\n", "t.tabular");
	ASSERT_FALSE(s.has_value());
	EXPECT_TRUE(Mentions(s.error().format(), "falls through")) << s.error().format();
}

TEST(Document, RefusalNamesFileLine)
{
	auto s = ReadSchema(
		"[table t\n"
		"  # columns a and b,\n"
		"  # in the header row\n"
		"  # a note\n"
		"  (pick none)\n"
		"  (order a)\n"
		"  (uniqe a)]\n"
		"a: int, b: int\n", "t.tabular");
	ASSERT_FALSE(s.has_value());
	EXPECT_EQ(Where(s.error()), "t.tabular:7") << s.error().format();
	EXPECT_FALSE(Mentions(s.error().message, "t.tabular")) << "the message carries no position";
	EXPECT_FALSE(Mentions(s.error().message, "line")) << s.error().message;
}

TEST(Document, RowsAreKeptForTablesWithBodies)
{
	auto d = ReadDocument(
		"[table a (pick none)]\n"
		"v: int\n"
		"1\n"
		"[table b (pick none)]\n"
		"v: int\n"
		"[table c (pick none)]\n"
		"v: int\n"
		"3\n", "t.tabular");
	ASSERT_TRUE(d.has_value()) << d.error().format();
	ASSERT_EQ(d->rows().size(), 2u);
	EXPECT_EQ(d->rows()[0].table, 0u);
	EXPECT_EQ(d->rows()[0].text, "v\n1\n");
	EXPECT_EQ(d->rows()[0].first_line, 2u);
	EXPECT_EQ(d->rows()[1].table, 2u);
	EXPECT_EQ(d->rows()[1].first_line, 7u);
}

TEST(Document, ASchemaAloneRefusesWhatItWouldDrop)
{
	auto rows = ReadSchema("[table a (pick none)]\nv: int\n1\n", "t.tabular");
	ASSERT_FALSE(rows.has_value());
	EXPECT_TRUE(Mentions(rows.error().format(), "ReadDocument")) << rows.error().format();
}

// ---- Machines -----------------------------------------------------------------

namespace
{

int64_t Hash(std::string_view text)
{
	return static_cast<int64_t>(CityHash32(text.data(), text.size()));
}

// The override fixture of hierarchy_gtest: brain is total, brain_mod has rows
// only where it changes something and its clause overrides brain's, and a third
// clause reads the coalesced answer. A clause in each of the other lists too.
constexpr std::string_view kModded = R"(
[table brain
  (order mode)]
>mode, next, steps: int, command
[table brain_mod
  (order mode)]
>mode, next, command
[table label
  (order mode)]
>mode, code: int
[table tally
  (order level)]
>level: int, count: int
[machine grazer
  (state mode hash (members idle walk sleep) (initial idle))
  (state steps int (initial 0))
  (state code int (initial 0))
  (input energy double 0..1)]
list, from, where, given, set, emit, overrides
create, label, mode=(literal idle), , code=code, ,
entry, label, mode=state.mode, , code=code, ,
step, brain, mode=state.mode, , mode=next steps=steps, command,
step, brain_mod, mode=state.mode, , mode=next, command, 0
step, label, mode=clause.0.next, , code=code, ,
exit, label, mode=state.mode, , code=code, ,
)";

constexpr std::string_view kSmall =
	"[table t (order mode)]\n>mode, next\n"
	"[machine m (state mode hash (members a b) (initial a))]\n"
	"list,from,where,given,set,emit,overrides\n"
	"step,t,mode=state.mode,,mode=next,,\n";

}

TEST(Document, ReadsAMachine)
{
	auto d = ReadDocument(kModded, "m.tabular");
	ASSERT_TRUE(d.has_value()) << d.error().format();
	ASSERT_EQ(d->machines().size(), 1u);
	create_machine const& m = d->machines()[0];
	EXPECT_EQ(m.name, "grazer");

	ASSERT_EQ(m.state.size(), 3u);
	EXPECT_EQ(m.state[0].name, "mode");
	EXPECT_EQ(m.state[0].type, column_type::Hash);
	ASSERT_EQ(m.state[0].members.size(), 3u);
	EXPECT_EQ(m.state[0].members[1].i, Hash("walk"));
	EXPECT_EQ(m.state[0].initial.i, Hash("idle"));
	EXPECT_EQ(m.state[1].type, column_type::Int);
	EXPECT_EQ(m.state[1].initial.i, 0);

	ASSERT_EQ(m.inputs.size(), 1u);
	EXPECT_EQ(m.inputs[0].name, "energy");
	EXPECT_TRUE(m.inputs[0].ranged);
	EXPECT_FALSE(m.inputs[0].cyclic);
	EXPECT_EQ(m.inputs[0].hi, 1.0f);

	ASSERT_EQ(m.create.size(), 1u);
	ASSERT_EQ(m.entry.size(), 1u);
	ASSERT_EQ(m.step.size(), 3u);
	ASSERT_EQ(m.exit.size(), 1u);
	EXPECT_EQ(m.create[0].table, "label");
	EXPECT_EQ(m.exit[0].table, "label");

	add_clause const& brain = m.step[0];
	EXPECT_EQ(brain.table, "brain");
	ASSERT_EQ(brain.where.size(), 1u);
	EXPECT_EQ(brain.where[0].target, "mode");
	EXPECT_EQ(brain.where[0].from.kind, source_kind::State);
	EXPECT_EQ(brain.where[0].from.name, "mode");
	EXPECT_EQ(brain.given.size(), 0u);
	ASSERT_EQ(brain.set.size(), 2u);
	EXPECT_EQ(brain.set[0].state, "mode");
	EXPECT_EQ(brain.set[0].column, "next");
	EXPECT_EQ(brain.set[1].state, "steps");
	EXPECT_EQ(brain.set[1].column, "steps");
	EXPECT_EQ(brain.emit, "command");
	EXPECT_EQ(brain.overrides, UINT32_MAX);

	EXPECT_EQ(m.step[1].table, "brain_mod");
	EXPECT_EQ(m.step[1].overrides, 0u);
	EXPECT_EQ(m.step[1].emit, "command");

	add_clause const& label = m.step[2];
	ASSERT_EQ(label.where.size(), 1u);
	EXPECT_EQ(label.where[0].from.kind, source_kind::Clause);
	EXPECT_EQ(label.where[0].from.clause, 0u);
	EXPECT_EQ(label.where[0].from.name, "next");
	EXPECT_EQ(label.emit, "");
}

TEST(Document, LiteralReadsByTargetType)
{
	auto hashed = ReadDocument(kModded, "m.tabular");
	ASSERT_TRUE(hashed.has_value()) << hashed.error().format();
	source const& idle = hashed->machines()[0].create[0].where[0].from;
	EXPECT_EQ(idle.kind, source_kind::Literal);
	EXPECT_EQ(idle.literal.i, Hash("idle"));

	auto numbered = ReadDocument(
		"[table tally (order level)]\n>level: int, count: int\n"
		"[machine m (state count int (initial 0))]\n"
		"list,from,where,given,set,emit,overrides\n"
		"step,tally,level=(literal 3),,count=count,,\n", "t.tabular");
	ASSERT_TRUE(numbered.has_value()) << numbered.error().format();
	EXPECT_EQ(numbered->machines()[0].step[0].where[0].from.literal.i, 3);

	auto quoted = ReadDocument(
		"[table t (order mode)]\n>mode, next\n"
		"[machine m (state mode hash (members \"a b\" c) (initial \"a b\"))]\n"
		"list,from,where,given,set,emit,overrides\n"
		"step,t,\"mode=(literal \"\"a b\"\")\",,mode=next,,\n", "t.tabular");
	ASSERT_TRUE(quoted.has_value()) << quoted.error().format();
	EXPECT_EQ(quoted->machines()[0].step[0].where[0].from.literal.i, Hash("a b"));
	EXPECT_EQ(quoted->machines()[0].state[0].initial.i, Hash("a b"));
}

TEST(Document, AMachineMayComeBeforeItsTables)
{
	std::string text(kSmall);
	// The table is its bracket line and its header row.
	std::string const tableLine = text.substr(0, text.find('\n', text.find('\n') + 1) + 1);
	text.erase(0, tableLine.size());
	text += tableLine;
	auto d = ReadDocument(text, "m.tabular");
	ASSERT_TRUE(d.has_value()) << d.error().format();
	EXPECT_EQ(d->machines().size(), 1u);
	EXPECT_EQ(d->tables.tables().size(), 1u);
}

TEST(Document, ASmallMachineReads)
{
	auto d = ReadDocument(kSmall, "t.tabular");
	ASSERT_TRUE(d.has_value()) << d.error().format();
	EXPECT_EQ(d->machines()[0].step.size(), 1u);
}

TEST(Document, PairWithoutEqualsIsRefused)
{
	std::string text(kSmall);
	text.replace(text.find("mode=state.mode"), 15, "mode state.mode");
	auto d = ReadDocument(text, "t.tabular");
	ASSERT_FALSE(d.has_value());
	EXPECT_TRUE(Mentions(d.error().format(), "mode")) << d.error().format();
	EXPECT_TRUE(Mentions(d.error().format(), "t.tabular:5")) << d.error().format();

	std::string empty(kSmall);
	empty.replace(empty.find("mode=state.mode"), 15, "mode=");
	EXPECT_FALSE(ReadDocument(empty, "t.tabular").has_value());
	std::string nameless(kSmall);
	nameless.replace(nameless.find("mode=state.mode"), 15, "=state.mode");
	EXPECT_FALSE(ReadDocument(nameless, "t.tabular").has_value());
}

TEST(Document, ARepeatedNameInAPairCellIsRefused)
{
	std::string text(kSmall);
	text.replace(text.find("mode=state.mode"), 15, "mode=state.mode mode=state.mode");
	auto d = ReadDocument(text, "t.tabular");
	ASSERT_FALSE(d.has_value());
	EXPECT_TRUE(Mentions(d.error().format(), "mode")) << d.error().format();
}

TEST(Document, UnknownListIsRefused)
{
	std::string text(kSmall);
	text.replace(text.find("step,t"), 6, "walk,t");
	auto d = ReadDocument(text, "t.tabular");
	ASSERT_FALSE(d.has_value());
	EXPECT_TRUE(Mentions(d.error().format(), "'walk'")) << d.error().format();
	EXPECT_TRUE(Mentions(d.error().format(), "t.tabular:5")) << d.error().format();
}

TEST(Document, WrongClauseHeaderIsRefused)
{
	std::string swapped(kSmall);
	swapped.replace(swapped.find("where,given"), 11, "given,where");
	auto d = ReadDocument(swapped, "t.tabular");
	ASSERT_FALSE(d.has_value());
	EXPECT_TRUE(Mentions(d.error().format(), "list, from, where, given, set, emit, overrides")) << d.error().format();
	EXPECT_TRUE(Mentions(d.error().format(), "t.tabular:4")) << d.error().format();

	std::string shorter(kSmall);
	shorter.replace(shorter.find(",overrides"), 10, "");
	EXPECT_FALSE(ReadDocument(shorter, "t.tabular").has_value());
	std::string longer(kSmall);
	longer.replace(longer.find(",overrides"), 10, ",overrides,extra");
	EXPECT_FALSE(ReadDocument(longer, "t.tabular").has_value());
}

TEST(Document, ClauseRefusalNamesFileLine)
{
	std::string text(kSmall);
	text += "step,t,mode=bogus.x,,mode=next,,\n";
	auto d = ReadDocument(text, "t.tabular");
	ASSERT_FALSE(d.has_value());
	EXPECT_TRUE(Mentions(d.error().format(), "t.tabular:6")) << d.error().format();
	EXPECT_TRUE(Mentions(d.error().format(), "bogus.x")) << d.error().format();
	EXPECT_TRUE(Mentions(d.error().format(), "machine 'm'")) << d.error().format();
	EXPECT_FALSE(Mentions(d.error().message, "t.tabular")) << d.error().message;

	std::string literal(kSmall);
	literal.replace(literal.find("mode=state.mode"), 15, "mode=(literal)");
	EXPECT_FALSE(ReadDocument(literal, "t.tabular").has_value());
	std::string overrides(kSmall);
	overrides.replace(overrides.find("next,,"), 6, "next,,x");
	auto bad = ReadDocument(overrides, "t.tabular");
	ASSERT_FALSE(bad.has_value());
	EXPECT_TRUE(Mentions(bad.error().format(), "overrides")) << bad.error().format();
	std::string noFrom(kSmall);
	noFrom.replace(noFrom.find("step,t,"), 7, "step,,");
	EXPECT_FALSE(ReadDocument(noFrom, "t.tabular").has_value());
}

TEST(Document, AMachineIsCheckedAgainstTheTables)
{
	std::string text(kSmall);
	text.replace(text.find("step,t,"), 7, "step,nope,");
	auto d = ReadDocument(text, "t.tabular");
	ASSERT_FALSE(d.has_value());
	EXPECT_TRUE(Mentions(d.error().format(), "nope")) << d.error().format();
	EXPECT_TRUE(Mentions(d.error().format(), "t.tabular")) << d.error().format();
}

TEST(Document, ASchemaAloneRefusesMachines)
{
	auto s = ReadSchema(kSmall, "t.tabular");
	ASSERT_FALSE(s.has_value());
	EXPECT_TRUE(Mentions(s.error().format(), "ReadDocument")) << s.error().format();
}

TEST(Document, AMachineOptionIsKnown)
{
	std::string text(kSmall);
	text.replace(text.find("(state"), 6, "(stat");
	auto d = ReadDocument(text, "t.tabular");
	ASSERT_FALSE(d.has_value());
	EXPECT_TRUE(Mentions(d.error().format(), "'stat'")) << d.error().format();
	EXPECT_TRUE(Mentions(d.error().format(), "t.tabular:3")) << d.error().format();

	auto twice = ReadDocument(
		"[table t (order mode)]\n>mode, next\n"
		"[machine m (state mode hash (members a) (initial a)) (state mode hash (members a) (initial a))]\n", "t.tabular");
	EXPECT_FALSE(twice.has_value()) << "two state columns named mode";
	auto noInitial = ReadDocument("[machine m (state mode hash (members a))]\n", "t.tabular");
	EXPECT_FALSE(noInitial.has_value());
}

// ---- Rows in the file ---------------------------------------------------------

namespace
{

using condition_reader = std::function<tl::expected<closure, ErrorContext>(std::string_view)>;

std::string Failure(tl::expected<void, ErrorContext> const& r)
{
	return r ? std::string() : r.error().format();
}

constexpr std::string_view kLookupFile = R"(
[table lookup
  (order label)]
>label: int, v: int
1, 10
2, 20
3, 30
)";

}

TEST(Document, RowsLoadFromTheFile)
{
	auto d = ReadDocument(kLookupFile, "t.tabular");
	ASSERT_TRUE(d.has_value()) << d.error().format();
	relation_store store(std::move(d->tables));
	ASSERT_EQ(Failure(LoadDocumentRows(store, *d, 7, condition_reader())), "");
	ASSERT_EQ(store.RowCount(0), 3u);
	EXPECT_EQ(store.OwnerOf(0, 0), 7u);

	cell const keys[] = {cell::Integer(2)};
	auto picked = PickBest(store, 0, pick_query{keys, {}});
	ASSERT_TRUE(picked.has_value()) << picked.error().format();
	ASSERT_TRUE(picked->has_value());
	EXPECT_EQ(store.Value(0, **picked, 1)->i, 20);
}

TEST(Document, PivotAndGridBodiesLoad)
{
	auto d = ReadDocument(R"(
[table lookup
  (input req flags)
  (order label)
  (pivot req clip: string)]
>label: int, None, Injured, Crouch, Injured|Crouch
1,c1_none,c1_injured,c1_crouch,
2,c2_none,c2_injured,c2_crouch,c2_combo
[grid g (rows r int) (columns c int) (cells int)]
r,1,2
1,10,11
2,20,21
)", "t.tabular");
	ASSERT_TRUE(d.has_value()) << d.error().format();
	ASSERT_EQ(d->rows().size(), 2u);
	relation_store store(std::move(d->tables));
	ASSERT_EQ(Failure(LoadDocumentRows(store, *d, 1, condition_reader())), "");

	cell const keys[]   = {cell::Integer(2)};
	cell const inputs[] = {cell::Integer(3)};
	auto picked = PickBest(store, 0, pick_query{keys, inputs});
	ASSERT_TRUE(picked.has_value() && picked->has_value());
	auto clip = store.StringAt(0, **picked, 2);
	ASSERT_TRUE(clip.has_value()) << clip.error().format();
	EXPECT_EQ(std::string_view(*clip), "c2_combo");

	auto grid = store.Grid(1);
	ASSERT_TRUE(grid.has_value()) << grid.error().format();
}

TEST(Document, BodyRefusalNamesFileLine)
{
	auto d = ReadDocument(
		"[table lookup\n"
		"  # a key, label, and a value, v,\n"
		"  # declared by the header row\n"
		"  (order label)]\n"
		">label: int, v: int\n"
		"1,10\n"
		"2,20\n"
		"# a note\n"
		"3,30\n"
		"4,40\n"
		"5,50\n"
		"6,notanumber\n", "t.tabular");
	ASSERT_TRUE(d.has_value()) << d.error().format();
	relation_store store(std::move(d->tables));
	auto loaded = LoadDocumentRows(store, *d, 1, condition_reader());
	ASSERT_FALSE(loaded.has_value());
	EXPECT_TRUE(Mentions(loaded.error().format(), "t.tabular:12")) << loaded.error().format();
}

TEST(Document, ARefusedBodyNamesItsTable)
{
	auto d = ReadDocument(R"(
[table first
  (order label)]
>label: int, v: int
1, 10
[table second
  (order label)]
>label: int, v: int
1, oops
)", "t.tabular");
	ASSERT_TRUE(d.has_value()) << d.error().format();
	relation_store store(std::move(d->tables));
	auto loaded = LoadDocumentRows(store, *d, 1, condition_reader());
	ASSERT_FALSE(loaded.has_value());
	EXPECT_TRUE(Mentions(loaded.error().format(), "table 'second'")) << loaded.error().format();
	EXPECT_FALSE(Mentions(loaded.error().format(), "table 'first'")) << loaded.error().format();
	EXPECT_EQ(store.RowCount(0), 1u) << "bodies before the refused one are loaded; a caller retracts the owner";
}

// ---- Review fixes -------------------------------------------------------------

namespace
{

size_t Count(std::string const& text, std::string_view piece)
{
	size_t n = 0;
	for (size_t at = text.find(piece); at != std::string::npos; at = text.find(piece, at + piece.size()))
	{
		++n;
	}
	return n;
}

}

TEST(Document, RowsIntoAStoreFromAnotherSchemaAreRefused)
{
	auto d = ReadDocument(R"(
[table first (pick none)]
v: int
[table second
  (order label)]
>label: int, v: int
1, 10
)", "t.tabular");
	ASSERT_TRUE(d.has_value()) << d.error().format();

	// Fewer tables than the document's body points at.
	auto fewer = ReadSchema("[table first (pick none)]\nv: int\n", "other.tabular");
	ASSERT_TRUE(fewer.has_value()) << fewer.error().format();
	relation_store small(std::move(*fewer));
	auto tooFew = LoadDocumentRows(small, *d, 1, condition_reader());
	ASSERT_FALSE(tooFew.has_value());
	EXPECT_TRUE(Mentions(tooFew.error().format(), "'second'")) << tooFew.error().format();

	// The same count, another table at that index.
	auto renamed = ReadSchema(R"(
[table first (pick none)]
v: int
[table other
  (order label)]
>label: int, v: int
)", "other.tabular");
	ASSERT_TRUE(renamed.has_value()) << renamed.error().format();
	relation_store other(std::move(*renamed));
	auto wrong = LoadDocumentRows(other, *d, 1, condition_reader());
	ASSERT_FALSE(wrong.has_value());
	EXPECT_TRUE(Mentions(wrong.error().format(), "'second'")) << wrong.error().format();
	EXPECT_EQ(other.RowCount(1), 0u);

	// Its own schema still loads.
	relation_store own(std::move(d->tables));
	EXPECT_EQ(Failure(LoadDocumentRows(own, *d, 1, condition_reader())), "");
}

TEST(Document, CheckMachineRefusalNamesTheRowLine)
{
	std::string text(kSmall);
	text.replace(text.find("mode=state.mode"), 15, "mode=state.nope");
	auto d = ReadDocument(text, "t.tabular");
	ASSERT_FALSE(d.has_value());
	EXPECT_TRUE(Mentions(d.error().format(), "nope")) << d.error().format();
	EXPECT_TRUE(Mentions(d.error().format(), "t.tabular:5")) << d.error().format();

	// A refusal about the machine itself, with no clause in it, names its bracket.
	std::string initial(kSmall);
	initial.replace(initial.find("(initial a)"), 11, "(initial c)");
	auto machine = ReadDocument(initial, "t.tabular");
	ASSERT_FALSE(machine.has_value());
	EXPECT_TRUE(Mentions(machine.error().format(), "t.tabular:3")) << machine.error().format();
}

TEST(Document, QuotedNamesAreRefused)
{
	auto refused = [](std::string_view text)
	{
		return !ReadDocument(text, "t.tabular").has_value();
	};
	EXPECT_TRUE(refused("[table \"my table\" (pick none)]\na: int\n"));
	EXPECT_TRUE(refused("[table t (pick none)]\nmy col: int\n"));
	EXPECT_TRUE(refused("[table t (pick none)]\n>\"k\": int\n"));
	EXPECT_TRUE(refused("[table t (pick none) (input \"in put\" double)]\na: int\n"));
	EXPECT_TRUE(refused("[grid \"g g\" (rows r int) (columns c int) (cells int)]"));
	EXPECT_TRUE(refused("[grid g (rows \"r\" int) (columns c int) (cells int)]"));
	EXPECT_TRUE(refused("[table t (pick none)]\na: int, then it: command\n"));
	EXPECT_TRUE(refused("[machine \"m m\"]"));
	EXPECT_TRUE(refused("[machine m (state \"mo de\" int (initial 0))]"));
	EXPECT_TRUE(refused("[machine m (input \"in put\" double)]"));

	auto d = ReadDocument("[table \"my table\" (pick none)]\na: int\n", "t.tabular");
	ASSERT_FALSE(d.has_value());
	EXPECT_TRUE(Mentions(d.error().format(), "my table")) << d.error().format();
	EXPECT_TRUE(Mentions(d.error().format(), "t.tabular:1")) << d.error().format();
	EXPECT_TRUE(ReadDocument("[table t (pick none)]\na: int\n", "t.tabular").has_value());
}

TEST(Document, AMalformedLiteralNamesItsClauseOnce)
{
	for (std::string_view bad : {"mode=(literal)", "mode=(literal a b)", "mode=(literal (x))", "mode=(literal a"})
	{
		std::string text(kSmall);
		text.replace(text.find("mode=state.mode"), 15, bad);
		auto d = ReadDocument(text, "t.tabular");
		ASSERT_FALSE(d.has_value()) << bad;
		std::string const all = d.error().format();
		EXPECT_EQ(Count(all, "machine 'm' step clause 0"), 1u) << bad << "\n" << all;
		EXPECT_EQ(Count(all, "t.tabular:5"), 1u) << bad << "\n" << all;
	}
}

TEST(Document, SchemaRefusalNamesTheTableLine)
{
	auto s = ReadSchema(
		"# two tables\n"
		"[table fine (pick none)]\nv: int\n"
		"[table keyed\n"
		"  # a fall-through key is a hash\n"
		"  # and this one is an int\n"
		"  (order k)]\n"
		">k?: int, v: int\n", "t.tabular");
	ASSERT_FALSE(s.has_value());
	EXPECT_TRUE(Mentions(s.error().format(), "t.tabular:4")) << s.error().format();
	EXPECT_TRUE(Mentions(s.error().format(), "keyed")) << s.error().format();
}

TEST(Document, ADomainCycleNamesALine)
{
	auto s = ReadSchema(
		"# a cycle\n"
		"[table a (order k) (domain k b.k)]\n>k: int, v: int\n"
		"[table b (order k) (domain k a.k)]\n>k: int, v: int\n"
		"[table fine (pick none)]\nv: int\n", "t.tabular");
	ASSERT_FALSE(s.has_value());
	EXPECT_TRUE(Mentions(s.error().format(), "cycle")) << s.error().format();
	EXPECT_TRUE(Mentions(s.error().format(), "t.tabular:2")) << s.error().format();
}

TEST(Document, AnUnclosedQuoteInABodyDoesNotSwallowTheNextSection)
{
	auto d = ReadDocument("[table t (order k)]\n>k\n\"open\n[table u (pick none)]\nv: int\n", "t.tabular");
	ASSERT_FALSE(d.has_value());
	EXPECT_EQ(Where(d.error()), "t.tabular:3") << d.error().format();
}

TEST(Document, ATableWithOnlyAHeaderLineHasNoRows)
{
	constexpr std::string_view headerOnly = "[table t (order k)]\n>k, v: int\n# a note\n,\n\n";
	auto s = ReadSchema(headerOnly, "t.tabular");
	EXPECT_TRUE(s.has_value()) << (s ? std::string() : s.error().format());
	auto d = ReadDocument(headerOnly, "t.tabular");
	ASSERT_TRUE(d.has_value());
	EXPECT_TRUE(d->rows().empty());

	// A comment is a row whose first unquoted character is '#': a comma before
	// it makes a row, as csv_text reads it.
	auto row = ReadSchema("[table t (order k)]\n>k, v: int\n,# x\n", "t.tabular");
	EXPECT_FALSE(row.has_value());
}

TEST(Document, AGridsHeaderLineIsPartOfItsBody)
{
	auto d = ReadDocument("[grid g (rows r int) (columns c int) (cells int)]\nr,1\n", "g.tabular");
	ASSERT_TRUE(d.has_value()) << d.error().format();
	EXPECT_EQ(d->rows().size(), 1u);
	EXPECT_FALSE(ReadSchema("[grid g (rows r int) (columns c int) (cells int)]\nr,1\n", "g.tabular").has_value());
}

// The example in document.h's header comment, as written there.
TEST(Document, TheHeaderCommentsExampleIsAValidFile)
{
	constexpr std::string_view example = R"([table grazer_brain
  (order mode)
  (domain mode idle walk)]
>mode, next, command
idle,walk,go
walk,idle,stop

[grid g (rows r int) (columns c int) (cells int)]

[machine grazer (state mode hash (members idle walk) (initial idle))]
list,from,where,given,set,emit,overrides
step,grazer_brain,mode=state.mode,,mode=next,command,
)";
	auto d = ReadDocument(example, "example.tabular");
	ASSERT_TRUE(d.has_value()) << d.error().format();
	EXPECT_EQ(d->machines().size(), 1u);
	EXPECT_EQ(d->rows().size(), 1u);
}

namespace
{

// Every field of a table a declaration sets, as one text, so two tables
// compare whole and a failure says which field differs.
std::string Describe(table_def const& def)
{
	std::ostringstream out;
	out << def.name << " pick=" << int(def.pick) << " output=" << def.output << " weight=" << def.weight
		<< " ranking=" << def.ranking << " keys=" << def.key_count << " suffix=" << def.suffix << " unique=" << def.unique
		<< " pivot=" << def.pivot_headers << "/" << def.pivot_cells << " edges=" << def.edges << " grid=" << def.is_grid
		<< "\n";
	for (column_def const& c : def.columns)
	{
		out << "  column " << c.name << " type=" << int(c.type) << " key=" << c.key << " input=" << c.input
			<< " edge=" << c.edge << "\n";
	}
	for (input_def const& i : def.inputs)
	{
		out << "  input " << i.name << " type=" << int(i.type) << " ranged=" << i.ranged << " " << i.lo << ".." << i.hi
			<< " cyclic=" << i.cyclic << "\n";
	}
	for (uint32_t o : def.order)
	{
		out << "  order " << o << "\n";
	}
	for (domain_def const& d : def.domains)
	{
		out << "  domain kind=" << int(d.kind) << " table=" << d.table << " column=" << d.column << " members=";
		for (int64_t m : d.members)
		{
			out << m << " ";
		}
		out << "\n";
	}
	return out.str();
}

std::string Describe(schema const& s)
{
	std::string out;
	for (table_def const& def : s.tables())
	{
		out += Describe(def);
	}
	return out;
}

std::string DescribeFile(std::string_view text)
{
	auto d = ReadDocument(text, "t.tabular");
	return d ? Describe(d->tables) : "refused: " + d.error().format();
}

std::string DescribeBuilt(std::span<create_table const> tables)
{
	auto built = BuildSchema(tables);
	return built ? Describe(*built) : "refused: " + built.error().format();
}

std::string RefusalOf(std::string_view text)
{
	auto d = ReadDocument(text, "t.tabular");
	return d ? std::string("read") : d.error().format();
}

}

TEST(Document, AHeaderDeclaresWhatTheOptionsDid)
{
	EXPECT_EQ(DescribeFile(R"(
[table spawn_points
  (pick none)
  (order section package unique)]
>package, section, x: double, y: double
[table rules
  (pick none)]
when, then: command, mask: flags, count: int
)"), DescribeFile(kSpawnPoints));

	EXPECT_EQ(DescribeFile(R"(
[table grazer_brain
  (domain mode idle walk)
  (order mode)]
>mode, next, command
idle,walk,go
walk,idle,stop
)"), ([] {
		cell const      walks[]  = {cell::Integer(Hash("idle")), cell::Integer(Hash("walk"))};
		add_column const cols[] = {{"mode", column_type::Hash, true, UINT32_MAX, {domain_kind::Members, walks, {}, {}}},
			{"next", column_type::Hash}};
		uint32_t const  byMode[] = {0};
		order_by const  order{byMode};
		create_table const t[] = {{"grazer_brain", cols, &order, true, {}, pick_policy::Best, UINT32_MAX, nullptr,
			nullptr, "command"}};
		return DescribeBuilt(t);
	}()));

	EXPECT_EQ(DescribeFile(R"(
[table keys
  (domain a x y)
  (domain n 1 2)
  (order a b n unique)]
>a, >b?, >n: int, v: double, then: command
[table foreign
  (domain f keys.a)
  (order f)]
>f, v: int
[table bands
  (input win double 0..1 cyclic)
  (input m flags)
  (pick all)
  (order clip)]
>clip: int, ^win
[table ors
  (pick or bits)
  (order clip)]
>clip: int, bits: flags
[table weights
  (pick weighted w)
  (order clip)]
>clip: int, w: double
[table pivoted
  (input mood flags)
  (order label)
  (pivot mood cell: string)]
>label: int
[table ranked
  (input rank flags)
  (order clip)]
>clip: int, rank: flags
)"), ([] {
		cell const      xy[]       = {cell::Integer(Hash("x")), cell::Integer(Hash("y"))};
		cell const      oneTwo[]   = {cell::Integer(1), cell::Integer(2)};
		add_column const keys[] = {{"a", column_type::Hash, true, UINT32_MAX, {domain_kind::Members, xy, {}, {}}},
			{"b", column_type::Hash, true, UINT32_MAX, key_domain::FallThrough()},
			{"n", column_type::Int, true, UINT32_MAX, {domain_kind::Members, oneTwo, {}, {}}},
			{"v", column_type::Double}};
		add_column const foreign[] = {{"f", column_type::Hash, true, UINT32_MAX, {domain_kind::Column, {}, "keys", "a"}},
			{"v", column_type::Int}};
		add_input const  bandInputs[] = {{"win", column_type::Double, true, 0, 1, true}, {"m", column_type::Flags}};
		add_column const bands[]   = {{"clip", column_type::Int, true}, {"win", column_type::Band, false, 0, {}, true}};
		add_column const ors[]     = {{"clip", column_type::Int, true}, {"bits", column_type::Flags}};
		add_column const weights[] = {{"clip", column_type::Int, true}, {"w", column_type::Double}};
		add_input const  mood[]    = {{"mood", column_type::Flags}};
		add_column const pivoted[] = {{"label", column_type::Int, true, UINT32_MAX, key_domain::Rows()},
			{"mood", column_type::Flags, false, 0}, {"cell", column_type::String}};
		add_input const  rank[]    = {{"rank", column_type::Flags}};
		add_column const ranked[]  = {{"clip", column_type::Int, true, UINT32_MAX, key_domain::Rows()},
			{"rank", column_type::Flags, false, 0}};
		uint32_t const   abn[]   = {0, 1, 2};
		uint32_t const   first[] = {0};
		order_by const   unique{abn, true};
		order_by const   byFirst{first};
		pivot_by const   pivot{1, 2};
		create_table const t[] = {
			{"keys", keys, &unique, true, {}, pick_policy::Best, UINT32_MAX, nullptr, nullptr, "then"},
			{"foreign", foreign, &byFirst, true, {}, pick_policy::Best},
			{"bands", bands, &byFirst, true, bandInputs, pick_policy::All},
			{"ors", ors, &byFirst, true, {}, pick_policy::Or, 1},
			{"weights", weights, &byFirst, true, {}, pick_policy::Weighted, UINT32_MAX, nullptr, nullptr, {}, 1},
			{"pivoted", pivoted, &byFirst, true, mood, pick_policy::Best, UINT32_MAX, &pivot},
			{"ranked", ranked, &byFirst, true, rank, pick_policy::Best},
		};
		return DescribeBuilt(t);
	}()));
}

TEST(Document, ALastCommandCellIsTheSuffix)
{
	auto d = ReadDocument("[table t (pick none)]\nk: int, then: command\n1, (a b), (c d)\n", "t.tabular");
	ASSERT_TRUE(d.has_value()) << d.error().format();
	table_def const& def = d->tables.tables()[0];
	EXPECT_TRUE(def.suffix);
	ASSERT_EQ(def.columns.size(), 2u);
	EXPECT_EQ(def.columns[1].name, "then");
	relation_store store(std::move(d->tables));
	ASSERT_EQ(Failure(LoadDocumentRows(store, *d, 1, condition_reader())), "");
	EXPECT_EQ(*store.CommandAt(0, 0, 1), "((a b) (c d))") << "the suffix takes the rest of the row";
}

TEST(Document, AMiddleCommandCellIsOneCell)
{
	auto d = ReadDocument("[table t (pick none)]\nthen: command, k: int\n(a b), 1\n", "t.tabular");
	ASSERT_TRUE(d.has_value()) << d.error().format();
	table_def const& def = d->tables.tables()[0];
	EXPECT_FALSE(def.suffix);
	EXPECT_EQ(def.columns[0].type, column_type::Command);
	relation_store store(std::move(d->tables));
	ASSERT_EQ(Failure(LoadDocumentRows(store, *d, 1, condition_reader())), "");
	EXPECT_EQ(*store.CommandAt(0, 0, 0), "((a b))");
	EXPECT_EQ(store.Value(0, 0, 1)->i, 1);
}

TEST(Document, AWeightColumnMakesTheTableWeighted)
{
	auto weighted = ReadDocument("[table t (order clip)]\n>clip: int, weight\n", "t.tabular");
	ASSERT_TRUE(weighted.has_value()) << weighted.error().format();
	EXPECT_EQ(weighted->tables.tables()[0].pick, pick_policy::Weighted);
	EXPECT_EQ(weighted->tables.tables()[0].weight, 1u);
	EXPECT_EQ(weighted->tables.tables()[0].columns[1].type, column_type::Double);

	auto best = ReadDocument("[table t (pick best) (order clip)]\n>clip: int, weight\n", "t.tabular");
	ASSERT_TRUE(best.has_value()) << best.error().format();
	EXPECT_EQ(best->tables.tables()[0].pick, pick_policy::Best) << "a (pick ..) overrides the weight column";
	EXPECT_EQ(best->tables.tables()[0].weight, UINT32_MAX);
}

TEST(Document, ABandMustBeNamedForAnInput)
{
	EXPECT_EQ(DescribeFile("[table t (input hp double 0..10) (pick all)]\nhp: band, v: int\n"),
		([] {
			add_input const  hp[]   = {{"hp", column_type::Double, true, 0, 10}};
			add_column const cols[] = {{"hp", column_type::Band, false, 0}, {"v", column_type::Int}};
			create_table const t[] = {{"t", cols, nullptr, true, hp, pick_policy::All}};
			return DescribeBuilt(t);
		}()));
	std::string const refused = RefusalOf("[table t (input t double 0..10) (pick all)]\nhp: band, v: int\n");
	EXPECT_TRUE(Mentions(refused, "'hp'")) << refused;
	EXPECT_TRUE(Mentions(refused, "input")) << refused;
	EXPECT_TRUE(Mentions(refused, "t.tabular:2")) << refused;
	std::string const edge = RefusalOf("[table t (input t double 0..10) (pick all)]\n^x, v: int\n");
	EXPECT_TRUE(Mentions(edge, "'x'")) << edge;
}

TEST(Document, AFlagsColumnNamedForAnInputRanks)
{
	auto s = ReadSchema("[table t (input mode flags) (order clip)]\n>clip: int, mode: flags, bits: flags\n", "t.tabular");
	ASSERT_TRUE(s.has_value()) << s.error().format();
	table_def const& def = s->tables()[0];
	EXPECT_EQ(def.columns[1].input, 0u) << "named for an input: ranking";
	EXPECT_EQ(def.ranking, 1u);
	EXPECT_EQ(def.columns[2].input, UINT32_MAX) << "no input of its name: a value";
}

TEST(Document, DomainOptionsDeclareMembersAndOtherTables)
{
	auto s = ReadSchema(R"(
[table kinds (pick none)]
>kind
[table t
  (domain mode idle walk)
  (domain level 1 3)
  (domain kind kinds.kind)
  (order mode level kind)]
>mode, >level: int, >kind, v: int
)", "t.tabular");
	ASSERT_TRUE(s.has_value()) << s.error().format();
	table_def const& def = s->tables()[1];
	ASSERT_EQ(def.domains.size(), 3u);
	EXPECT_EQ(def.domains[0].kind, domain_kind::Members);
	EXPECT_EQ(def.domains[0].members.size(), 2u);
	EXPECT_EQ(def.domains[1].kind, domain_kind::Members);
	EXPECT_EQ(def.domains[1].members[0], 1);
	EXPECT_EQ(def.domains[2].kind, domain_kind::Column);
	EXPECT_EQ(def.domains[2].table, 0u);

	std::string const noKey = RefusalOf("[table t (domain nokey a) (order mode)]\n>mode, v: int\n");
	EXPECT_TRUE(Mentions(noKey, "'nokey'")) << noKey;
	EXPECT_TRUE(Mentions(noKey, "t.tabular:1")) << noKey;
	std::string const fall = RefusalOf("[table t (domain sub a) (order mode)]\n>mode, >sub?, v: int\n");
	EXPECT_TRUE(Mentions(fall, "'sub'")) << fall;
	EXPECT_TRUE(Mentions(fall, "falls through")) << fall;
	std::string const twice = RefusalOf("[table t (domain mode a) (domain mode b) (order mode)]\n>mode, v: int\n");
	EXPECT_TRUE(Mentions(twice, "'mode'")) << twice;
	EXPECT_FALSE(ReadSchema("[table t (domain mode) (order mode)]\n>mode, v: int\n", "t.tabular").has_value());
}

TEST(Document, APivotDeclaresItsTwoColumnsInTheBracket)
{
	constexpr std::string_view kHeaderForm = R"(
[table lookup
  (input req flags)
  (order label)
  (pivot req clip: string)]
>label: int, None, Injured, Crouch, Injured|Crouch
1,c1_none,c1_injured,c1_crouch,
2,c2_none,c2_injured,c2_crouch,c2_combo
)";
	EXPECT_EQ(DescribeFile(kHeaderForm), ([] {
		add_input const  req[]  = {{"req", column_type::Flags}};
		add_column const cols[] = {{"label", column_type::Int, true, UINT32_MAX, key_domain::Rows()},
			{"req", column_type::Flags, false, 0}, {"clip", column_type::String}};
		uint32_t const   first[] = {0};
		order_by const   order{first};
		pivot_by const   pivot{1, 2};
		create_table const t[] = {{"lookup", cols, &order, true, req, pick_policy::Best, UINT32_MAX, &pivot}};
		return DescribeBuilt(t);
	}()));
	auto d = ReadDocument(kHeaderForm, "t.tabular");
	ASSERT_TRUE(d.has_value()) << d.error().format();
	relation_store store(std::move(d->tables));
	ASSERT_EQ(Failure(LoadDocumentRows(store, *d, 1, condition_reader())), "");
	cell const keys[]   = {cell::Integer(2)};
	cell const inputs[] = {cell::Integer(3)};
	auto picked = PickBest(store, 0, pick_query{keys, inputs});
	ASSERT_TRUE(picked.has_value() && picked->has_value());
	EXPECT_EQ(std::string_view(*store.StringAt(0, **picked, 2)), "c2_combo");

	// The cells column untyped is a hash, and a band when named for an input.
	auto bands = ReadSchema("[table b (input req flags) (input t double 0..1) (order label) (pivot req t)]\n>label: int\n",
		"t.tabular");
	ASSERT_TRUE(bands.has_value()) << bands.error().format();
	EXPECT_EQ(bands->tables()[0].columns[2].type, column_type::Band);
	EXPECT_EQ(bands->tables()[0].columns[2].input, 1u);
	auto hashes = ReadSchema("[table h (input req flags) (order label) (pivot req clip)]\n>label: int\n", "t.tabular");
	ASSERT_TRUE(hashes.has_value()) << hashes.error().format();
	EXPECT_EQ(hashes->tables()[0].columns[2].type, column_type::Hash);
}

TEST(Document, ATableWithoutAHeaderRowIsRefused)
{
	std::string const neither = RefusalOf("[table t (pick none)]\n");
	EXPECT_TRUE(Mentions(neither, "no columns")) << neither;
}

TEST(Document, ACompiledRowRefusalNamesItsSourceLine)
{
	auto d = ReadDocument(
		"[table t (order k)]\n"
		"# the header follows a note\n"
		">k: int, v: int\n"
		"1, 10\n"
		"\n"
		"\"2\", \"20,\"\"x\"\"\"\n", "t.tabular");
	ASSERT_TRUE(d.has_value()) << d.error().format();
	ASSERT_EQ(d->rows().size(), 1u);
	EXPECT_EQ(d->rows()[0].text, "k,v\n1,10\n2,\"20,\"\"x\"\"\"\n") << "cells are re-quoted only where they must be";
	relation_store store(std::move(d->tables));
	auto loaded = LoadDocumentRows(store, *d, 1, condition_reader());
	ASSERT_FALSE(loaded.has_value());
	EXPECT_EQ(Where(loaded.error()), "t.tabular:6") << loaded.error().format();
}

TEST(Document, TheRemovedOptionsAreUnknown)
{
	for (std::string_view option : {"(key k hash (domain rows))", "(column v int)", "(suffix command)"})
	{
		std::string const text = cc(std::string("[table t "), option, "]\n>k, v: int\n");
		std::string const refused = RefusalOf(text);
		EXPECT_TRUE(Mentions(refused, "unknown option")) << option << ": " << refused;
		EXPECT_TRUE(Mentions(refused, "t.tabular:1")) << refused;
	}
}

namespace
{

// The row of table 0 whose key column 0 holds key.
size_t RowOf(relation_store const& store, cell key)
{
	auto rows = store.Find(0, 0, key);
	EXPECT_TRUE(rows.has_value() && rows->size() == 1u);
	return rows.has_value() && rows->size() == 1u ? (*rows)[0] : SIZE_MAX;
}

}

TEST(Document, ABlankCellTakesTheColumnDefault)
{
	auto d = ReadDocument("[table t (order k)]\n>k: int, status = None, n: int = 3, command = (idle)\n"
		"1, , \n"
		"2, Hurt, 4, (run) (stop)\n", "t.tabular");
	ASSERT_TRUE(d.has_value()) << d.error().format();
	EXPECT_EQ(d->rows()[0].text, "k,status,n,command\n1,None,3,(idle)\n2,Hurt,4,(run) (stop)\n");
	relation_store store(std::move(d->tables));
	ASSERT_EQ(Failure(LoadDocumentRows(store, *d, 1, condition_reader())), "");
	size_t const one = RowOf(store, cell::Integer(1));
	size_t const two = RowOf(store, cell::Integer(2));
	EXPECT_EQ(store.Value(0, one, 1)->i, Hash("None"));
	EXPECT_EQ(store.Value(0, one, 2)->i, 3);
	EXPECT_EQ(*store.CommandAt(0, one, 3), "((idle))");
	EXPECT_EQ(store.Value(0, two, 1)->i, Hash("Hurt")) << "a written cell is not defaulted";
	EXPECT_EQ(store.Value(0, two, 2)->i, 4);
	EXPECT_EQ(*store.CommandAt(0, two, 3), "((run) (stop))");
}

TEST(Document, ADefaultNamingAKeyTakesThatRowsKey)
{
	auto d = ReadDocument("[table t (order mode)]\n>mode, next = mode\nidle,\nwalk, idle\neat,\n", "t.tabular");
	ASSERT_TRUE(d.has_value()) << d.error().format();
	relation_store store(std::move(d->tables));
	ASSERT_EQ(Failure(LoadDocumentRows(store, *d, 1, condition_reader())), "");
	for (auto [mode, next] : {std::pair{"idle", "idle"}, std::pair{"walk", "idle"}, std::pair{"eat", "eat"}})
	{
		size_t const row = RowOf(store, cell::Integer(Hash(mode)));
		EXPECT_EQ(store.Value(0, row, 1)->i, Hash(next)) << mode;
	}
}

TEST(Document, ABlankBandIsTheInputsWholeRange)
{
	constexpr std::string_view kBracket = "[table t (input hp double -100..1000) (pick all) (order k)]\n>k: int, hp: band, v: int\n";
	auto blank   = ReadDocument(cc(std::string(kBracket), "1, , 10\n2, 0..5, 20\n3,,30\n"), "t.tabular");
	auto written = ReadDocument(cc(std::string(kBracket), "1, -100..1000, 10\n2, 0..5, 20\n3,-100..1000,30\n"), "t.tabular");
	ASSERT_TRUE(blank.has_value()) << blank.error().format();
	ASSERT_TRUE(written.has_value()) << written.error().format();
	// A store reads no band cell back, so the compiled rows are compared: the
	// blank compiles to what the author could have written.
	EXPECT_EQ(blank->rows()[0].text, written->rows()[0].text);
	relation_store a(std::move(blank->tables));
	ASSERT_EQ(Failure(LoadDocumentRows(a, *blank, 1, condition_reader())), "");
	EXPECT_EQ(a.Value(0, RowOf(a, cell::Integer(3)), 2)->i, 30);
}

TEST(Document, ABlankBandOnAnUnrangedInputIsRefused)
{
	std::string const refused = RefusalOf("[table t (input hp double) (pick all)]\nk: int, hp: band\n1, 0..1\n2, \n");
	EXPECT_TRUE(Mentions(refused, "t.tabular:4")) << refused;
	EXPECT_TRUE(Mentions(refused, "'hp'")) << refused;
	EXPECT_TRUE(Mentions(refused, "range")) << refused;
}

TEST(Document, ABlankEdgeIsStillNoEdge)
{
	auto d = ReadDocument("[table t (input x double 0..1) (pick all)]\nk: int, ^x\n1,\n", "t.tabular");
	ASSERT_TRUE(d.has_value()) << d.error().format();
	EXPECT_EQ(d->rows()[0].text, "k,x\n1,\n");
	relation_store store(std::move(d->tables));
	ASSERT_EQ(Failure(LoadDocumentRows(store, *d, 1, condition_reader())), "");
	EXPECT_EQ(store.RowCount(0), 1u) << "the loader reads a blank edge cell as no edge";
}

namespace
{

// The compiled rows of a file's one table, or its refusal.
std::string CompiledOf(std::string_view text)
{
	auto d = ReadDocument(text, "t.tabular");
	if (!d)
	{
		return "refused: " + d.error().format();
	}
	return d->rows().empty() ? std::string("no rows") : std::string(d->rows()[0].text);
}

// What a best pick of table 0 answers in column 2, keyed on mode with one
// double input.
int64_t Picked(relation_store const& store, std::string_view mode, double hp)
{
	cell const keys[]   = {cell::Integer(Hash(mode))};
	cell const inputs[] = {cell::Real(hp)};
	auto       picked   = PickBest(store, 0, pick_query{keys, inputs});
	EXPECT_TRUE(picked.has_value() && picked->has_value()) << mode << " " << hp;
	return picked.has_value() && picked->has_value() ? store.Value(0, **picked, 2)->i : 0;
}

}

TEST(Document, AStarRowIsCopiedPerKeyValueWhereItIsWritten)
{
	constexpr std::string_view kTable = "[table t (input hp double 0..1) (order mode)]\n>mode, hp: band, next\n"
		"*, 0..0.5, flee\n"
		"idle, 0..1, idle\n"
		"walk, 0..1, walk\n";
	EXPECT_EQ(CompiledOf(kTable), "mode,hp,next\nidle,0..0.5,flee\nwalk,0..0.5,flee\nidle,0..1,idle\nwalk,0..1,walk\n");
	auto d = ReadDocument(kTable, "t.tabular");
	ASSERT_TRUE(d.has_value()) << d.error().format();
	relation_store store(std::move(d->tables));
	ASSERT_EQ(Failure(LoadDocumentRows(store, *d, 1, condition_reader())), "");
	EXPECT_EQ(Picked(store, "idle", 0.25), Hash("flee")) << "the * row is first, so it beats idle's own row";
	EXPECT_EQ(Picked(store, "walk", 0.25), Hash("flee"));
	EXPECT_EQ(Picked(store, "idle", 0.75), Hash("idle"));
}

TEST(Document, ABottomStarRowYieldsToAKeysOwnUnconditionalRow)
{
	constexpr std::string_view kTable = "[table t (input hp double 0..1) (order mode)]\n>mode, hp: band, next\n"
		"idle, , idle\n"
		"walk, 0..0.5, walk\n"
		"*, , rest\n";
	EXPECT_EQ(CompiledOf(kTable), "mode,hp,next\nidle,0..1,idle\nwalk,0..0.5,walk\nwalk,0..1,rest\n")
		<< "idle's copy is dropped: idle's own row answers every input first";
	auto d = ReadDocument(kTable, "t.tabular");
	ASSERT_TRUE(d.has_value()) << d.error().format();
	relation_store store(std::move(d->tables));
	ASSERT_EQ(Failure(LoadDocumentRows(store, *d, 1, condition_reader())), "");
	EXPECT_EQ(Picked(store, "idle", 0.75), Hash("idle"));
	EXPECT_EQ(Picked(store, "walk", 0.25), Hash("walk"));
	EXPECT_EQ(Picked(store, "walk", 0.75), Hash("rest")) << "walk's own row is conditional, so the * row is its fallback";
}

TEST(Document, AStarRowAfterAConditionalRowIsNotDropped)
{
	EXPECT_EQ(CompiledOf("[table t (order mode)]\n>mode, when, next\nidle, (near), idle\n*, , rest\n"),
		"mode,when,next\nidle,(near),idle\nidle,,rest\n") << "a condition";
	EXPECT_EQ(CompiledOf("[table t (input x double 0..1) (order mode)]\n>mode, ^x, next\nidle, 0..1, idle\n*, , rest\n"),
		"mode,x,next\nidle,0..1,idle\nidle,,rest\n") << "an edge";
	EXPECT_EQ(CompiledOf("[table t (input hp double 0..1) (order mode)]\n>mode, hp: band, next\nidle, 0..1, idle\n"
		"walk, 0..0.75, walk\n*, , rest\n"),
		"mode,hp,next\nidle,0..1,idle\nwalk,0..0.75,walk\nwalk,0..1,rest\n") << "a band short of the range; a whole range written out is unconditional";
	EXPECT_EQ(CompiledOf("[table t (input m flags) (order mode)]\n>mode, m: flags, next\nidle, A, idle\n*, , rest\n"),
		"mode,m,next\nidle,A,idle\nidle,,rest\n") << "other ranking flags";
}

TEST(Document, AStarUsesDomainMembersOverRowValues)
{
	EXPECT_EQ(CompiledOf("[table t (domain mode idle walk eat) (order mode)]\n>mode, next\nwalk, idle\n*, rest\n"),
		"mode,next\nwalk,idle\nidle,rest\neat,rest\n");
}

TEST(Document, TwoStarsGiveTheProductTheFirstKeySlowest)
{
	EXPECT_EQ(CompiledOf("[table t (domain a x y) (domain b p q) (order a b)]\n>a, >b, v: int\n*, *, 1\n"),
		"a,b,v\nx,p,1\nx,q,1\ny,p,1\ny,q,1\n");
}

TEST(Document, AStarOnAnotherTablesDomainIsRefused)
{
	std::string const refused = RefusalOf(
		"[table kinds (pick none)]\n>kind\n[table t (domain kind kinds.kind) (order kind)]\n>kind, v: int\n*, 1\n");
	EXPECT_TRUE(Mentions(refused, "'kind'")) << refused;
	EXPECT_TRUE(Mentions(refused, "members")) << refused;
	EXPECT_TRUE(Mentions(refused, "t.tabular:5")) << refused;
}

TEST(Document, AStarDefaultTakesTheExpandedKey)
{
	EXPECT_EQ(CompiledOf("[table t (domain mode idle walk) (order mode)]\n>mode, next = mode\n*,\n"),
		"mode,next\nidle,idle\nwalk,walk\n") << "next = mode reads the copy's mode, not '*'";
}

TEST(Document, AnExpandedRowRefusalNamesTheStarRowsLine)
{
	auto d = ReadDocument("[table t (domain mode idle walk) (order mode)]\n>mode, n: int\n# a note\n*, many\n", "t.tabular");
	ASSERT_TRUE(d.has_value()) << d.error().format();
	relation_store store(std::move(d->tables));
	auto loaded = LoadDocumentRows(store, *d, 1, condition_reader());
	ASSERT_FALSE(loaded.has_value());
	EXPECT_EQ(Where(loaded.error()), "t.tabular:4") << loaded.error().format();
}

TEST(Document, AFragmentAppendsRowsInSectionOrder)
{
	EXPECT_EQ(CompiledOf("[table t (pick none)]\nk: int, v: int\n1, 10\n"
		"[fragment t]\nk, v\n2, 20\n"
		"[table u (pick none)]\nk: int\n"
		"[fragment t]\nv, k\n30, 3\n"),
		"k,v\n1,10\n2,20\n3,30\n");
	EXPECT_EQ(CompiledOf("[table t (pick none)]\nk: int, v: int\n[fragment t]\nk, v\n2, 20\n"), "k,v\n2,20\n")
		<< "a table with no rows of its own takes a fragment's";
}

TEST(Document, AFragmentConstantCopiesPerListedValue)
{
	EXPECT_EQ(CompiledOf("[table t (pick none) (order k mode)]\n>k: int, >mode, v: int\n"
		"[fragment t (mode idle walk)]\nk, v\n1, 10\n2, 20\n"),
		"k,mode,v\n1,idle,10\n1,walk,10\n2,idle,20\n2,walk,20\n");
	EXPECT_EQ(CompiledOf("[table b (domain mode idle walk) (order mode)]\n>mode, next\n[fragment b (mode *)]\nnext\nrest\n"),
		"mode,next\nidle,rest\nwalk,rest\n") << "(mode *) copies per value as a * cell does";
	std::string const both = RefusalOf("[table t (pick none) (order k mode)]\n>k: int, >mode, v: int\n"
		"[fragment t (mode idle)]\nmode, k, v\nwalk, 1, 10\n");
	EXPECT_TRUE(Mentions(both, "'mode'")) << both;
	EXPECT_TRUE(Mentions(both, "t.tabular:3")) << both;
	std::string const unknown = RefusalOf("[table t (pick none)]\nk: int\n[fragment t (nope 1)]\nk\n1\n");
	EXPECT_TRUE(Mentions(unknown, "'nope'")) << unknown;
}

TEST(Document, AFragmentLeavesOutColumnsBlankThenDefaulted)
{
	constexpr std::string_view kFile = "[table t (input hp double 0..1) (order mode)]\n>mode, hp: band, next = mode, command\n"
		"idle, , , (stand)\n"
		"[fragment t]\nmode\nwalk\n";
	EXPECT_EQ(CompiledOf(kFile), "mode,hp,next,command\nidle,0..1,idle,(stand)\nwalk,0..1,walk,\n");
	auto d = ReadDocument(kFile, "t.tabular");
	ASSERT_TRUE(d.has_value()) << d.error().format();
	relation_store store(std::move(d->tables));
	ASSERT_EQ(Failure(LoadDocumentRows(store, *d, 1, condition_reader())), "");
	EXPECT_EQ(store.RowCount(0), 2u);
}

TEST(Document, AFragmentHeaderNamesOnlyDeclaredColumns)
{
	for (std::string_view header : {"k, nope", "k, v: int", ">k, v", "k, v = 1", "k, k"})
	{
		std::string const refused = RefusalOf(cc(std::string("[table t (pick none)]\nk: int, v: int\n[fragment t]\n"),
			header, "\n1, 2\n"));
		EXPECT_TRUE(Mentions(refused, "fragment")) << header << ": " << refused;
		EXPECT_TRUE(Mentions(refused, "t.tabular:4")) << header << ": " << refused;
	}
}

TEST(Document, AFragmentBeforeItsTableIsRefused)
{
	std::string const before = RefusalOf("[fragment t]\nk\n1\n[table t (pick none)]\nk: int\n");
	EXPECT_TRUE(Mentions(before, "'t'")) << before;
	EXPECT_TRUE(Mentions(before, "t.tabular:1")) << before;
	std::string const unknown = RefusalOf("[table t (pick none)]\nk: int\n[fragment u]\nk\n1\n");
	EXPECT_TRUE(Mentions(unknown, "'u'")) << unknown;
	std::string const grid = RefusalOf("[grid g (rows r int) (columns c int) (cells int)]\n,1\n1,2\n[fragment g]\nr\n1\n");
	EXPECT_TRUE(Mentions(grid, "grid")) << grid;
	std::string const pivot = RefusalOf("[table p (input req flags) (order label) (pivot req clip: string)]\n>label: int, None\n"
		"[fragment p]\nlabel\n1\n");
	EXPECT_TRUE(Mentions(pivot, "pivot")) << pivot;
}

TEST(Document, AFragmentRowRefusalNamesItsOwnLine)
{
	auto d = ReadDocument("[table t (pick none)]\nk: int, v: int\n1, 10\n[fragment t]\nk, v\n# a note\n2, many\n", "t.tabular");
	ASSERT_TRUE(d.has_value()) << d.error().format();
	relation_store store(std::move(d->tables));
	auto loaded = LoadDocumentRows(store, *d, 1, condition_reader());
	ASSERT_FALSE(loaded.has_value());
	EXPECT_EQ(Where(loaded.error()), "t.tabular:7") << loaded.error().format();
}
