#include <gtest/gtest.h>
#include "../src/machine_internal.h"
#include "../src/relation_internal.h"
#include "tabular/machine.h"
#include "tabular/relation.h"
#include "tabular/rows_csv.h"
#include "tabular/schema_yaml.h"
#include "cityhash/src/city.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace tabular;

// Machines: create_machine, its YAML, the definition-build refusals and
// Prepare's proof by layout over each clause's key domain. The worked example
// is the grazer of the remainder spec (§4), adapted to what the schema and CSV
// front ends read today (see GrazerTables).

namespace
{

template<typename T>
std::string RefusalOf(tl::expected<T, ErrorContext> const& e)
{
	return e ? std::string("(accepted)") : e.error().format();
}

bool Mentions(std::string const& message, std::string_view text)
{
	return message.find(text) != std::string::npos;
}

std::string H(std::string_view text)
{
	return std::to_string(static_cast<int64_t>(CityHash32(text.data(), text.size())));
}

int64_t Hash(std::string_view text)
{
	return static_cast<int64_t>(CityHash32(text.data(), text.size()));
}

// The spec's tables, with what today's front ends need and the spec's sketch
// leaves out: the inputs a band and a ranking flags column read, each key's
// domain, the order a pick's keys lead, and no `writes:` (a clause's set says
// which state column a value writes). The message key declares
// `fallthrough: true`: a blank cell is the blank run's, and a value no row
// keys on falls through to it. The host fills the message input for a step.
constexpr std::string_view kGrazerTables = R"(
tables:
  - name: grazer_brain
    read_only: true
    pick: best
    inputs:
      - {name: energy, type: double, range: 0..1}
      - {name: food_near, type: flags}
    columns:
      - {name: mode,    type: hash, key: true, domain: rows}
      - {name: message, type: hash, key: true, fallthrough: true}
      - {name: energy,  type: band, input: energy}
      - {name: near,    type: flags, input: food_near}
      - {name: when,    type: condition}
      - {name: next,    type: hash}
    order_by: {columns: [mode, message]}
  - name: grazer_enter
    read_only: true
    pick: best
    columns:
      - {name: mode, type: hash, key: true, domain: rows}
    order_by: {columns: [mode]}
    suffix: command
  - name: tally
    read_only: true
    pick: best
    columns:
      - {name: level, type: int, key: true, domain: rows}
      - {name: count, type: int}
    order_by: {columns: [level]}
  - name: kinds
    columns:
      - {name: kind, type: hash, key: true}
)";

constexpr std::string_view kGrazerMachine = R"(
machines:
  - name: grazer
    state:
      - {name: mode, type: hash, members: [idle, walk, eat, panic], initial: idle}
    inputs:
      - {name: energy, type: double, range: 0..1}
      - {name: food_near, type: flags}      # host wire: bit 0 set when food is in reach
      - {name: message, type: hash}         # 0 on a step with no event, filled by the host
    step:
      - from:  grazer_brain
        where: {mode: state.mode, message: input.message}
        given: {energy: input.energy, food_near: input.food_near}
        set:   {mode: next}
    entry:
      - from:  grazer_enter
        where: {mode: state.mode}
        emit:  command
)";

// The spec's rows, as written: a message no row of a mode keys on falls
// through to that mode's blank run, so nothing fills in the other pairs.
constexpr std::string_view kBrainRows = R"(# grazer_brain
mode,  message, energy,  near, when,        next
idle,      ,    0..0.3,  1,    ,            eat
idle,      ,    0..1,    0,    ,            walk
walk,      ,    0..1,    0,    ,            idle
eat,       ,    0..1,    0,    ,            idle
idle,  scared,  0..1,    0,    ,            panic
walk,  scared,  0..1,    0,    ,            panic
eat,   scared,  0..1,    0,    ,            panic
panic,     ,    0..1,    0,    ,            panic
panic, calm,    0..1,    0,    ,            idle
)";

constexpr std::string_view kEnterRows = R"csv(# grazer_enter
mode,  command
eat,   (anim chew), "(message food ""eat"")"
panic, (anim run), "(message this ""calm"" :delay random(0.4, 1.0))"
idle,  (anim stand)
walk,  (anim walk)
)csv";

enum grazer_table : uint32_t { Brain, Enter, Tally, Kinds };

tl::expected<schema_document, ErrorContext> Document(std::string_view tables, std::string_view machines)
{
	return DocumentFromYaml(std::string(tables) + std::string(machines), "grazer.yaml");
}

std::string Without(std::string_view text, std::string_view line)
{
	std::string out(text);
	size_t const at = out.find(line);
	EXPECT_NE(at, std::string::npos) << line;
	if (at != std::string::npos)
	{
		out.erase(at, line.size());
	}
	return out;
}

std::string Replace(std::string_view text, std::string_view from, std::string_view to)
{
	std::string out(text);
	size_t const at = out.find(from);
	EXPECT_NE(at, std::string::npos) << from;
	if (at != std::string::npos)
	{
		out.replace(at, from.size(), to);
	}
	return out;
}

// A store holding the grazer's tables, loaded with brain and enter rows.
struct grazer_fixture
{
	schema_document document;
	relation_store  store;

	grazer_fixture(schema_document&& d, std::string_view brain, std::string_view enter)
		: document(std::move(d)), store(std::move(document.tables))
	{
		EXPECT_EQ(RefusalOf(LoadCsv(store, csv_rows{Brain, 1, brain, "grazer_brain.csv"})), "(accepted)");
		EXPECT_EQ(RefusalOf(LoadCsv(store, csv_rows{Enter, 1, enter, "grazer_enter.csv"})), "(accepted)");
	}

	// A refused document already failed the test; an empty definition is
	// refused by Prepare instead of read past the end of no machines.
	create_machine const& Definition() const
	{
		static create_machine const none;
		if (document.machines().empty())
		{
			ADD_FAILURE() << "the document has no machine";
			return none;
		}
		return document.machines()[0];
	}
};

schema_document Parsed(std::string_view tables, std::string_view machines)
{
	auto doc = Document(tables, machines);
	EXPECT_TRUE(doc.has_value()) << RefusalOf(doc);
	return doc ? std::move(*doc) : schema_document{};
}

// Prepare's result, read through the access struct the library's own code uses.
machine_access::Data const& Inside(counted_ptr<Machine const> const& m)
{
	return machine_access::Of(*m);
}

}

// ---- The worked example --------------------------------------------------------

TEST(GrazerMachine, TheSpecsBrainLoadsAndPrepares)
{
	grazer_fixture f(Parsed(kGrazerTables, kGrazerMachine), kBrainRows, kEnterRows);
	auto machine = Prepare(f.Definition(), f.store);
	ASSERT_TRUE(machine.has_value()) << RefusalOf(machine);
	EXPECT_EQ((*machine)->name(), "grazer");
	EXPECT_FALSE((*machine)->Stale(f.store));

	auto const& data = Inside(*machine);
	ASSERT_EQ(data.lists[size_t(trigger::Step)].size(), 1u);
	ASSERT_EQ(data.lists[size_t(trigger::Entry)].size(), 1u);
	EXPECT_EQ(data.lists[size_t(trigger::Step)][0].tupleCount, 8u)
		<< "each member's blank run, and the four runs keyed on a message whose mode is a member";
	EXPECT_EQ(data.lists[size_t(trigger::Entry)][0].tupleCount, 4u) << "one run per member";
}

TEST(GrazerMachine, TheRunDirectoryNamesVersionRowsOfEachTuplesRun)
{
	grazer_fixture f(Parsed(kGrazerTables, kGrazerMachine), kBrainRows, kEnterRows);
	auto machine = Prepare(f.Definition(), f.store);
	ASSERT_TRUE(machine.has_value()) << RefusalOf(machine);
	detail::prepared_clause const& step = Inside(*machine).lists[size_t(trigger::Step)][0];
	ASSERT_EQ(step.runs.runs.size(), step.tupleCount);
	EXPECT_TRUE(step.runs.layout.closed.empty() && step.runs.layout.listed.empty()) << "proof-only arrays are dropped";
	size_t listedRows = 0;
	for (size_t t = 0; t < step.tupleCount; ++t)
	{
		detail::run_span const span = step.runs.runs[t];
		ASSERT_GT(span.count, 0u) << "tuple " << t << " has a run";
		uint32_t const firstCell = step.runs.layout.cellStart[span.first];
		uint32_t const lastCell  = step.runs.layout.cellStart[span.first + span.count];
		for (uint32_t c = firstCell; c < lastCell; ++c)
		{
			for (uint32_t i = step.runs.layout.listStart[c]; i < step.runs.layout.listStart[c + 1]; ++i)
			{
				uint32_t const row = step.runs.layout.lists[i];
				EXPECT_EQ(step.version->Value(row, 0)->i, step.tuples[t * 2]) << "row " << row;
				EXPECT_EQ(step.version->Value(row, 1)->i, step.tuples[t * 2 + 1]) << "row " << row;
				++listedRows;
			}
		}
	}
	EXPECT_GE(listedRows, 9u) << "every brain row is in some list";
}

TEST(GrazerMachine, LeavingOutPanicsCatchAllRowIsRefusedByTheLoad)
{
	// The spec's reading (§4): panic's calm row puts panic in mode's rows
	// domain, and a fall-through key demands the blank run of every tuple of the
	// other keys, so the load refuses before any machine is prepared.
	auto doc = Document(kGrazerTables, kGrazerMachine);
	ASSERT_TRUE(doc.has_value()) << RefusalOf(doc);
	relation_store store(std::move(doc->tables));
	std::string const brain = Without(kBrainRows, "panic,     ,    0..1,    0,    ,            panic\n");
	std::string const error = RefusalOf(LoadCsv(store, csv_rows{Brain, 1, brain, "grazer_brain.csv"}));
	EXPECT_TRUE(Mentions(error, "table 'grazer_brain' key (" + H("panic")
		+ ", 0): energy 0 has no row with no flags and no script condition; no row has this key, and the key is in the "
		"table's domain")) << error;
}

TEST(GrazerMachine, LeavingOutPanicsCatchAllRowIsRefusedByTheLoadWhenTheTableClosesMode)
{
	// The spec's reading: the table itself declares mode's members, so the load
	// refuses before any machine is prepared.
	std::string const tables = Replace(kGrazerTables, "{name: mode,    type: hash, key: true, domain: rows}",
		"{name: mode,    type: hash, key: true, domain: {members: [idle, walk, eat, panic]}}");
	auto doc = Document(tables, kGrazerMachine);
	ASSERT_TRUE(doc.has_value()) << RefusalOf(doc);
	relation_store store(std::move(doc->tables));
	std::string const brain = Without(kBrainRows, "panic,     ,    0..1,    0,    ,            panic\n");
	std::string const error = RefusalOf(LoadCsv(store, csv_rows{Brain, 1, brain, "grazer_brain.csv"}));
	EXPECT_TRUE(Mentions(error, "table 'grazer_brain' key (" + H("panic") + ", " + std::string("0")
		+ "): energy 0 has no row with no flags and no script condition; no row has this key, and the key is in the table's domain"))
		<< error;
}

TEST(GrazerMachine, AStateMemberWithNoRunIsRefused)
{
	std::string const machines = Replace(kGrazerMachine, "members: [idle, walk, eat, panic]",
		"members: [idle, walk, eat, panic, sleep]");
	// Entry answers for sleep, so the brain is what is missing it.
	grazer_fixture f(Parsed(kGrazerTables, machines), kBrainRows, std::string(kEnterRows) + "sleep, (anim sleep)\n");
	std::string const error = RefusalOf(Prepare(f.Definition(), f.store));
	EXPECT_TRUE(Mentions(error, "machine 'grazer' step clause 0 (table 'grazer_brain')")) << error;
	EXPECT_TRUE(Mentions(error, "table 'grazer_brain' key (" + H("sleep") + ", ")) << error;
	EXPECT_TRUE(Mentions(error, "no row has this key, and the key is in the clause's domain")) << error;
}

TEST(GrazerMachine, TheEntryClauseIsProvedOverTheMembersToo)
{
	grazer_fixture f(Parsed(kGrazerTables, kGrazerMachine), kBrainRows,
		Without(kEnterRows, "walk,  (anim walk)\n"));
	std::string const error = RefusalOf(Prepare(f.Definition(), f.store));
	EXPECT_TRUE(Mentions(error, "machine 'grazer' entry clause 0 (table 'grazer_enter')")) << error;
	EXPECT_TRUE(Mentions(error, "table 'grazer_enter' key (" + H("walk") + ") has no row with no script condition")) << error;
}

TEST(GrazerMachine, AClauseNamingALaterClauseIsRefusedAtDefinitionBuild)
{
	std::string const machines = Replace(kGrazerMachine, "where: {mode: state.mode, message: input.message}",
		"where: {mode: clause.1.next, message: input.message}");
	std::string const error = RefusalOf(Document(kGrazerTables, machines));
	EXPECT_TRUE(Mentions(error, "machine 'grazer' step clause 0 (table 'grazer_brain')")) << error;
	EXPECT_TRUE(Mentions(error, "key 'mode' reads clause 1, which comes later; a clause reads only the clauses before it in its list"))
		<< error;
}

TEST(GrazerMachine, AClauseNamingItselfIsRefusedAtDefinitionBuild)
{
	std::string const machines = Replace(kGrazerMachine, "where: {mode: state.mode, message: input.message}",
		"where: {mode: clause.0.next, message: input.message}");
	std::string const error = RefusalOf(Document(kGrazerTables, machines));
	EXPECT_TRUE(Mentions(error, "key 'mode' reads clause 0, which is this clause")) << error;
}

TEST(GrazerMachine, ANextStateThatIsNotAMemberIsRefused)
{
	std::string const brain = Replace(kBrainRows, "panic, calm,    0..1,    0,    ,            idle",
		"panic, calm,    0..1,    0,    ,            sleep");
	grazer_fixture    f(Parsed(kGrazerTables, kGrazerMachine), brain, kEnterRows);
	std::string const error = RefusalOf(Prepare(f.Definition(), f.store));
	EXPECT_TRUE(Mentions(error, "machine 'grazer' step clause 0 (table 'grazer_brain')")) << error;
	EXPECT_TRUE(Mentions(error, "sets state column 'mode' from column 'next' to " + H("sleep")
		+ ", which is not one of its members")) << error;
}

TEST(GrazerMachine, TwoClausesWritingOneColumnAreOrderedByPosition)
{
	// R4 as accepted: the later write wins, so nothing is refused.
	std::string const machines = Replace(kGrazerMachine, R"(    entry:)", R"(      - from:  grazer_brain
        where: {mode: state.mode, message: input.message}
        given: {energy: input.energy, food_near: input.food_near}
        set:   {mode: next}
    entry:)");
	grazer_fixture f(Parsed(kGrazerTables, machines), kBrainRows, kEnterRows);
	auto machine = Prepare(f.Definition(), f.store);
	ASSERT_TRUE(machine.has_value()) << RefusalOf(machine);
	EXPECT_EQ(Inside(*machine).lists[size_t(trigger::Step)].size(), 2u);
}

TEST(GrazerMachine, ALiteralKeyIsOneValue)
{
	// The fall-through key stays open, so the literal closes mode alone.
	std::string const machines = Replace(kGrazerMachine, "where: {mode: state.mode,", "where: {mode: {literal: idle},");
	grazer_fixture f(Parsed(kGrazerTables, machines), kBrainRows, kEnterRows);
	auto machine = Prepare(f.Definition(), f.store);
	ASSERT_TRUE(machine.has_value()) << RefusalOf(machine);
	detail::prepared_clause const& step = Inside(*machine).lists[size_t(trigger::Step)][0];
	ASSERT_EQ(step.tupleCount, 2u) << "idle's blank run and its scared run";
	for (size_t t = 0; t < step.tupleCount; ++t)
	{
		EXPECT_EQ(step.tuples[t * 2], Hash("idle"));
	}
}

TEST(GrazerMachine, ChangingATableItReadsMakesTheMachineStale)
{
	grazer_fixture f(Parsed(kGrazerTables, kGrazerMachine), kBrainRows, kEnterRows);
	auto machine = Prepare(f.Definition(), f.store);
	ASSERT_TRUE(machine.has_value()) << RefusalOf(machine);
	ASSERT_EQ(RefusalOf(LoadCsv(f.store, csv_rows{Tally, 1, "level,count\n0,1\n", "tally.csv"})), "(accepted)");
	EXPECT_FALSE((*machine)->Stale(f.store)) << "a table the machine does not read";
	ASSERT_EQ(RefusalOf(LoadCsv(f.store, csv_rows{Enter, 1, Replace(kEnterRows, "(anim stand)", "(anim sit)"),
		"grazer_enter.csv"})), "(accepted)");
	EXPECT_TRUE((*machine)->Stale(f.store));
}

// ---- Clause sources ------------------------------------------------------------

namespace
{

constexpr std::string_view kChain = R"(
tables:
  - name: mood_of
    pick: best
    columns:
      - {name: mode, type: hash, key: true, domain: rows}
      - {name: mood, type: hash}
    order_by: {columns: [mode]}
  - name: react
    pick: best
    columns:
      - {name: mood, type: hash, key: true, domain: rows}
      - {name: next, type: hash}
    order_by: {columns: [mood]}
machines:
  - name: chain
    state:
      - {name: mode, type: hash, members: [calm, alert], initial: calm}
    step:
      - from: mood_of
        where: {mode: state.mode}
      - from: react
        where: {mood: clause.0.mood}
        set: {mode: next}
)";

}

TEST(MachinePrepare, AnEarlierClauseClosesAKeyByWhatItCanAnswer)
{
	auto doc = Document(kChain, "");
	ASSERT_TRUE(doc.has_value()) << RefusalOf(doc);
	relation_store store(std::move(doc->tables));
	ASSERT_EQ(RefusalOf(LoadCsv(store, csv_rows{0, 1, "mode,mood\ncalm,happy\nalert,tense\n", "mood_of.csv"})), "(accepted)");
	// bored is never answered, so its run is outside the clause's domain.
	ASSERT_EQ(RefusalOf(LoadCsv(store, csv_rows{1, 1, "mood,next\nhappy,calm\ntense,alert\nbored,calm\n", "react.csv"})),
		"(accepted)");
	auto machine = Prepare(doc->machines()[0], store);
	ASSERT_TRUE(machine.has_value()) << RefusalOf(machine);
	detail::prepared_clause const& react = Inside(*machine).lists[size_t(trigger::Step)][1];
	EXPECT_EQ(react.tupleCount, 2u);
	std::vector<int64_t> moods(react.tuples.begin(), react.tuples.end());
	std::vector<int64_t> wanted = {Hash("happy"), Hash("tense")};
	std::sort(wanted.begin(), wanted.end());
	EXPECT_EQ(moods, wanted);
}

TEST(MachinePrepare, AValueAnEarlierClauseAnswersWithNoRunIsRefused)
{
	auto doc = Document(kChain, "");
	ASSERT_TRUE(doc.has_value()) << RefusalOf(doc);
	relation_store store(std::move(doc->tables));
	ASSERT_EQ(RefusalOf(LoadCsv(store, csv_rows{0, 1, "mode,mood\ncalm,happy\nalert,tense\n", "mood_of.csv"})), "(accepted)");
	ASSERT_EQ(RefusalOf(LoadCsv(store, csv_rows{1, 1, "mood,next\nhappy,calm\n", "react.csv"})), "(accepted)");
	std::string const error = RefusalOf(Prepare(doc->machines()[0], store));
	EXPECT_TRUE(Mentions(error, "machine 'chain' step clause 1 (table 'react')")) << error;
	EXPECT_TRUE(Mentions(error, "table 'react' key (" + H("tense")
		+ ") has no row with no script condition; no row has this key, and the key is in the clause's domain")) << error;
}

TEST(MachinePrepare, AnUnclosedSourceKeepsTheTablesDeclaredDomain)
{
	// An int state column holds any value, as an input does: neither closes the
	// key, so both keep tally's declared domain (its rows) and prove the same.
	auto doc = Document(kGrazerTables, R"(
machines:
  - name: by_state
    state:
      - {name: level, type: int, initial: 0}
    step:
      - from: tally
        where: {level: state.level}
  - name: by_input
    inputs:
      - {name: level, type: int}
    step:
      - from: tally
        where: {level: input.level}
)");
	ASSERT_TRUE(doc.has_value()) << RefusalOf(doc);
	relation_store store(std::move(doc->tables));
	ASSERT_EQ(RefusalOf(LoadCsv(store, csv_rows{Tally, 1, "level,count\n0,1\n2,5\n", "tally.csv"})), "(accepted)");
	auto byState = Prepare(doc->machines()[0], store);
	auto byInput = Prepare(doc->machines()[1], store);
	ASSERT_TRUE(byState.has_value()) << RefusalOf(byState);
	ASSERT_TRUE(byInput.has_value()) << RefusalOf(byInput);
	detail::prepared_clause const& s = Inside(*byState).lists[size_t(trigger::Step)][0];
	detail::prepared_clause const& i = Inside(*byInput).lists[size_t(trigger::Step)][0];
	EXPECT_EQ(s.tupleCount, 2u);
	EXPECT_EQ(std::vector<int64_t>(s.tuples.begin(), s.tuples.end()), (std::vector<int64_t>{0, 2}));
	EXPECT_EQ(std::vector<int64_t>(i.tuples.begin(), i.tuples.end()), (std::vector<int64_t>{0, 2}));
}

TEST(MachinePrepare, AnInputKeyOverAForeignDomainWatchesThatTable)
{
	auto doc = DocumentFromYaml(R"(
tables:
  - name: kinds
    columns:
      - {name: kind, type: hash, key: true}
  - name: reaction
    pick: best
    columns:
      - {name: kind, type: hash, key: true, domain: {table: kinds, column: kind}}
      - {name: next, type: hash}
    order_by: {columns: [kind]}
machines:
  - name: watcher
    state:
      - {name: mode, type: hash, members: [a, b], initial: a}
    inputs:
      - {name: kind, type: hash}
    step:
      - from: reaction
        where: {kind: input.kind}
        set: {mode: next}
)", "watcher.yaml");
	ASSERT_TRUE(doc.has_value()) << RefusalOf(doc);
	relation_store store(std::move(doc->tables));
	ASSERT_EQ(RefusalOf(LoadCsv(store, csv_rows{1, 1, "kind,next\nx,b\n", "reaction.csv"})), "(accepted)");
	cell const x[] = {cell::Integer(Hash("x"))};
	ASSERT_EQ(RefusalOf(store.Insert(insert_row{0, 7, x})), "(accepted)");
	auto machine = Prepare(doc->machines()[0], store);
	ASSERT_TRUE(machine.has_value()) << RefusalOf(machine);
	EXPECT_EQ(Inside(*machine).lists[size_t(trigger::Step)][0].tupleCount, 1u);
	EXPECT_FALSE((*machine)->Stale(store));

	// The domain's table changes and the clause's table does not: the proof
	// read the domain, so the machine is stale all the same.
	uint64_t const reactionBefore = store.Generation(1);
	ASSERT_EQ(RefusalOf(store.Insert(insert_row{0, 8, x})), "(accepted)");
	EXPECT_EQ(store.Generation(1), reactionBefore);
	EXPECT_TRUE((*machine)->Stale(store));
}

// ---- Definition build ----------------------------------------------------------

namespace
{

// The grazer machine with one edit, built against the grazer tables.
std::string BuildRefusal(std::string_view from, std::string_view to)
{
	return RefusalOf(Document(kGrazerTables, Replace(kGrazerMachine, from, to)));
}

}

TEST(MachineDefinition, TheGrazerIsWellFormed)
{
	auto doc = Document(kGrazerTables, kGrazerMachine);
	ASSERT_TRUE(doc.has_value()) << RefusalOf(doc);
	ASSERT_EQ(doc->machines().size(), 1u);
	create_machine const& m = doc->machines()[0];
	EXPECT_EQ(m.name, "grazer");
	ASSERT_EQ(m.state.size(), 1u);
	EXPECT_EQ(m.state[0].members.size(), 4u);
	EXPECT_EQ(m.state[0].initial.i, Hash("idle"));
	ASSERT_EQ(m.step.size(), 1u);
	ASSERT_EQ(m.step[0].where.size(), 2u);
	EXPECT_EQ(m.step[0].where[0].target, "mode");
	EXPECT_EQ(m.step[0].where[0].from.kind, source_kind::State);
	EXPECT_EQ(m.step[0].where[0].from.name, "mode");
	EXPECT_EQ(m.step[0].where[1].from.kind, source_kind::Input);
	ASSERT_EQ(m.entry.size(), 1u);
	EXPECT_EQ(m.entry[0].emit, "command");
	EXPECT_EQ(RefusalOf(CheckMachine(m, doc->tables)), "(accepted)");
}

TEST(MachineDefinition, SchemaFromYamlRefusesADocumentWithMachines)
{
	std::string const error = RefusalOf(SchemaFromYaml(std::string(kGrazerTables) + std::string(kGrazerMachine), "g.yaml"));
	EXPECT_TRUE(Mentions(error, "read it with DocumentFromYaml")) << error;
}

TEST(MachineDefinition, UnknownNamesAreRefused)
{
	EXPECT_TRUE(Mentions(BuildRefusal("from:  grazer_enter", "from:  grazer_exit"),
		"the clause reads table 'grazer_exit', which the schema does not have"));
	EXPECT_TRUE(Mentions(BuildRefusal("where: {mode: state.mode, message", "where: {mood: state.mode, message"),
		"where names column 'mood', which table 'grazer_brain' does not have"));
	EXPECT_TRUE(Mentions(BuildRefusal("given: {energy: input.energy,", "given: {vigour: input.energy,"),
		"given names input 'vigour', which table 'grazer_brain' does not declare"));
	EXPECT_TRUE(Mentions(BuildRefusal("where: {mode: state.mode, message", "where: {mode: state.mood, message"),
		"key 'mode' reads state column 'mood', which the machine does not declare"));
	EXPECT_TRUE(Mentions(BuildRefusal("message: input.message}", "message: input.msg}"),
		"key 'message' reads input 'msg', which the machine does not declare"));
	EXPECT_TRUE(Mentions(BuildRefusal("set:   {mode: next}", "set:   {mood: next}"),
		"set writes state column 'mood', which the machine does not declare"));
	EXPECT_TRUE(Mentions(BuildRefusal("set:   {mode: next}", "set:   {mode: nxt}"),
		"set reads column 'nxt', which table 'grazer_brain' does not have"));
}

TEST(MachineDefinition, EveryKeyAndInputIsBoundOnceAndOnlyKeysByWhere)
{
	EXPECT_TRUE(Mentions(BuildRefusal("where: {mode: state.mode, message: input.message}", "where: {mode: state.mode}"),
		"key 'message' of table 'grazer_brain' is not bound; where gives every key a source"));
	EXPECT_TRUE(Mentions(BuildRefusal("given: {energy: input.energy, food_near: input.food_near}",
		"given: {energy: input.energy}"),
		"input 'food_near' of table 'grazer_brain' is not bound; given gives every input of the table a source"));
	EXPECT_TRUE(Mentions(BuildRefusal("message: input.message}", "message: input.message, next: state.mode}"),
		"where binds column 'next' of table 'grazer_brain', which is not a key"));
}

TEST(MachineDefinition, ASourceOfAnotherTypeIsRefused)
{
	EXPECT_TRUE(Mentions(BuildRefusal("message: input.message}", "message: input.energy}"),
		"key 'message' is a hash, and the input 'energy' it reads is a double"));
	EXPECT_TRUE(Mentions(BuildRefusal("given: {energy: input.energy,", "given: {energy: input.food_near,"),
		"input 'energy' is a double, and the input 'food_near' it reads is a flag set"));
}

TEST(MachineDefinition, SetReadsAValueColumnOfTheStatesType)
{
	EXPECT_TRUE(Mentions(BuildRefusal("set:   {mode: next}", "set:   {mode: when}"),
		"set reads column 'when' of table 'grazer_brain', a condition; a state column is written from a value column"));
	EXPECT_TRUE(Mentions(BuildRefusal("set:   {mode: next}", "set:   {mode: near}"),
		"set writes hash state column 'mode' from column 'near', a flag set"));
}

TEST(MachineDefinition, EmitNamesACommandColumn)
{
	EXPECT_TRUE(Mentions(BuildRefusal("set:   {mode: next}", "set:   {mode: next}\n        emit:  command"),
		"the clause emits 'command', and table 'grazer_brain' has no command column or suffix to emit"));
	EXPECT_TRUE(Mentions(BuildRefusal("emit:  command", "emit:  mode"),
		"the clause emits 'mode', which is not a command column of table 'grazer_enter'"));
}

TEST(MachineDefinition, AHashStatesInitialValueIsAMember)
{
	EXPECT_TRUE(Mentions(BuildRefusal("initial: idle}", "initial: asleep}"),
		"state column 'mode' starts at " + H("asleep") + ", which is not one of its members"));
}

TEST(MachineDefinition, AMachineMemberIsHashedAsACellIs)
{
	EXPECT_TRUE(Mentions(BuildRefusal("members: [idle, walk, eat, panic], initial: idle",
		"members: [idle, n26963, n50336], initial: idle"), "'n50336' has the same hash as 'n26963'"));
}

TEST(MachineDefinition, AClauseReadsAPickedTable)
{
	std::string const error = RefusalOf(Document(kGrazerTables, R"(
machines:
  - name: lister
    step:
      - from: kinds
        where: {kind: {literal: x}}
)"));
	EXPECT_TRUE(Mentions(error, "table 'kinds' has no pick; a clause reads a picked table")) << error;
}

TEST(MachineDefinition, AClauseOnAnAllTableIsRefused)
{
	std::string const error = RefusalOf(Document(R"(
tables:
  - name: every
    read_only: true
    pick: all
    columns:
      - {name: mode, type: hash, key: true}
      - {name: next, type: hash}
    order_by: {columns: [mode]}
)", R"(
machines:
  - name: lister
    state:
      - {name: mode, type: hash, members: [a], initial: a}
    step:
      - from: every
        where: {mode: state.mode}
)"));
	EXPECT_TRUE(Mentions(error, "machine 'lister' step clause 0 (table 'every')")) << error;
	EXPECT_TRUE(Mentions(error, "table 'every' is an all table; a clause answers one value per column, and an all pick "
		"answers with every row that holds")) << error;
}

TEST(MachineDefinition, ASourceIsOneOfTheFourShapes)
{
	std::string const shape = BuildRefusal("message: input.message}", "message: message}");
	EXPECT_TRUE(Mentions(shape,
		"'message' reads 'message'; a source is state.<column>, input.<name>, clause.<n>.<column> or {literal: <text>}"))
		<< shape;
	EXPECT_TRUE(Mentions(shape, "machine 'grazer' step clause 0 (table 'grazer_brain')"))
		<< "the YAML front end and CheckMachine name a clause alike: " << shape;
	EXPECT_TRUE(Mentions(BuildRefusal("message: input.message}", "message: clause.x.next}"),
		"an earlier clause is named clause.<n>.<column>"));
	std::string const literal = BuildRefusal("message: input.message}", "message: {literal: [a]}}");
	EXPECT_TRUE(Mentions(literal, "the literal for 'message' is a sequence; a literal is a scalar")) << literal;
}

TEST(MachineDefinition, CommandStructsAreCheckedWithoutYaml)
{
	auto doc = Document(kGrazerTables, "");
	ASSERT_TRUE(doc.has_value()) << RefusalOf(doc);
	cell const        members[] = {cell::Integer(Hash("idle")), cell::Integer(Hash("walk"))};
	add_state const   state[]   = {{"mode", column_type::Hash, members, cell::Integer(Hash("idle"))}};
	set_state const   sets[]    = {{"mode", "next"}, {"mode", "next"}};
	add_input const   inputs[]  = {{"energy"}, {"food_near", column_type::Flags}, {"message", column_type::Hash}};
	source            mode;
	mode.kind = source_kind::State;
	mode.name = "mode";
	source message;
	message.kind = source_kind::Input;
	message.name = "message";
	source energy;
	energy.kind = source_kind::Input;
	energy.name = "energy";
	source near;
	near.kind = source_kind::Input;
	near.name = "food_near";
	clause_bind const where[] = {{"mode", mode}, {"message", message}};
	clause_bind const given[] = {{"energy", energy}, {"food_near", near}};
	add_clause const  step[]  = {{"grazer_brain", where, given, sets, {}}};
	create_machine    m{"grazer", state, inputs, {}, {}, step, {}};
	EXPECT_TRUE(Mentions(RefusalOf(CheckMachine(m, doc->tables)), "set writes state column 'mode' twice"));

	add_clause const once[] = {{"grazer_brain", where, given, std::span<set_state const>(sets, 1), {}}};
	m.step = once;
	EXPECT_EQ(RefusalOf(CheckMachine(m, doc->tables)), "(accepted)");

	add_state const typed[] = {{"mode", column_type::Flags, members, cell::Integer(Hash("idle"))}};
	m.state = typed;
	EXPECT_TRUE(Mentions(RefusalOf(CheckMachine(m, doc->tables)),
		"state column 'mode' is a flag set; a state column is a hash, an int or a double"));
}

// ---- Prepare: what each clause keeps and reports ---------------------------------

TEST(GrazerMachine, TheStepClauseKeepsItsAxisAndRankingInputs)
{
	grazer_fixture f(Parsed(kGrazerTables, kGrazerMachine), kBrainRows, kEnterRows);
	auto machine = Prepare(f.Definition(), f.store);
	ASSERT_TRUE(machine.has_value()) << RefusalOf(machine);
	detail::prepared_clause const& step = Inside(*machine).lists[size_t(trigger::Step)][0];
	ASSERT_EQ(step.axisInputs.size(), 1u) << "one band input, energy";
	EXPECT_EQ(step.axisInputs[0], 0u);
	EXPECT_EQ(step.rankingInput, 1u) << "near ranks by food_near";
	detail::prepared_clause const& entry = Inside(*machine).lists[size_t(trigger::Entry)][0];
	EXPECT_EQ(entry.axisInputs.size(), 0u);
	EXPECT_EQ(entry.rankingInput, UINT32_MAX);
}

TEST(GrazerMachine, PrepareReportsRunsOutsideTheClausesDomainAsOrphans)
{
	// A typo in a rows-declared key: the load cannot tell it from a value, and the
	// machine never reaches it.
	std::string const brain = std::string(kBrainRows) + "pnaic,     ,    0..1,    0,    ,            idle\n";
	grazer_fixture    f(Parsed(kGrazerTables, kGrazerMachine), brain, kEnterRows);
	std::vector<run_note> report = {run_note{Kinds, run_status::NotTotal, {}}};
	auto machine = Prepare(f.Definition(), f.store, &report);
	ASSERT_TRUE(machine.has_value()) << RefusalOf(machine);
	ASSERT_EQ(report.size(), 1u) << "cleared first, then the one orphan";
	EXPECT_EQ(report[0].table, uint32_t(Brain));
	EXPECT_EQ(report[0].status, run_status::Orphan);
	ASSERT_EQ(report[0].key.size(), 2u);
	EXPECT_EQ(report[0].key[0].i, Hash("pnaic"));
	EXPECT_EQ(report[0].key[1].i, 0);
}

TEST(GrazerMachine, ARefusedPrepareLeavesTheReportEmpty)
{
	// The entry clause notes its orphan before the step clause is refused for
	// a member the brain has no run for.
	std::string const machines = Replace(kGrazerMachine, "members: [idle, walk, eat, panic]",
		"members: [idle, walk, eat, panic, sleep]");
	grazer_fixture f(Parsed(kGrazerTables, machines), kBrainRows,
		std::string(kEnterRows) + "sleep, (anim sleep)\npnaic, (anim run)\n");
	std::vector<run_note> report;
	auto machine = Prepare(f.Definition(), f.store, &report);
	ASSERT_FALSE(machine.has_value());
	EXPECT_TRUE(report.empty()) << report.size() << " notes";
}

TEST(GrazerMachine, AClauseReachingARunTheLoadNeverProvedIsRefused)
{
	// The table closes mode without eat, so the load never checks eat's runs; the
	// machine's state has eat, so its clause reaches them, and eat with no
	// message has only a flagged row.
	std::string const tables = Replace(kGrazerTables, "{name: mode,    type: hash, key: true, domain: rows}",
		"{name: mode,    type: hash, key: true, domain: {members: [idle, walk, panic]}}");
	std::string const brain = Replace(kBrainRows, "eat,       ,    0..1,    0,    ,            idle",
		"eat,       ,    0..1,    1,    ,            idle");
	grazer_fixture    f(Parsed(tables, kGrazerMachine), brain, kEnterRows);
	std::string const error = RefusalOf(Prepare(f.Definition(), f.store));
	EXPECT_TRUE(Mentions(error, "machine 'grazer' step clause 0 (table 'grazer_brain')")) << error;
	EXPECT_TRUE(Mentions(error, "table 'grazer_brain' key (" + H("eat") + ", " + std::string("0")
		+ "): energy 0 has no row with no flags and no script condition; its first row is owner 1's row with key ("
		+ H("eat") + ", " + std::string("0") + "), flags 1, energy 0..1")) << error;
}

TEST(MachinePrepare, AnEarlierClauseAnswersOnlyFromRowsItsDomainReaches)
{
	// sleepy is no state member, so mood_of's sleepy run is never picked and its
	// bored is never answered: react need not have it.
	auto doc = Document(kChain, "");
	ASSERT_TRUE(doc.has_value()) << RefusalOf(doc);
	relation_store store(std::move(doc->tables));
	ASSERT_EQ(RefusalOf(LoadCsv(store, csv_rows{0, 1, "mode,mood\ncalm,happy\nalert,tense\nsleepy,bored\n", "mood_of.csv"})),
		"(accepted)");
	ASSERT_EQ(RefusalOf(LoadCsv(store, csv_rows{1, 1, "mood,next\nhappy,calm\ntense,alert\n", "react.csv"})), "(accepted)");
	auto machine = Prepare(doc->machines()[0], store);
	ASSERT_TRUE(machine.has_value()) << RefusalOf(machine);
	EXPECT_EQ(Inside(*machine).lists[size_t(trigger::Step)][1].tupleCount, 2u);
}

TEST(MachinePrepare, AnInputKeyOverMembersIsProvedOverEveryMember)
{
	// kinds is empty, so the load's domain is empty and c's missing run goes
	// unchecked; the clause closes mode by the state and keeps who's members.
	auto doc = DocumentFromYaml(R"(
tables:
  - name: kinds
    columns:
      - {name: kind, type: hash, key: true}
  - name: greet
    read_only: true
    pick: best
    columns:
      - {name: who,  type: hash, key: true, domain: {members: [a, b, c]}}
      - {name: mode, type: hash, key: true, domain: {table: kinds, column: kind}}
      - {name: next, type: hash}
    order_by: {columns: [who, mode]}
machines:
  - name: greeter
    state:
      - {name: mode, type: hash, members: [x], initial: x}
    inputs:
      - {name: who, type: hash}
    step:
      - from: greet
        where: {who: input.who, mode: state.mode}
        set: {mode: next}
)", "greet.yaml");
	ASSERT_TRUE(doc.has_value()) << RefusalOf(doc);
	relation_store store(std::move(doc->tables));
	ASSERT_EQ(RefusalOf(LoadCsv(store, csv_rows{1, 1, "who,mode,next\na,x,x\nb,x,x\n", "greet.csv"})), "(accepted)");
	std::string const error = RefusalOf(Prepare(doc->machines()[0], store));
	EXPECT_TRUE(Mentions(error, "table 'greet' key (" + H("c") + ", " + H("x")
		+ ") has no row with no script condition; no row has this key, and the key is in the clause's domain")) << error;
}

// ---- Literal inputs --------------------------------------------------------------

namespace
{

// The grazer's step clause on the command structs, its energy given lit.
std::string EnergyLiteralRefusal(schema const& tables, double lit)
{
	cell const      members[] = {cell::Integer(Hash("idle")), cell::Integer(Hash("walk")), cell::Integer(Hash("eat")),
		cell::Integer(Hash("panic"))};
	add_state const state[]   = {{"mode", column_type::Hash, members, cell::Integer(Hash("idle"))}};
	add_input const inputs[]  = {{"food_near", column_type::Flags}, {"message", column_type::Hash}};
	source mode;
	mode.kind = source_kind::State;
	mode.name = "mode";
	source message;
	message.kind = source_kind::Input;
	message.name = "message";
	source energy;
	energy.literal = cell::Real(lit);
	source near;
	near.kind = source_kind::Input;
	near.name = "food_near";
	clause_bind const where[] = {{"mode", mode}, {"message", message}};
	clause_bind const given[] = {{"energy", energy}, {"food_near", near}};
	add_clause const  step[]  = {{"grazer_brain", where, given, {}, {}}};
	create_machine    m{"grazer", state, inputs, {}, {}, step, {}};
	return RefusalOf(CheckMachine(m, tables));
}

}

TEST(MachineDefinition, ALiteralForARangedInputIsFiniteAndInItsRange)
{
	auto doc = Document(kGrazerTables, "");
	ASSERT_TRUE(doc.has_value()) << RefusalOf(doc);
	EXPECT_EQ(EnergyLiteralRefusal(doc->tables, 0.5), "(accepted)");
	EXPECT_EQ(EnergyLiteralRefusal(doc->tables, 1.0), "(accepted)") << "the top of the range is an input a pick clamps";
	std::string const nan = EnergyLiteralRefusal(doc->tables, std::nan(""));
	EXPECT_TRUE(Mentions(nan, "input 'energy' has range 0..1, and its literal is nan; a literal for a ranged input is "
		"finite and inside its range")) << nan;
	std::string const inf = EnergyLiteralRefusal(doc->tables, INFINITY);
	EXPECT_TRUE(Mentions(inf, "input 'energy' has range 0..1, and its literal is inf")) << inf;
	std::string const low = EnergyLiteralRefusal(doc->tables, -0.25);
	EXPECT_TRUE(Mentions(low, "input 'energy' has range 0..1, and its literal is -0.25")) << low;
	std::string const five = BuildRefusal("given: {energy: input.energy,", "given: {energy: {literal: 5},");
	EXPECT_TRUE(Mentions(five, "machine 'grazer' step clause 0 (table 'grazer_brain')")) << five;
	EXPECT_TRUE(Mentions(five, "input 'energy' has range 0..1, and its literal is 5")) << five;
}

// ---- Or, weighted and all clauses ------------------------------------------------

namespace
{

// The definition's chain: an or table answers a flag set, and a best table keyed
// by flags reads it. bits is no closed set, so lookup keeps its declared rows.
constexpr std::string_view kPhaseChain = R"(
tables:
  - name: phase
    read_only: true
    pick: or
    output: bits
    columns:
      - {name: mode,  type: hash, key: true}
      - {name: bits,  type: flags}
      - {name: label, type: hash}
    order_by: {columns: [mode]}
  - name: lookup
    read_only: true
    pick: best
    columns:
      - {name: bits, type: flags, key: true, domain: rows}
      - {name: next, type: hash}
    order_by: {columns: [bits]}
machines:
  - name: chain
    state:
      - {name: mode, type: hash, members: [idle, walk], initial: idle}
    step:
      - from: phase
        where: {mode: state.mode}
      - from: lookup
        where: {bits: clause.0.bits}
        set: {mode: next}
)";

constexpr std::string_view kPhaseRows  = "mode,bits,label\nidle,1,a\nwalk,2,b\nwalk,4,c\n";
// 4 is a value phase's rows hold and lookup has no run for: an or output closes
// nothing, so lookup is proved over its own rows, which do not name it.
constexpr std::string_view kLookupRows = "bits,next\n0,idle\n1,walk\n2,idle\n6,idle\n";

struct chain_fixture
{
	schema_document document;
	relation_store  store;

	chain_fixture(std::string_view yaml, std::string_view phase, std::string_view lookup)
		: document(Parsed(yaml, "")), store(std::move(document.tables))
	{
		EXPECT_EQ(RefusalOf(LoadCsv(store, csv_rows{0, 1, phase, "phase.csv"})), "(accepted)");
		EXPECT_EQ(RefusalOf(LoadCsv(store, csv_rows{1, 1, lookup, "lookup.csv"})), "(accepted)");
	}
};

size_t TupleOf(detail::prepared_clause const& clause, int64_t key)
{
	for (size_t t = 0; t < clause.tupleCount; ++t)
	{
		if (clause.tuples[t] == key)
		{
			return t;
		}
	}
	return SIZE_MAX;
}

}

TEST(MachinePrepare, AnOrClauseInAPhaseChainPrepares)
{
	chain_fixture f(kPhaseChain, kPhaseRows, kLookupRows);
	auto machine = Prepare(f.document.machines()[0], f.store);
	ASSERT_TRUE(machine.has_value()) << RefusalOf(machine);
	auto const& step = Inside(*machine).lists[size_t(trigger::Step)];
	ASSERT_EQ(step.size(), 2u);
	ASSERT_EQ(step[0].tupleCount, 2u);
	size_t const walk = TupleOf(step[0], Hash("walk"));
	ASSERT_NE(walk, SIZE_MAX);
	EXPECT_EQ(step[0].rowRuns[walk].count, 2u) << "an or clause keeps its whole run";
	EXPECT_EQ(step[1].tupleCount, 4u) << "lookup's declared rows: 0, 1, 2, 6";
}

TEST(MachinePrepare, AnOrClauseTupleWithNoRunIsAnEmptyRun)
{
	chain_fixture f(kPhaseChain, "mode,bits,label\nidle,1,a\n", kLookupRows);
	auto machine = Prepare(f.document.machines()[0], f.store);
	ASSERT_TRUE(machine.has_value()) << RefusalOf(machine);
	detail::prepared_clause const& phase = Inside(*machine).lists[size_t(trigger::Step)][0];
	ASSERT_EQ(phase.tupleCount, 2u);
	size_t const walk = TupleOf(phase, Hash("walk"));
	ASSERT_NE(walk, SIZE_MAX);
	EXPECT_EQ(phase.rowRuns[walk].count, 0u) << "it answers 0";
}

TEST(MachineDefinition, AnOrClauseEmitsNothing)
{
	std::string const tables = Replace(kPhaseChain, "    order_by: {columns: [mode]}\n",
		"    order_by: {columns: [mode]}\n    suffix: say\n");
	std::string const machines = Replace(tables, "        where: {mode: state.mode}\n",
		"        where: {mode: state.mode}\n        emit: say\n");
	std::string const error = RefusalOf(Document(machines, ""));
	EXPECT_TRUE(Mentions(error, "machine 'chain' step clause 0 (table 'phase')")) << error;
	EXPECT_TRUE(Mentions(error, "the clause emits 'say', and or table 'phase' answers the OR of every row that holds, "
		"which may be several rows; it has no one row to emit")) << error;
}

TEST(MachineDefinition, AnOrClauseAnswersOnlyItsOutput)
{
	std::string const read = RefusalOf(Document(Replace(kPhaseChain, "clause.0.bits", "clause.0.label"), ""));
	EXPECT_TRUE(Mentions(read, "machine 'chain' step clause 1 (table 'lookup')")) << read;
	EXPECT_TRUE(Mentions(read, "key 'bits' reads column 'label' of clause 0, and or table 'phase' answers only its output "
		"'bits'")) << read;
	std::string const set = RefusalOf(Document(Replace(kPhaseChain, "        where: {mode: state.mode}\n",
		"        where: {mode: state.mode}\n        set: {mode: label}\n"), ""));
	EXPECT_TRUE(Mentions(set, "machine 'chain' step clause 0 (table 'phase')")) << set;
	EXPECT_TRUE(Mentions(set, "set reads column 'label' of or table 'phase', which answers only its output 'bits'")) << set;
}

namespace
{

constexpr std::string_view kWander = R"(
tables:
  - name: wander
    read_only: true
    pick: weighted
    weight: w
    columns:
      - {name: mode, type: hash, key: true}
      - {name: w,    type: double}
      - {name: next, type: hash}
    order_by: {columns: [mode]}
machines:
  - name: drift
    state:
      - {name: mode, type: hash, members: [idle, walk], initial: idle}
    step:
      - from: wander
        where: {mode: state.mode}
        set: {mode: next}
)";

}

TEST(MachinePrepare, AWeightedClauseWithEveryTuplePrepares)
{
	auto doc = Document(kWander, "");
	ASSERT_TRUE(doc.has_value()) << RefusalOf(doc);
	relation_store store(std::move(doc->tables));
	ASSERT_EQ(RefusalOf(LoadCsv(store, csv_rows{0, 1, "mode,w,next\nidle,1,walk\nidle,3,idle\nwalk,1,idle\n", "w.csv"})),
		"(accepted)");
	auto machine = Prepare(doc->machines()[0], store);
	ASSERT_TRUE(machine.has_value()) << RefusalOf(machine);
	detail::prepared_clause const& wander = Inside(*machine).lists[size_t(trigger::Step)][0];
	ASSERT_EQ(wander.tupleCount, 2u);
	size_t const idle = TupleOf(wander, Hash("idle"));
	ASSERT_NE(idle, SIZE_MAX);
	EXPECT_EQ(wander.rowRuns[idle].count, 2u);
	EXPECT_EQ(wander.rowRuns[1 - idle].count, 1u);
}

TEST(MachinePrepare, AWeightedClauseTupleWithNoRunIsRefused)
{
	auto doc = Document(kWander, "");
	ASSERT_TRUE(doc.has_value()) << RefusalOf(doc);
	relation_store store(std::move(doc->tables));
	ASSERT_EQ(RefusalOf(LoadCsv(store, csv_rows{0, 1, "mode,w,next\nidle,1,walk\nidle,3,idle\n", "w.csv"})), "(accepted)");
	std::string const error = RefusalOf(Prepare(doc->machines()[0], store));
	EXPECT_TRUE(Mentions(error, "machine 'drift' step clause 0 (table 'wander')")) << error;
	EXPECT_TRUE(Mentions(error, "table 'wander' key (" + H("walk")
		+ ") has no row to draw; no row has this key, and the key is in the clause's domain")) << error;
}

TEST(MachinePrepare, AWeightedRowSettingANonMemberIsRefused)
{
	auto doc = Document(kWander, "");
	ASSERT_TRUE(doc.has_value()) << RefusalOf(doc);
	relation_store store(std::move(doc->tables));
	ASSERT_EQ(RefusalOf(LoadCsv(store, csv_rows{0, 1, "mode,w,next\nidle,1,walk\nwalk,1,sleep\n", "w.csv"})),
		"(accepted)");
	std::string const error = RefusalOf(Prepare(doc->machines()[0], store));
	EXPECT_TRUE(Mentions(error, "sets state column 'mode' from column 'next' to " + H("sleep")
		+ ", which is not one of its members")) << error;
}
