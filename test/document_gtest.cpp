#include <gtest/gtest.h>
#include "tabular/cell_rows.h"
#include "tabular/document.h"
#include "tabular/grid.h"
#include "tabular/machine.h"
#include "tabular/pick.h"
#include "cityhash/src/city.h"

#include <array>
#include <functional>
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
  (key package hash)
  (column section hash)
  (column x double)
  (column y double)
  (pick none)
  (order section package unique)]
[table rules
  (column when condition)
  (column then command)
  (column mask flags)
  (column count int)
  (pick none)]
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
	auto s = ReadSchema("[table t (column a int) (pick none) (order b)]", "t.tabular");
	ASSERT_FALSE(s.has_value());
	EXPECT_TRUE(Mentions(s.error().format(), "'b'")) << s.error().format();
	EXPECT_TRUE(Mentions(s.error().format(), "t.tabular")) << s.error().format();
}

TEST(Document, AnUnknownTypeIsRefused)
{
	auto s = ReadSchema("[table t (column a vec3) (pick none)]", "t.tabular");
	ASSERT_FALSE(s.has_value());
	EXPECT_TRUE(Mentions(s.error().format(), "vec3")) << s.error().format();
}

TEST(Document, AMisspelledOptionIsRefusedNotIgnored)
{
	auto s = ReadSchema("[table t (column a int) (pick none) (uniqe a)]", "t.tabular");
	ASSERT_FALSE(s.has_value());
	EXPECT_TRUE(Mentions(s.error().format(), "uniqe")) << s.error().format();

	// Read as a column name, a misspelled flag is a missing column, not a quiet
	// non-unique table.
	auto flag = ReadSchema("[table t (column a int) (pick none) (order a uniqe)]", "t.tabular");
	ASSERT_FALSE(flag.has_value());
	EXPECT_TRUE(Mentions(flag.error().format(), "uniqe")) << flag.error().format();
}

TEST(Document, MissingRequiredPartsAreRefused)
{
	EXPECT_FALSE(ReadSchema("[table (column a int) (pick none)]", "t.tabular").has_value());
	EXPECT_FALSE(ReadSchema("[table]", "t.tabular").has_value());
	EXPECT_FALSE(ReadSchema("[table \"\" (column a int) (pick none)]", "t.tabular").has_value());
	EXPECT_FALSE(ReadSchema("[table t (pick none)]", "t.tabular").has_value());
	EXPECT_FALSE(ReadSchema("[table t (column a) (pick none)]", "t.tabular").has_value());
	EXPECT_FALSE(ReadSchema("[table t (key) (pick none)]", "t.tabular").has_value());
	EXPECT_FALSE(ReadSchema("[something t]", "t.tabular").has_value());
}

TEST(Document, CoreRefusalsCarryTheLabel)
{
	// Two tables named t: BuildSchema refuses it, and the label says which file.
	auto s = ReadSchema(R"(
[table t (column a int) (pick none)]
[table t (column a int) (pick none)]
)", "dup.tabular");
	ASSERT_FALSE(s.has_value());
	EXPECT_TRUE(Mentions(s.error().format(), "dup.tabular")) << s.error().format();
	EXPECT_TRUE(Mentions(s.error().format(), "'t'")) << s.error().format();
}

TEST(Document, MalformedBracketsAreRefused)
{
	auto s = ReadSchema("[table t (column a int\n", "bad.tabular");
	ASSERT_FALSE(s.has_value());
	EXPECT_TRUE(Mentions(s.error().format(), "bad.tabular")) << s.error().format();
}

TEST(Document, ReadsTheVec2Type)
{
	auto s = ReadSchema("[table t (column at vec2) (pick none)]", "t.tabular");
	ASSERT_TRUE(s.has_value()) << s.error().format();
	EXPECT_EQ(s->tables()[0].columns[0].type, column_type::Vec2);
}

TEST(Document, ReadsTheStringType)
{
	auto s = ReadSchema("[table t (key n string) (pick none)]", "t.tabular");
	ASSERT_TRUE(s.has_value()) << s.error().format();
	EXPECT_EQ(s->tables()[0].columns[0].type, column_type::String);
}

TEST(Document, APickedTableIsDeclaredByName)
{
	auto s = ReadSchema(R"(
[table phase
  (pick or bits)
  (input t double)
  (key clip int)
  (column bits flags)
  (column window band (input t))
  (order clip)]
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
	constexpr std::string_view kBody = R"(
  (key clip int (domain rows))
  (column v int)
  (order clip))";
	auto none = ReadSchema(std::string("[table t") + std::string(kBody) + "]", "t.tabular");
	ASSERT_TRUE(none.has_value()) << none.error().format();
	EXPECT_EQ(none->tables()[0].pick, pick_policy::Best);

	auto spelled = ReadSchema(std::string("[table t (pick best)") + std::string(kBody) + "]", "t.tabular");
	ASSERT_TRUE(spelled.has_value()) << spelled.error().format();
	EXPECT_EQ(spelled->tables()[0].pick, pick_policy::Best);

	auto explicitNone = ReadSchema("[table t (pick none) (column v int)]", "t.tabular");
	ASSERT_TRUE(explicitNone.has_value()) << explicitNone.error().format();
	EXPECT_EQ(explicitNone->tables()[0].pick, pick_policy::None);
}

TEST(Document, EveryFileTableIsReadOnly)
{
	auto s = ReadSchema(R"(
[table a (pick none) (column v int)]
[table b (pick all) (key clip int) (order clip)]
[table c (key clip int (domain rows)) (order clip)]
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
	auto s = ReadSchema("[table t (column a int) (pick none) (mutable)]", "t.tabular");
	ASSERT_FALSE(s.has_value());
	EXPECT_TRUE(Mentions(s.error().format(), "mutable")) << s.error().format();
	EXPECT_FALSE(ReadSchema("[table t (column a int) (pick none) (read_only)]", "t.tabular").has_value());
	EXPECT_FALSE(ReadSchema("[table t (column a int) (pick none) mutable]", "t.tabular").has_value());
}

TEST(Document, AnUndeclaredInputOrOutputIsRefused)
{
	auto input = ReadSchema("[table t (pick none) (column window band (input speed))]", "t.tabular");
	ASSERT_FALSE(input.has_value());
	EXPECT_TRUE(Mentions(input.error().format(), "input 'speed'")) << input.error().format();

	auto output = ReadSchema("[table t (pick or bits) (column clip int)]", "t.tabular");
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

	auto both = ReadSchema("[grid g (rows r int) (columns c int) (cells int) (column x int)]", "g.tabular");
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
	auto input = ReadSchema("[table t (pick none) (input mode flags) (column req flags (input \"\"))]", "t.tabular");
	ASSERT_FALSE(input.has_value()) << "a flags column with input \"\" became unranked";
	EXPECT_TRUE(Mentions(input.error().format(), "input ''")) << input.error().format();

	auto output = ReadSchema("[table t (pick or \"\") (key clip int) (order clip)]", "t.tabular");
	ASSERT_FALSE(output.has_value());
	EXPECT_TRUE(Mentions(output.error().format(), "output ''")) << output.error().format();

	auto suffix = ReadSchema("[table t (pick none) (column a int) (suffix \"\")]", "t.tabular");
	ASSERT_FALSE(suffix.has_value());
	EXPECT_TRUE(Mentions(suffix.error().format(), "suffix")) << suffix.error().format();
}

TEST(Document, ABandIsNamedInRefusals)
{
	auto s = ReadSchema("[table t (pick none) (input t double) (key window band (input t))]", "t.tabular");
	ASSERT_FALSE(s.has_value());
	EXPECT_TRUE(Mentions(s.error().format(), "band")) << s.error().format();
	EXPECT_FALSE(Mentions(s.error().format(), "unknown type")) << s.error().format();
}

TEST(Document, AnInputReadsItsRange)
{
	auto s = ReadSchema(R"(
[table phase
  (pick or bits)
  (input t double -0.5..2)
  (key clip int)
  (column bits flags)
  (column window band (input t))
  (order clip)]
)", "phase.tabular");
	ASSERT_TRUE(s.has_value()) << s.error().format();
	input_def const& t = s->tables()[0].inputs[0];
	EXPECT_TRUE(t.ranged);
	EXPECT_EQ(t.lo, -0.5f);
	EXPECT_EQ(t.hi, 2.0f);

	auto bad = ReadSchema("[table phase (pick none) (input t double \"0-1\") (column clip int)]", "phase.tabular");
	ASSERT_FALSE(bad.has_value());
	EXPECT_TRUE(Mentions(bad.error().format(), "written lo..hi")) << bad.error().format();
}

TEST(Document, EveryOptionIsRead)
{
	auto s = ReadSchema(R"(
[table keys
  (key a hash (domain (members x y)))
  (key b hash fallthrough)
  (key n int (domain (members 1 2)))
  (column v double)
  (order a b n unique)
  (suffix then)]
[table foreign
  (key f hash (domain (from keys a)))
  (column v int)
  (order f)]
[table bands
  (input t double 0..1 cyclic)
  (input m flags)
  (key clip int)
  (column win band (input t) edge)
  (pick all)
  (order clip)]
[table ors
  (key clip int)
  (column bits flags)
  (pick or bits)
  (order clip)]
[table weights
  (key clip int)
  (column w double)
  (pick weighted w)
  (order clip)]
[table pivoted
  (input mode flags)
  (key label int (domain rows))
  (column mood flags (input mode))
  (column cell string)
  (order label)
  (pivot mood cell)]
[table ranked
  (input m flags)
  (key clip int (domain rows))
  (column rank flags (input m))
  (order clip)]
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
	auto pick = ReadSchema("[table t (column a int) (pick best) (pick best)]", "t.tabular");
	ASSERT_FALSE(pick.has_value());
	EXPECT_TRUE(Mentions(pick.error().format(), "pick")) << pick.error().format();

	auto order = ReadSchema("[table t (column a int) (pick none) (order a) (order a)]", "t.tabular");
	ASSERT_FALSE(order.has_value());
	EXPECT_TRUE(Mentions(order.error().format(), "order")) << order.error().format();

	auto suffix = ReadSchema("[table t (column a int) (pick none) (suffix x) (suffix y)]", "t.tabular");
	ASSERT_FALSE(suffix.has_value());
	EXPECT_TRUE(Mentions(suffix.error().format(), "suffix")) << suffix.error().format();

	auto domain = ReadSchema("[table t (key a hash (domain rows) (domain rows)) (column v int)]", "t.tabular");
	ASSERT_FALSE(domain.has_value());
	EXPECT_TRUE(Mentions(domain.error().format(), "domain")) << domain.error().format();

	auto grid = ReadSchema("[grid g (rows r int) (rows r int) (columns c int) (cells int)]", "g.tabular");
	ASSERT_FALSE(grid.has_value());
	EXPECT_TRUE(Mentions(grid.error().format(), "rows")) << grid.error().format();
}

TEST(Document, OptionOnWrongHeadIsRefused)
{
	auto table = ReadSchema("[table t (column a int) (pick none) (cells int)]", "t.tabular");
	ASSERT_FALSE(table.has_value());
	EXPECT_TRUE(Mentions(table.error().format(), "cells")) << table.error().format();

	auto grid = ReadSchema("[grid g (rows r int) (columns c int) (cells int) (pick all)]", "g.tabular");
	ASSERT_FALSE(grid.has_value());
	EXPECT_TRUE(Mentions(grid.error().format(), "pick")) << grid.error().format();
}

TEST(Document, ABadPickShapeIsRefused)
{
	EXPECT_FALSE(ReadSchema("[table t (column a int) (pick)]", "t.tabular").has_value());
	EXPECT_FALSE(ReadSchema("[table t (column a int) (pick sorted)]", "t.tabular").has_value());
	EXPECT_FALSE(ReadSchema("[table t (column a int) (pick or)]", "t.tabular").has_value());
	EXPECT_FALSE(ReadSchema("[table t (column a int) (pick all bits)]", "t.tabular").has_value());
	EXPECT_FALSE(ReadSchema("[table t (column a int) (pick weighted)]", "t.tabular").has_value());
}

TEST(Document, AFallThroughKeyDeclaresNoDomain)
{
	auto s = ReadSchema("[table t (key a hash (domain rows) fallthrough) (column v int)]", "t.tabular");
	ASSERT_FALSE(s.has_value());
	EXPECT_TRUE(Mentions(s.error().format(), "falls through")) << s.error().format();
}

TEST(Document, RefusalNamesFileLine)
{
	auto s = ReadSchema(
		"[table t\n"
		"  (column a int)\n"
		"  (column b int)\n"
		"  # a note\n"
		"  (pick none)\n"
		"  (order a)\n"
		"  (uniqe a)]\n", "t.tabular");
	ASSERT_FALSE(s.has_value());
	EXPECT_EQ(Where(s.error()), "t.tabular:7") << s.error().format();
	EXPECT_FALSE(Mentions(s.error().message, "t.tabular")) << "the message carries no position";
	EXPECT_FALSE(Mentions(s.error().message, "line")) << s.error().message;
}

TEST(Document, RowsAreKeptForTablesWithBodies)
{
	auto d = ReadDocument(
		"[table a (pick none) (column v int)]\n"
		"v\n"
		"1\n"
		"[table b (pick none) (column v int)]\n"
		"# no rows here\n"
		"[table c (pick none) (column v int)]\n"
		"v\n"
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
	auto rows = ReadSchema("[table a (pick none) (column v int)]\nv\n1\n", "t.tabular");
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
  (key mode hash (domain rows))
  (column next hash)
  (column steps int)
  (order mode)
  (suffix command)]
[table brain_mod
  (key mode hash (domain rows))
  (column next hash)
  (order mode)
  (suffix command)]
[table label
  (key mode hash (domain rows))
  (column code int)
  (order mode)]
[table tally
  (key level int (domain rows))
  (column count int)
  (order level)]
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
	"[table t (key mode hash (domain rows)) (column next hash) (order mode)]\n"
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
		"[table tally (key level int (domain rows)) (column count int) (order level)]\n"
		"[machine m (state count int (initial 0))]\n"
		"list,from,where,given,set,emit,overrides\n"
		"step,tally,level=(literal 3),,count=count,,\n", "t.tabular");
	ASSERT_TRUE(numbered.has_value()) << numbered.error().format();
	EXPECT_EQ(numbered->machines()[0].step[0].where[0].from.literal.i, 3);

	auto quoted = ReadDocument(
		"[table t (key mode hash (domain rows)) (column next hash) (order mode)]\n"
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
	std::string const tableLine = text.substr(0, text.find('\n') + 1);
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
	EXPECT_TRUE(Mentions(d.error().format(), "t.tabular:4")) << d.error().format();

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
	EXPECT_TRUE(Mentions(d.error().format(), "t.tabular:4")) << d.error().format();
}

TEST(Document, WrongClauseHeaderIsRefused)
{
	std::string swapped(kSmall);
	swapped.replace(swapped.find("where,given"), 11, "given,where");
	auto d = ReadDocument(swapped, "t.tabular");
	ASSERT_FALSE(d.has_value());
	EXPECT_TRUE(Mentions(d.error().format(), "list, from, where, given, set, emit, overrides")) << d.error().format();
	EXPECT_TRUE(Mentions(d.error().format(), "t.tabular:3")) << d.error().format();

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
	EXPECT_TRUE(Mentions(d.error().format(), "t.tabular:5")) << d.error().format();
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
	EXPECT_TRUE(Mentions(d.error().format(), "t.tabular:2")) << d.error().format();

	auto twice = ReadDocument(
		"[table t (key mode hash (domain rows)) (column next hash) (order mode)]\n"
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
  (key label int (domain rows))
  (column v int)
  (order label)]
label, v
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
  (input mode flags)
  (key label int (domain rows))
  (column req flags (input mode))
  (column clip string)
  (order label)
  (pivot req clip)]
label,None,Injured,Crouch,Injured|Crouch
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
		"  (key label int (domain rows))\n"
		"  (column v int)\n"
		"  (order label)]\n"
		"label,v\n"
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
  (key label int (domain rows))
  (column v int)
  (order label)]
label, v
1, 10
[table second
  (key label int (domain rows))
  (column v int)
  (order label)]
label, v
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
[table first (pick none) (column v int)]
[table second
  (key label int (domain rows))
  (column v int)
  (order label)]
label, v
1, 10
)", "t.tabular");
	ASSERT_TRUE(d.has_value()) << d.error().format();

	// Fewer tables than the document's body points at.
	auto fewer = ReadSchema("[table first (pick none) (column v int)]", "other.tabular");
	ASSERT_TRUE(fewer.has_value()) << fewer.error().format();
	relation_store small(std::move(*fewer));
	auto tooFew = LoadDocumentRows(small, *d, 1, condition_reader());
	ASSERT_FALSE(tooFew.has_value());
	EXPECT_TRUE(Mentions(tooFew.error().format(), "'second'")) << tooFew.error().format();

	// The same count, another table at that index.
	auto renamed = ReadSchema(R"(
[table first (pick none) (column v int)]
[table other
  (key label int (domain rows))
  (column v int)
  (order label)])", "other.tabular");
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
	EXPECT_TRUE(Mentions(d.error().format(), "t.tabular:4")) << d.error().format();

	// A refusal about the machine itself, with no clause in it, names its bracket.
	std::string initial(kSmall);
	initial.replace(initial.find("(initial a)"), 11, "(initial c)");
	auto machine = ReadDocument(initial, "t.tabular");
	ASSERT_FALSE(machine.has_value());
	EXPECT_TRUE(Mentions(machine.error().format(), "t.tabular:2")) << machine.error().format();
}

TEST(Document, QuotedNamesAreRefused)
{
	auto refused = [](std::string_view text)
	{
		return !ReadDocument(text, "t.tabular").has_value();
	};
	EXPECT_TRUE(refused("[table \"my table\" (pick none) (column a int)]"));
	EXPECT_TRUE(refused("[table t (pick none) (column \"my col\" int)]"));
	EXPECT_TRUE(refused("[table t (pick none) (key \"k\" int)]"));
	EXPECT_TRUE(refused("[table t (pick none) (input \"in put\" double) (column a int)]"));
	EXPECT_TRUE(refused("[grid \"g g\" (rows r int) (columns c int) (cells int)]"));
	EXPECT_TRUE(refused("[grid g (rows \"r\" int) (columns c int) (cells int)]"));
	EXPECT_TRUE(refused("[table t (pick none) (column a int) (suffix \"then it\")]"));
	EXPECT_TRUE(refused("[machine \"m m\"]"));
	EXPECT_TRUE(refused("[machine m (state \"mo de\" int (initial 0))]"));
	EXPECT_TRUE(refused("[machine m (input \"in put\" double)]"));

	auto d = ReadDocument("[table \"my table\" (pick none) (column a int)]", "t.tabular");
	ASSERT_FALSE(d.has_value());
	EXPECT_TRUE(Mentions(d.error().format(), "my table")) << d.error().format();
	EXPECT_TRUE(Mentions(d.error().format(), "t.tabular:1")) << d.error().format();
	EXPECT_TRUE(ReadDocument("[table t (pick none) (column a int)]", "t.tabular").has_value());
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
		EXPECT_EQ(Count(all, "t.tabular:4"), 1u) << bad << "\n" << all;
	}
}

TEST(Document, SchemaRefusalNamesTheTableLine)
{
	auto s = ReadSchema(
		"# two tables\n"
		"[table fine (pick none) (column v int)]\n"
		"\n"
		"[table keyed\n"
		"  # no domain on the key\n"
		"  (key k int)\n"
		"  (column v int)\n"
		"  (order k)]\n", "t.tabular");
	ASSERT_FALSE(s.has_value());
	EXPECT_TRUE(Mentions(s.error().format(), "t.tabular:4")) << s.error().format();
	EXPECT_TRUE(Mentions(s.error().format(), "keyed")) << s.error().format();
}

TEST(Document, ADomainCycleNamesALine)
{
	auto s = ReadSchema(
		"[table fine (pick none) (column v int)]\n"
		"[table a (key k int (domain (from b k))) (column v int) (order k)]\n"
		"[table b (key k int (domain (from a k))) (column v int) (order k)]\n", "t.tabular");
	ASSERT_FALSE(s.has_value());
	EXPECT_TRUE(Mentions(s.error().format(), "cycle")) << s.error().format();
	EXPECT_TRUE(Mentions(s.error().format(), "t.tabular:2")) << s.error().format();
}

TEST(Document, AnUnclosedQuoteInABodyDoesNotSwallowTheNextSection)
{
	auto d = ReadDocument("[table t (key k hash (domain rows)) (order k)]\nk\n\"open\n[table u (column v int) (pick none)]\n", "t.tabular");
	ASSERT_FALSE(d.has_value());
	EXPECT_EQ(Where(d.error()), "t.tabular:3") << d.error().format();
}

TEST(Document, ATableWithOnlyAHeaderLineHasNoRows)
{
	constexpr std::string_view headerOnly = "[table t (key k hash (domain rows)) (column v int) (order k)]\nk,v\n# a note\n,\n\n";
	auto s = ReadSchema(headerOnly, "t.tabular");
	EXPECT_TRUE(s.has_value()) << (s ? std::string() : s.error().format());
	auto d = ReadDocument(headerOnly, "t.tabular");
	ASSERT_TRUE(d.has_value());
	EXPECT_TRUE(d->rows().empty());

	// A comment is a row whose first unquoted character is '#': a comma before
	// it makes a row, as csv_text reads it.
	auto row = ReadSchema("[table t (key k hash (domain rows)) (column v int) (order k)]\nk,v\n,# x\n", "t.tabular");
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
  (key mode hash (domain (members idle walk)))
  (column next hash)
  (order mode)
  (suffix command)]
mode,next,command
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

// ---- Host sections --------------------------------------------------------------

namespace
{

// Line 1 is [class Grazer], line 2 [messages], whose body starts on line 3.
constexpr std::string_view kHosted =
	"[class Grazer]\n"
	"[messages]\n"
	"message,from,p1,p2\n"
	"Eat,,amount: int,\n"
	"[table t (key mode hash (domain rows)) (column next hash) (order mode)]\n"
	"mode,next\n"
	"a,b\n"
	"b,a\n"
	"[machine m (state mode hash (members a b) (initial a))]\n"
	"list,from,where,given,set,emit,overrides\n"
	"step,t,mode=state.mode,,mode=next,,\n";

constexpr std::array<std::string_view, 2> kHostHeads{"class", "messages"};

}

TEST(Document, ClaimedHeadsPassThroughUnread)
{
	std::string buffer(kHosted);
	auto d = ReadDocument(buffer, "h.tabular", kHostHeads);
	// The views are into the document's copy, not the caller's buffer.
	buffer.assign(buffer.size(), 'x');
	ASSERT_TRUE(d.has_value()) << d.error().format();
	std::span<host_section const> const hosts = d->host_sections();
	ASSERT_EQ(hosts.size(), 2u);

	EXPECT_EQ(hosts[0].head, "class");
	ASSERT_EQ(hosts[0].words.size(), 1u);
	EXPECT_EQ(hosts[0].words[0], "Grazer");
	EXPECT_TRUE(hosts[0].body.empty()) << hosts[0].body;
	EXPECT_EQ(hosts[0].line, 1u);

	EXPECT_EQ(hosts[1].head, "messages");
	EXPECT_TRUE(hosts[1].words.empty());
	EXPECT_EQ(hosts[1].line, 2u);
	EXPECT_EQ(hosts[1].body_line, 3u);
	EXPECT_TRUE(Mentions(std::string(hosts[1].body), "Eat,,amount: int,")) << hosts[1].body;
	EXPECT_FALSE(Mentions(std::string(hosts[1].body), "[table")) << "the body ends at the next bracket";

	// The table and the machine read as they would without the host sections.
	EXPECT_EQ(d->tables.tables().size(), 1u);
	EXPECT_EQ(d->rows().size(), 1u);
	EXPECT_EQ(d->machines().size(), 1u);
}

TEST(Document, UnclaimedHeadIsStillRefused)
{
	auto d = ReadDocument(kHosted, "h.tabular");
	ASSERT_FALSE(d.has_value());
	EXPECT_TRUE(Mentions(d.error().message, "unknown bracket 'class'")) << d.error().format();
	EXPECT_EQ(Where(d.error()), "h.tabular:1") << d.error().format();

	// Claiming one head does not claim the other.
	constexpr std::array<std::string_view, 1> classOnly{"class"};
	auto partly = ReadDocument(kHosted, "h.tabular", classOnly);
	ASSERT_FALSE(partly.has_value());
	EXPECT_TRUE(Mentions(partly.error().message, "unknown bracket 'messages'")) << partly.error().format();
	EXPECT_EQ(Where(partly.error()), "h.tabular:2") << partly.error().format();
}

TEST(Document, AClaimedHeadTakesOnlyBareWords)
{
	auto list = ReadDocument("# note\n[class (Grazer)]\n", "h.tabular", kHostHeads);
	ASSERT_FALSE(list.has_value());
	EXPECT_TRUE(Mentions(list.error().message, "a list")) << list.error().format();
	EXPECT_EQ(Where(list.error()), "h.tabular:2") << list.error().format();

	auto quoted = ReadDocument("[class \"Grazer\"]\n", "h.tabular", kHostHeads);
	ASSERT_FALSE(quoted.has_value());
	EXPECT_TRUE(Mentions(quoted.error().message, "a quoted string")) << quoted.error().format();
}

TEST(Document, ReadCellsUndoesQuotes)
{
	auto cells = ReadCells("a,\"b,\"\"c\"\"\"\n# note\n\nd\n", "t", 7);
	ASSERT_TRUE(cells.has_value()) << cells.error().format();
	ASSERT_EQ(cells->rows(), 2u);
	EXPECT_EQ(cells->cell(0, 0), "a");
	EXPECT_EQ(cells->cell(1, 0), "b,\"c\"");
	EXPECT_EQ(cells->used_width(0), 2u);
	EXPECT_EQ(cells->cell(0, 1), "d");
	EXPECT_EQ(cells->line_of_row(0), 7u);
	EXPECT_EQ(cells->line_of_row(1), 10u);

	auto open = ReadCells("a\n\"never closed\n", "t", 7);
	ASSERT_FALSE(open.has_value());
	EXPECT_EQ(Where(open.error()), "t:8") << open.error().format();
}
