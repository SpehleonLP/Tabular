#include <gtest/gtest.h>
#include "Support/allocationtestingsupport.h"
#include "tabular/instance.h"
#include "tabular/machine.h"
#include "tabular/pick.h"
#include "tabular/relation.h"
#include "tabular/rows_csv.h"
#include "tabular/schema_yaml.h"
#include "cityhash/src/city.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace tabular;

// Hierarchy (commands-and-messages spec, Part 4): entry and exit run per
// column, several fall-through keys give way in key order, and an override
// clause is a left outer join with a coalesce, so a hierarchical machine can
// be written in one table or in two and runs the same.

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

int64_t Hash(std::string_view text)
{
	return static_cast<int64_t>(CityHash32(text.data(), text.size()));
}

// What the host fills for one step: the event's kind (0 for none).
struct host_object
{
	int64_t kind = 0;
};

cell ReadKind(void*, void const* self, ErrorContext*)
{
	return cell::Integer(static_cast<host_object const*>(self)->kind);
}

host_binding KindHost()
{
	host_binding host;
	host.resolve = [](std::string_view name) -> tl::expected<input_getter, ErrorContext>
	{
		if (name == "kind")
		{
			return input_getter{&ReadKind};
		}
		return tl::make_unexpected(ErrorContext("no wire named '", name, "'"));
	};
	return host;
}

// A document's tables loaded with rows (one text per table, in table order),
// its first machine prepared and bound.
struct machine_fixture
{
	schema_document            document;
	relation_store             store;
	counted_ptr<Machine const> machine;
	counted_ptr<Binding const> binding;
	std::string                refusal;   // Prepare's, when it refused

	machine_fixture(std::string_view yaml, std::vector<std::string_view> const& rows)
		: document(Parse(yaml)), store(std::move(document.tables))
	{
		if (document.machines().empty())
		{
			return;
		}
		for (uint32_t t = 0; t < rows.size(); ++t)
		{
			EXPECT_EQ(RefusalOf(LoadCsv(store, csv_rows{t, 1, rows[t], "rows.csv"})), "(accepted)") << "table " << t;
		}
		auto prepared = Prepare(document.machines()[0], store);
		if (!prepared)
		{
			refusal = prepared.error().format();
			return;
		}
		machine = *prepared;
		auto bound = Bind(machine, KindHost());
		EXPECT_TRUE(bound.has_value()) << RefusalOf(bound);
		if (bound)
		{
			binding = *bound;
		}
	}

	static schema_document Parse(std::string_view yaml)
	{
		auto doc = DocumentFromYaml(yaml, "machine.yaml");
		EXPECT_TRUE(doc.has_value()) << RefusalOf(doc);
		return doc ? std::move(*doc) : schema_document{};
	}
};

// A run's commands, as text, in the order returned.
std::vector<std::string> Texts(std::span<emitted const> results)
{
	std::vector<std::string> out;
	for (emitted const& e : results)
	{
		out.emplace_back(e.command);
	}
	return out;
}

// One step with kind filled for it and reset after, as a host dispatching an
// event does.
std::vector<std::string> Dispatch(instance& subject, host_object& host, std::string_view kind, uint64_t seed)
{
	host.kind = kind.empty() ? 0 : Hash(kind);
	std::vector<std::string> out = Texts(Step(subject, &host, seed));
	host.kind = 0;
	return out;
}

std::string Replace(std::string_view text, std::string_view from, std::string_view to)
{
	std::string out(text);
	size_t const at = out.find(from);
	EXPECT_NE(at, std::string::npos) << "'" << from << "' is not in the text";
	if (at != std::string::npos)
	{
		out.replace(at, from.size(), to);
	}
	return out;
}

// ---- Entry and exit per column -------------------------------------------------------

// The craft bench's two state columns: a page and a modal on it. Each has its
// own entry and exit table, keyed on its own column.
constexpr std::string_view kBench = R"(
tables:
  - name: bench
    read_only: true
    pick: best
    columns:
      - {name: page,  type: hash, key: true, domain: rows}
      - {name: modal, type: hash, key: true, domain: rows}
      - {name: kind,  type: hash, key: true, fallthrough: true}
      - {name: next_page,  type: hash}
      - {name: next_modal, type: hash}
    order_by: {columns: [page, modal, kind]}
  - name: page_enter
    read_only: true
    pick: best
    columns:
      - {name: page, type: hash, key: true, domain: rows}
    order_by: {columns: [page]}
    suffix: command
  - name: page_leave
    read_only: true
    pick: best
    columns:
      - {name: page, type: hash, key: true, domain: rows}
    order_by: {columns: [page]}
    suffix: command
  - name: modal_enter
    read_only: true
    pick: best
    columns:
      - {name: modal, type: hash, key: true, domain: rows}
    order_by: {columns: [modal]}
    suffix: command
  - name: modal_leave
    read_only: true
    pick: best
    columns:
      - {name: modal, type: hash, key: true, domain: rows}
    order_by: {columns: [modal]}
    suffix: command
machines:
  - name: craft_bench
    inputs:
      - {name: kind, type: hash}
    state:
      - {name: page,  type: hash, members: [experiment, recipes], initial: experiment}
      - {name: modal, type: hash, members: [none, confirm, result], initial: none}
    step:
      - from:  bench
        where: {page: state.page, modal: state.modal, kind: input.kind}
        set:   {page: next_page, modal: next_modal}
    exit:
      - from:  page_leave
        where: {page: state.page}
        emit:  command
      - from:  modal_leave
        where: {modal: state.modal}
        emit:  command
    entry:
      - from:  page_enter
        where: {page: state.page}
        emit:  command
      - from:  modal_enter
        where: {modal: state.modal}
        emit:  command
)";

constexpr std::string_view kBenchRows = R"(page,       modal,   kind,    next_page,  next_modal
experiment, none,    ,        experiment, none
experiment, none,    craft,   experiment, confirm
experiment, none,    flip,    recipes,    none
experiment, confirm, ,        experiment, confirm
experiment, confirm, yes,     experiment, result
experiment, result,  ,        experiment, result
experiment, result,  ok,      recipes,    none
recipes,    none,    ,        recipes,    none
recipes,    none,    flip,    experiment, none
recipes,    confirm, ,        recipes,    none
recipes,    result,  ,        recipes,    none
)";

constexpr std::string_view kPageEnterRows  = "page,command\nexperiment,show-page experiment\nrecipes,show-page recipes\n";
constexpr std::string_view kPageLeaveRows  = "page,command\nexperiment,hide-page experiment\nrecipes,hide-page recipes\n";
constexpr std::string_view kModalEnterRows = "modal,command\nnone,()\nconfirm,show-modal confirm\nresult,show-modal result\n";
constexpr std::string_view kModalLeaveRows = "modal,command\nnone,()\nconfirm,hide-modal confirm\nresult,hide-modal result\n";

}

TEST(EntryPerColumn, AModalOnlyChangeRunsNoPageEntryOrExit)
{
	machine_fixture f(kBench, {kBenchRows, kPageEnterRows, kPageLeaveRows, kModalEnterRows, kModalLeaveRows});
	ASSERT_TRUE(f.binding) << f.refusal;
	host_object host;
	instance bench = CreateInstance(*f.binding, 1, 1, &host);
	EXPECT_EQ(Texts(bench.Emitted()), (std::vector<std::string>{"((show-page experiment))", "()"}))
		<< "creation enters every column";

	EXPECT_EQ(Dispatch(bench, host, "craft", 2), (std::vector<std::string>{"()", "((show-modal confirm))"}));
	EXPECT_EQ(Dispatch(bench, host, "yes", 3),
		(std::vector<std::string>{"((hide-modal confirm))", "((show-modal result))"}))
		<< "confirm to result changes only the modal: the page is not hidden and shown again";
	EXPECT_EQ(bench.Answer(trigger::Entry, 0).outcome, clause_outcome::NotRun);
	EXPECT_EQ(bench.Answer(trigger::Exit, 0).outcome, clause_outcome::NotRun);
	EXPECT_EQ(bench.Answer(trigger::Entry, 1).outcome, clause_outcome::Answered);

	EXPECT_EQ(Dispatch(bench, host, "ok", 4), (std::vector<std::string>{"((hide-page experiment))",
		"((hide-modal result))", "((show-page recipes))", "()"})) << "both columns changed: both run";
	EXPECT_EQ(Dispatch(bench, host, "flip", 5),
		(std::vector<std::string>{"((hide-page recipes))", "((show-page experiment))"}))
		<< "a page-only change runs no modal entry or exit";
	EXPECT_EQ(bench.Answer(trigger::Entry, 1).outcome, clause_outcome::NotRun);
	EXPECT_EQ(Dispatch(bench, host, "", 6), (std::vector<std::string>{})) << "nothing changed, nothing runs";
}

TEST(EntryPerColumn, AClauseKeyedOnNoStateColumnRunsOnAnyChange)
{
	// A third entry clause keys on no state column (a constant table): it runs
	// whenever any hash column changed, as every clause did before Part 4.
	std::string const yaml = Replace(Replace(kBench, "machines:\n", R"(  - name: chime
    read_only: true
    pick: best
    columns:
      - {name: v, type: int}
    suffix: command
machines:
)"), R"(      - from:  modal_enter
        where: {modal: state.modal}
        emit:  command
)", R"(      - from:  modal_enter
        where: {modal: state.modal}
        emit:  command
      - from:  chime
        emit:  command
)");
	machine_fixture f(yaml, {kBenchRows, kPageEnterRows, kPageLeaveRows, kModalEnterRows, kModalLeaveRows,
		"v,command\n1,chime\n"});
	ASSERT_TRUE(f.binding) << f.refusal;
	host_object host;
	instance bench = CreateInstance(*f.binding, 1, 1, &host);
	EXPECT_EQ(Dispatch(bench, host, "craft", 2), (std::vector<std::string>{"()", "((show-modal confirm))", "(chime)"}));
	EXPECT_EQ(Dispatch(bench, host, "", 3), (std::vector<std::string>{})) << "and not when nothing changed";
}

TEST(EntryPerColumn, ClausesThatReadOneAnotherRunAsOne)
{
	// The modal's entry reads what the page's entry answered: they run on the
	// union of their columns, so a modal-only change runs the page's entry too
	// rather than leaving its reader without a source.
	std::string const yaml = Replace(Replace(kBench, R"(  - name: modal_enter
    read_only: true
    pick: best
    columns:
      - {name: modal, type: hash, key: true, domain: rows}
    order_by: {columns: [modal]}
    suffix: command)", R"(  - name: modal_enter
    read_only: true
    pick: best
    columns:
      - {name: modal, type: hash, key: true, domain: rows}
      - {name: page,  type: int, key: true, domain: rows}
    order_by: {columns: [modal, page]}
    suffix: command
  - name: page_code
    read_only: true
    pick: best
    columns:
      - {name: page, type: hash, key: true, domain: rows}
      - {name: code, type: int}
    order_by: {columns: [page]})"), R"(    entry:
      - from:  page_enter
        where: {page: state.page}
        emit:  command
      - from:  modal_enter
        where: {modal: state.modal}
        emit:  command
)", R"(    entry:
      - from:  page_code
        where: {page: state.page}
      - from:  modal_enter
        where: {modal: state.modal, page: clause.0.code}
        emit:  command
)");
	machine_fixture f(yaml, {kBenchRows, kPageEnterRows, kPageLeaveRows,
		"modal,page,command\nnone,1,()\nnone,2,()\nconfirm,1,show-modal confirm on 1\nconfirm,2,()\nresult,1,()\nresult,2,()\n",
		"page,code\nexperiment,1\nrecipes,2\n", kModalLeaveRows});
	ASSERT_TRUE(f.binding) << f.refusal;
	host_object host;
	instance bench = CreateInstance(*f.binding, 1, 1, &host);
	EXPECT_EQ(Dispatch(bench, host, "craft", 2), (std::vector<std::string>{"()", "((show-modal confirm on 1))"}));
	EXPECT_EQ(bench.Answer(trigger::Entry, 0).outcome, clause_outcome::Answered) << "the source ran with its reader";
}

// ---- A hierarchical machine in one table -------------------------------------------------

namespace
{

// The spec's HFSM: a superstate and a substate, the substate's key falling
// through first (it is listed before kind). A walk that is hit has no run of
// its own and answers from the superstate's row; a run overrides it.
constexpr std::string_view kHfsm = R"(
tables:
  - name: behaviour
    read_only: true
    pick: best
    columns:
      - {name: super, type: hash, key: true, domain: rows}
      - {name: sub,   type: hash, key: true, fallthrough: true}
      - {name: kind,  type: hash, key: true, fallthrough: true}
      - {name: next_super, type: hash}
      - {name: next_sub,   type: hash}
    order_by: {columns: [super, sub, kind]}
  - name: super_enter
    read_only: true
    pick: best
    columns:
      - {name: super, type: hash, key: true, domain: rows}
    order_by: {columns: [super]}
    suffix: command
  - name: sub_enter
    read_only: true
    pick: best
    columns:
      - {name: sub, type: hash, key: true, domain: rows}
    order_by: {columns: [sub]}
    suffix: command
machines:
  - name: creature
    inputs:
      - {name: kind, type: hash}
    state:
      - {name: super, type: hash, members: [move, rest], initial: move}
      - {name: sub,   type: hash, members: [walk, run, sit, sleep], initial: walk}
    step:
      - from:  behaviour
        where: {super: state.super, sub: state.sub, kind: input.kind}
        set:   {super: next_super, sub: next_sub}
    entry:
      - from:  super_enter
        where: {super: state.super}
        emit:  command
      - from:  sub_enter
        where: {sub: state.sub}
        emit:  command
)";

constexpr std::string_view kBehaviourRows = R"(super, sub,   kind,   next_super, next_sub
move,  ,      ,       move,       walk
move,  walk,  ,       move,       walk
move,  run,   ,       move,       run
move,  walk,  faster, move,       run
move,  ,      hit,    rest,       sit
move,  run,   hit,    move,       walk
rest,  ,      ,       rest,       sit
rest,  sleep, ,       rest,       sleep
rest,  ,      wake,   move,       walk
rest,  sit,   nap,    rest,       sleep
)";

constexpr std::string_view kSuperEnterRows = "super,command\nmove,enter-move\nrest,enter-rest\n";
constexpr std::string_view kSubEnterRows   = "sub,command\nwalk,enter-walk\nrun,enter-run\nsit,enter-sit\nsleep,enter-sleep\n";

}

TEST(Hierarchy, AWalkThatIsHitAnswersFromTheSuperstateAndARunFromItsOwnRow)
{
	machine_fixture f(kHfsm, {kBehaviourRows, kSuperEnterRows, kSubEnterRows});
	ASSERT_TRUE(f.binding) << f.refusal;
	host_object host;
	instance walker = CreateInstance(*f.binding, 1, 1, &host);
	Dispatch(walker, host, "hit", 2);
	EXPECT_EQ(walker.State(0).i, Hash("rest"));
	EXPECT_EQ(walker.State(1).i, Hash("sit")) << "(move, walk, hit) has no run: the superstate's (move, _, hit) answers";
	EXPECT_TRUE(walker.Answer(trigger::Step, 0).fellThrough);

	instance runner = CreateInstance(*f.binding, 2, 1, &host);
	Dispatch(runner, host, "faster", 2);
	ASSERT_EQ(runner.State(1).i, Hash("run"));
	Dispatch(runner, host, "hit", 3);
	EXPECT_EQ(runner.State(0).i, Hash("move"));
	EXPECT_EQ(runner.State(1).i, Hash("walk")) << "run's own row overrides the superstate's";
	EXPECT_FALSE(runner.Answer(trigger::Step, 0).fellThrough);
}

TEST(Hierarchy, WalkToRunRunsRunsEntryAndNotMoves)
{
	machine_fixture f(kHfsm, {kBehaviourRows, kSuperEnterRows, kSubEnterRows});
	ASSERT_TRUE(f.binding) << f.refusal;
	host_object host;
	instance creature = CreateInstance(*f.binding, 1, 1, &host);
	EXPECT_EQ(Texts(creature.Emitted()), (std::vector<std::string>{"(enter-move)", "(enter-walk)"}));
	EXPECT_EQ(Dispatch(creature, host, "faster", 2), (std::vector<std::string>{"(enter-run)"}))
		<< "the substate changed, the superstate did not";
	EXPECT_EQ(Dispatch(creature, host, "hit", 3), (std::vector<std::string>{"(enter-walk)"}))
		<< "run hit goes to walk within move, which is not entered again";
	EXPECT_EQ(Dispatch(creature, host, "hit", 4), (std::vector<std::string>{"(enter-rest)", "(enter-sit)"}));
}

TEST(Hierarchy, PickBestFallsThroughInTheSameOrder)
{
	// The fall-through is the table's, so the interpreted pick tries the same
	// keys in the same order as a step.
	machine_fixture f(kHfsm, {kBehaviourRows, kSuperEnterRows, kSubEnterRows});
	ASSERT_TRUE(f.binding) << f.refusal;
	auto next = [&](std::string_view super, std::string_view sub, std::string_view kind)
	{
		cell const keys[] = {cell::Integer(Hash(super)), cell::Integer(sub.empty() ? 0 : Hash(sub)),
			cell::Integer(kind.empty() ? 0 : Hash(kind))};
		auto row = PickBest(f.store, 0, pick_query{keys, {}});
		EXPECT_TRUE(row.has_value() && row->has_value()) << RefusalOf(row);
		return row && *row ? f.store.Value(0, **row, 4)->i : 0;
	};
	EXPECT_EQ(next("move", "walk", "hit"), Hash("sit"));
	EXPECT_EQ(next("move", "run", "hit"), Hash("walk"));
	EXPECT_EQ(next("move", "walk", "dance"), Hash("walk")) << "(move, walk, _): the substate holds out against an unknown kind";
	EXPECT_EQ(next("rest", "sleep", "dance"), Hash("sleep"));
	EXPECT_EQ(next("rest", "sit", "dance"), Hash("sit")) << "(rest, _, _), the superstate's catch-all";
}

// ---- Retry order -----------------------------------------------------------------------

namespace
{

// Two fall-through keys, a then b: a is the lowest bit, so it gives way first.
constexpr std::string_view kTwo = R"(
tables:
  - name: two
    read_only: true
    pick: best
    columns:
      - {name: a, type: hash, key: true, fallthrough: true}
      - {name: b, type: hash, key: true, fallthrough: true}
      - {name: said, type: hash}
    order_by: {columns: [a, b]}
)";

constexpr std::string_view kTwoRows = R"(a, b, said
,  ,  neither
,  y, b-held
x, ,  a-held
x, y, both
)";

}

TEST(Hierarchy, TwoFallThroughKeysGiveWayInKeyOrderTheFirstAsTheLowestBit)
{
	auto s = SchemaFromYaml(kTwo, "two.yaml");
	ASSERT_TRUE(s.has_value()) << RefusalOf(s);
	relation_store store(std::move(*s));
	ASSERT_EQ(RefusalOf(LoadCsv(store, csv_rows{0, 1, kTwoRows, "two.csv"})), "(accepted)");
	auto said = [&](std::string_view a, std::string_view b)
	{
		cell const keys[] = {cell::Integer(a.empty() ? 0 : Hash(a)), cell::Integer(b.empty() ? 0 : Hash(b))};
		auto row = PickBest(store, 0, pick_query{keys, {}});
		EXPECT_TRUE(row.has_value() && row->has_value()) << RefusalOf(row);
		return row && *row ? store.Value(0, **row, 2)->i : 0;
	};
	EXPECT_EQ(said("x", "y"), Hash("both")) << "exact";
	EXPECT_EQ(said("p", "y"), Hash("b-held")) << "try 1: (blank, y)";
	EXPECT_EQ(said("x", "q"), Hash("a-held")) << "(blank, q) misses; try 2: (x, blank)";
	EXPECT_EQ(said("p", "q"), Hash("neither")) << "try 3: (blank, blank)";
	EXPECT_EQ(said("", "q"), Hash("neither")) << "a blank already: the retries that would blank it again are skipped";
}

TEST(Hierarchy, ALoadDemandsTheRunWithEveryFallThroughKeyBlank)
{
	auto s = SchemaFromYaml(kTwo, "two.yaml");
	ASSERT_TRUE(s.has_value()) << RefusalOf(s);
	relation_store store(std::move(*s));
	std::string const refused = RefusalOf(LoadCsv(store, csv_rows{0, 1, "a,b,said\n,y,b-held\nx,,a-held\n", "two.csv"}));
	EXPECT_TRUE(Mentions(refused, "two")) << refused;
	EXPECT_NE(refused, "(accepted)") << "the all-blank run is the catch-all, and it is missing";
}

// ---- Override clauses -------------------------------------------------------------------

namespace
{

// The spec's mod: the base package's table is total, the mod's has rows only
// where it changes something, and its clause overrides the base clause.
// Clause 2 reads the base clause's next, which is the coalesced answer.
constexpr std::string_view kModded = R"(
tables:
  - name: brain
    read_only: true
    pick: best
    columns:
      - {name: mode,  type: hash, key: true, domain: rows}
      - {name: next,  type: hash}
      - {name: steps, type: int}
    order_by: {columns: [mode]}
    suffix: command
  - name: brain_mod
    read_only: true
    pick: best
    columns:
      - {name: mode, type: hash, key: true, domain: rows}
      - {name: next, type: hash}
    order_by: {columns: [mode]}
    suffix: command
  - name: label
    read_only: true
    pick: best
    columns:
      - {name: mode, type: hash, key: true, domain: rows}
      - {name: code, type: int}
    order_by: {columns: [mode]}
machines:
  - name: grazer
    state:
      - {name: mode,  type: hash, members: [idle, walk, sleep], initial: idle}
      - {name: steps, type: int, initial: 0}
      - {name: code,  type: int, initial: 0}
    step:
      - from:  brain
        where: {mode: state.mode}
        set:   {mode: next, steps: steps}
        emit:  command
      - from:  brain_mod
        overrides: 0
        where: {mode: state.mode}
        set:   {mode: next}
        emit:  command
      - from:  label
        where: {mode: clause.0.next}
        set:   {code: code}
)";

constexpr std::string_view kBaseRows  = "mode,next,steps,command\nidle,walk,1,anim walk\nwalk,idle,2,anim stand\nsleep,idle,3,anim wake\n";
constexpr std::string_view kModRows   = "mode,next,command\nidle,sleep,anim yawn\n";
constexpr std::string_view kLabelRows = "mode,code\nidle,10\nwalk,20\nsleep,30\n";

}

TEST(OverrideClause, AHitReplacesTheSetsAndEmitAndADependentReadsTheCoalescedAnswer)
{
	machine_fixture f(kModded, {kBaseRows, kModRows, kLabelRows});
	ASSERT_TRUE(f.binding) << f.refusal;
	host_object host;
	instance grazer = CreateInstance(*f.binding, 1, 1, &host);
	EXPECT_EQ(Texts(Step(grazer, &host, 2)), (std::vector<std::string>{"((anim yawn))"}))
		<< "the mod's emit replaces the base's, not as well as";
	EXPECT_EQ(grazer.State(0).i, Hash("sleep")) << "the mod's set is written over the base's";
	EXPECT_EQ(grazer.State(1).i, 1) << "a column the mod does not set keeps the base's write";
	EXPECT_EQ(grazer.State(2).i, 30) << "the reader of clause 0 read the coalesced next";
	EXPECT_EQ(grazer.Answer(trigger::Step, 1).outcome, clause_outcome::Answered);
}

TEST(OverrideClause, AMissLeavesTheBaseClausesSetsAndEmit)
{
	machine_fixture f(kModded, {kBaseRows, kModRows, kLabelRows});
	ASSERT_TRUE(f.binding) << f.refusal;
	host_object host;
	instance grazer = CreateInstance(*f.binding, 1, 1, &host);
	Step(grazer, &host, 2);   // idle -> sleep, by the mod
	EXPECT_EQ(Texts(Step(grazer, &host, 3)), (std::vector<std::string>{"((anim wake))"}));
	EXPECT_EQ(grazer.State(0).i, Hash("idle"));
	EXPECT_EQ(grazer.State(1).i, 3);
	EXPECT_EQ(grazer.State(2).i, 10);
	EXPECT_EQ(grazer.Answer(trigger::Step, 1).outcome, clause_outcome::Missed) << "no mod row for sleep: NULL";
	EXPECT_EQ(grazer.Answer(trigger::Step, 0).outcome, clause_outcome::Answered);
}

TEST(OverrideClause, AMissIsNeitherAHoldNorReported)
{
	machine_fixture f(kModded, {kBaseRows, kModRows, kLabelRows});
	ASSERT_TRUE(f.binding) << f.refusal;
	int reports = 0;
	host_binding host = KindHost();
	host.report = [&reports](uint64_t, uint32_t, size_t, ErrorContext const&) { ++reports; };
	auto binding = Bind(f.machine, std::move(host));
	ASSERT_TRUE(binding.has_value()) << RefusalOf(binding);
	host_object object;
	instance grazer = CreateInstance(**binding, 1, 1, &object);
	for (uint64_t seed = 2; seed < 8; ++seed)
	{
		Step(grazer, &object, seed);
	}
	EXPECT_EQ(reports, 0);
}

TEST(OverrideClause, TheDependentsDomainHoldsTheOverridesValuesToo)
{
	// The mod can answer sleep from idle, which the base never does from
	// idle; the label of clause 0's next must have a run for every value the
	// coalesced answer can take, sleep included.
	machine_fixture f(kModded, {"mode,next,steps,command\nidle,walk,1,()\nwalk,idle,2,()\nsleep,walk,3,()\n", kModRows,
		"mode,code\nidle,10\nwalk,20\n"});
	EXPECT_FALSE(f.machine);
	EXPECT_TRUE(Mentions(f.refusal, "machine 'grazer' step clause 2 (table 'label')")) << f.refusal;
}

TEST(OverrideClause, TheOverridesTableNeedNotBeTotalButItsRunsAreProved)
{
	// The mod names only idle, and Prepare accepts it; a run the mod has is
	// proved like any clause's, so a hash it writes must be a member.
	machine_fixture good(kModded, {kBaseRows, kModRows, kLabelRows});
	EXPECT_TRUE(good.machine) << good.refusal;
	machine_fixture bad(kModded, {kBaseRows, "mode,next,command\nidle,dream,()\n",
		"mode,code\nidle,10\nwalk,20\nsleep,30\ndream,40\n"});
	EXPECT_FALSE(bad.machine);
	EXPECT_TRUE(Mentions(bad.refusal, "machine 'grazer' step clause 1 (table 'brain_mod')")) << bad.refusal;
	EXPECT_TRUE(Mentions(bad.refusal, "which is not one of its members")) << bad.refusal;
}

TEST(OverrideClause, ItsShapeIsChecked)
{
	auto refusal = [](std::string const& yaml) { return RefusalOf(DocumentFromYaml(yaml, "machine.yaml")); };
	std::string const notNext = refusal(Replace(kModded, "        overrides: 0\n", "        overrides: 2\n"));
	EXPECT_TRUE(Mentions(notNext, "an override names the clause directly before it")) << notNext;

	std::string const gap = refusal(Replace(kModded, "      - from:  brain_mod\n        overrides: 0\n",
		"      - from:  label\n        where: {mode: state.mode}\n      - from:  brain_mod\n        overrides: 0\n"));
	EXPECT_TRUE(Mentions(gap, "which does not directly precede it")) << gap;

	std::string const wider = refusal(Replace(Replace(kModded, R"(      - {name: next, type: hash}
    order_by: {columns: [mode]}
    suffix: command
  - name: label)", R"(      - {name: next, type: hash}
      - {name: code, type: int}
    order_by: {columns: [mode]}
    suffix: command
  - name: label)"), "        set:   {mode: next}\n        emit:  command\n      - from:  label",
		"        set:   {mode: next, code: code}\n        emit:  command\n      - from:  label"));
	EXPECT_TRUE(Mentions(wider, "sets state column 'code', which clause 0 it overrides does not")) << wider;

	std::string const twice = refusal(Replace(kModded, R"(        set:   {mode: next}
        emit:  command
      - from:  label)", R"(        set:   {mode: next}
        emit:  command
      - from:  brain_mod
        overrides: 1
        where: {mode: state.mode}
        set:   {mode: next}
      - from:  label)"));
	EXPECT_TRUE(Mentions(twice, "which is itself an override")) << twice;

	std::string const emits = refusal(Replace(kModded, R"(        set:   {mode: next, steps: steps}
        emit:  command
)", R"(        set:   {mode: next, steps: steps}
)"));
	EXPECT_TRUE(Mentions(emits, "the clause emits, and clause 0 it overrides does not")) << emits;

	std::string const direct = refusal(Replace(kModded, "        where: {mode: clause.0.next}\n",
		"        where: {mode: clause.1.next}\n"));
	EXPECT_TRUE(Mentions(direct, "reads clause 1, which overrides clause 0; read clause 0")) << direct;
}

TEST(OverrideClause, ItNamesAClauseThatAlwaysAnswers)
{
	// A base keyed on a machine input closes nothing: a value outside its
	// domain would hold it for want of a run, and the override would have
	// nothing to coalesce onto.
	std::string const yaml = Replace(Replace(kModded, "    state:\n", "    inputs:\n      - {name: kind, type: hash}\n    state:\n"),
		R"(      - from:  brain
        where: {mode: state.mode})", R"(      - from:  brain
        where: {mode: input.kind})");
	std::string const refused = RefusalOf(DocumentFromYaml(yaml, "machine.yaml"));
	EXPECT_TRUE(Mentions(refused, "machine 'grazer' step clause 1 (table 'brain_mod')")) << refused;
	EXPECT_TRUE(Mentions(refused, "whose key 'mode' of table 'brain' is read from a source that closes nothing")) << refused;
}

// ---- Many surfaces, one machine -----------------------------------------------------------

namespace
{

// One hierarchical machine in two forms. One table: the event's kind falls
// through first, then the substate. Two tables: the superstate's rules, and
// an override with the substates' own rows. Each lookup tries the same keys
// in the same order: (super, kind, sub), (super, _, sub), then (super, kind),
// (super, _).
constexpr std::string_view kOneTable = R"(
tables:
  - name: rules
    read_only: true
    pick: best
    columns:
      - {name: super, type: hash, key: true, domain: rows}
      - {name: kind,  type: hash, key: true, fallthrough: true}
      - {name: sub,   type: hash, key: true, fallthrough: true}
      - {name: next_super, type: hash}
      - {name: next_sub,   type: hash}
    order_by: {columns: [super, kind, sub]}
    suffix: command
machines:
  - name: creature
    inputs:
      - {name: kind, type: hash}
    state:
      - {name: super, type: hash, members: [move, rest], initial: move}
      - {name: sub,   type: hash, members: [walk, run, sit, sleep], initial: walk}
    step:
      - from:  rules
        where: {super: state.super, kind: input.kind, sub: state.sub}
        set:   {super: next_super, sub: next_sub}
        emit:  command
)";

constexpr std::string_view kOneTableRows = R"(super, kind, sub,   next_super, next_sub, command
move,  ,     ,      move,       walk,     ()
move,  hit,  ,      rest,       sit,      ouch
move,  fast, ,      move,       run,      (speed up)
move,  ,     run,   move,       run,      ()
move,  hit,  run,   move,       walk,     stumble
rest,  ,     ,      rest,       sit,      ()
rest,  wake, ,      move,       walk,     (get up)
rest,  nap,  ,      rest,       sleep,    yawn
rest,  ,     sleep, rest,       sleep,    (snore)
)";

constexpr std::string_view kTwoTables = R"(
tables:
  - name: super_rules
    read_only: true
    pick: best
    columns:
      - {name: super, type: hash, key: true, domain: rows}
      - {name: kind,  type: hash, key: true, fallthrough: true}
      - {name: next_super, type: hash}
      - {name: next_sub,   type: hash}
    order_by: {columns: [super, kind]}
    suffix: command
  - name: sub_rules
    read_only: true
    pick: best
    columns:
      - {name: super, type: hash, key: true, domain: rows}
      - {name: kind,  type: hash, key: true, fallthrough: true}
      - {name: sub,   type: hash, key: true, domain: rows}
      - {name: next_super, type: hash}
      - {name: next_sub,   type: hash}
    order_by: {columns: [super, kind, sub]}
    suffix: command
machines:
  - name: creature
    inputs:
      - {name: kind, type: hash}
    state:
      - {name: super, type: hash, members: [move, rest], initial: move}
      - {name: sub,   type: hash, members: [walk, run, sit, sleep], initial: walk}
    step:
      - from:  super_rules
        where: {super: state.super, kind: input.kind}
        set:   {super: next_super, sub: next_sub}
        emit:  command
      - from:  sub_rules
        overrides: 0
        where: {super: state.super, kind: input.kind, sub: state.sub}
        set:   {super: next_super, sub: next_sub}
        emit:  command
)";

constexpr std::string_view kSuperRows = R"(super, kind, next_super, next_sub, command
move,  ,     move,       walk,     ()
move,  hit,  rest,       sit,      ouch
move,  fast, move,       run,      (speed up)
rest,  ,     rest,       sit,      ()
rest,  wake, move,       walk,     (get up)
rest,  nap,  rest,       sleep,    yawn
)";

constexpr std::string_view kSubRows = R"(super, kind, sub,   next_super, next_sub, command
move,  ,     run,   move,       run,      ()
move,  hit,  run,   move,       walk,     stumble
rest,  ,     sleep, rest,       sleep,    (snore)
)";

}

TEST(ManySurfaces, OneTableAndTwoTablesWithAnOverrideRunTheSame)
{
	machine_fixture one(kOneTable, {kOneTableRows});
	machine_fixture two(kTwoTables, {kSuperRows, kSubRows});
	ASSERT_TRUE(one.binding) << one.refusal;
	ASSERT_TRUE(two.binding) << two.refusal;
	host_object host;
	instance a = CreateInstance(*one.binding, 1, 1, &host);
	instance b = CreateInstance(*two.binding, 1, 1, &host);
	std::string_view const events[] = {"", "fast", "", "dance", "hit", "hit", "", "nap", "", "wake", "fast", "hit", "nap",
		"wake", ""};
	uint64_t seed = 2;
	for (std::string_view kind : events)
	{
		std::vector<std::string> const fromOne = Dispatch(a, host, kind, seed);
		std::vector<std::string> const fromTwo = Dispatch(b, host, kind, seed);
		EXPECT_EQ(fromOne, fromTwo) << "step " << seed << " on '" << kind << "'";
		EXPECT_EQ(a.State(0).i, b.State(0).i) << "step " << seed << " on '" << kind << "'";
		EXPECT_EQ(a.State(1).i, b.State(1).i) << "step " << seed << " on '" << kind << "'";
		++seed;
	}
	EXPECT_EQ(a.State(0).i, Hash("rest"));
	EXPECT_EQ(a.State(1).i, Hash("sleep")) << "a sleeper's own blank-kind row holds out against every kind, in both";
}

// Per-column entry, several fall-through keys, an override's miss and hit,
// and the coalesce: a step that does all of them allocates nothing.
TEST(ManySurfaces, HierarchicalStepsAllocateNothing)
{
	if (!kCountsAllocations)
	{
		GTEST_SKIP() << "a sanitizer owns operator new in this build, so nothing counts allocations";
	}
	machine_fixture hfsm(kHfsm, {kBehaviourRows, kSuperEnterRows, kSubEnterRows});
	machine_fixture two(kTwoTables, {kSuperRows, kSubRows});
	machine_fixture modded(kModded, {kBaseRows, kModRows, kLabelRows});
	ASSERT_TRUE(hfsm.binding) << hfsm.refusal;
	ASSERT_TRUE(two.binding) << two.refusal;
	ASSERT_TRUE(modded.binding) << modded.refusal;
	host_object host;
	instance a = CreateInstance(*hfsm.binding, 1, 1, &host);
	instance b = CreateInstance(*two.binding, 2, 1, &host);
	instance c = CreateInstance(*modded.binding, 3, 1, &host);
	int64_t const kinds[] = {0, Hash("faster"), Hash("hit"), Hash("hit"), Hash("wake"), Hash("fast"), Hash("nap"), Hash("dance")};
	auto round = [&](uint64_t seed)
	{
		for (int64_t kind : kinds)
		{
			host.kind = kind;
			Step(a, &host, seed);
			Step(b, &host, seed);
			Step(c, &host, seed);
		}
		host.kind = 0;
	};
	round(1);
	AllocationCount count;
	round(2);
	EXPECT_EQ(count.Count(), 0u);
}
