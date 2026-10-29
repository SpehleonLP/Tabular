#include <gtest/gtest.h>
#include "Support/allocationtestingsupport.h"
#include "tabular/instance.h"
#include "tabular/machine.h"
#include "tabular/pick.h"
#include "tabular/relation.h"
#include "tabular/rows_csv.h"
#include "tabular/document.h"
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
	int64_t kind   = 0;
	bool    mod_on = false;   // what the mod's condition tests
	bool    broken = false;   // the mod's condition fails to answer
};

bool ModOn(void*, void const* self, ErrorContext* error_out)
{
	host_object const& host = *static_cast<host_object const*>(self);
	if (host.broken)
	{
		*error_out = ErrorContext("the mod's script raised");
		return false;
	}
	return host.mod_on;
}

tl::expected<closure, ErrorContext> Conditions(std::string_view text)
{
	if (text == "mod_on")
	{
		return closure{&ModOn, nullptr, nullptr};
	}
	return tl::make_unexpected(ErrorContext("no condition '", text, "'"));
}

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

	machine_fixture(std::string_view source, std::vector<std::string_view> const& rows)
		: document(Parse(source)), store(std::move(document.tables))
	{
		if (document.machines().empty())
		{
			return;
		}
		for (uint32_t t = 0; t < rows.size(); ++t)
		{
			csv_rows load{t, 1, rows[t], "rows.csv"};
			load.condition = &Conditions;
			EXPECT_EQ(RefusalOf(LoadCsv(store, load)), "(accepted)") << "table " << t;
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

	static schema_document Parse(std::string_view source)
	{
		auto doc = ReadDocument(source, "machine.tabular");
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
[table bench
  (key page hash (domain rows))
  (key modal hash (domain rows))
  (key kind hash fallthrough)
  (column next_page hash)
  (column next_modal hash)
  (order page modal kind)]
[table page_enter
  (key page hash (domain rows))
  (order page)
  (suffix command)]
[table page_leave
  (key page hash (domain rows))
  (order page)
  (suffix command)]
[table modal_enter
  (key modal hash (domain rows))
  (order modal)
  (suffix command)]
[table modal_leave
  (key modal hash (domain rows))
  (order modal)
  (suffix command)]
[machine craft_bench
  (state page hash (members experiment recipes) (initial experiment))
  (state modal hash (members none confirm result) (initial none))
  (input kind hash)]
list,from,where,given,set,emit,overrides
entry,page_enter,page=state.page,,,command,
entry,modal_enter,modal=state.modal,,,command,
step,bench,page=state.page modal=state.modal kind=input.kind,,page=next_page modal=next_modal,,
exit,page_leave,page=state.page,,,command,
exit,modal_leave,modal=state.modal,,,command,
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
	std::string const source = Replace(Replace(kBench, "[machine craft_bench", "[table chime (column v int) (suffix command)]\n[machine craft_bench"),
		"entry,modal_enter,modal=state.modal,,,command,\n", "entry,modal_enter,modal=state.modal,,,command,\nentry,chime,,,,command,\n");
	machine_fixture f(source, {kBenchRows, kPageEnterRows, kPageLeaveRows, kModalEnterRows, kModalLeaveRows,
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
	std::string const source = Replace(Replace(kBench, "[table modal_enter\n  (key modal hash (domain rows))\n  (order modal)\n  (suffix command)]\n",
		"[table modal_enter\n  (key modal hash (domain rows))\n  (key page int (domain rows))\n  (order modal page)\n  (suffix command)]\n"
		"[table page_code\n  (key page hash (domain rows))\n  (column code int)\n  (order page)]\n"),
		"entry,page_enter,page=state.page,,,command,\nentry,modal_enter,modal=state.modal,,,command,\n",
		"entry,page_code,page=state.page,,,,\nentry,modal_enter,modal=state.modal page=clause.0.code,,,command,\n");
	machine_fixture f(source, {kBenchRows, kPageEnterRows, kPageLeaveRows,
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
[table behaviour
  (key super hash (domain rows))
  (key sub hash fallthrough)
  (key kind hash fallthrough)
  (column next_super hash)
  (column next_sub hash)
  (order super sub kind)]
[table super_enter
  (key super hash (domain rows))
  (order super)
  (suffix command)]
[table sub_enter
  (key sub hash (domain rows))
  (order sub)
  (suffix command)]
[machine creature
  (state super hash (members move rest) (initial move))
  (state sub hash (members walk run sit sleep) (initial walk))
  (input kind hash)]
list,from,where,given,set,emit,overrides
entry,super_enter,super=state.super,,,command,
entry,sub_enter,sub=state.sub,,,command,
step,behaviour,super=state.super sub=state.sub kind=input.kind,,super=next_super sub=next_sub,,
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
[table two
  (key a hash fallthrough)
  (key b hash fallthrough)
  (column said hash)
  (order a b)]
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
	auto s = ReadSchema(kTwo, "two.tabular");
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
	auto s = ReadSchema(kTwo, "two.tabular");
	ASSERT_TRUE(s.has_value()) << RefusalOf(s);
	relation_store store(std::move(*s));
	std::string const refused = RefusalOf(LoadCsv(store, csv_rows{0, 1, "a,b,said\n,y,b-held\nx,,a-held\n", "two.csv"}));
	EXPECT_TRUE(Mentions(refused, "two")) << refused;
	EXPECT_NE(refused, "(accepted)") << "the all-blank run is the catch-all, and it is missing";
}

// ---- Override clauses -------------------------------------------------------------------

namespace
{

// Two tables of one package: the first is total, the second (named for the
// conditions it carries) has rows only where it changes something, and its
// clause overrides the first's.
// Clause 2 reads the base clause's next, which is the coalesced answer.
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
[machine grazer
  (state mode hash (members idle walk sleep) (initial idle))
  (state steps int (initial 0))
  (state code int (initial 0))]
list,from,where,given,set,emit,overrides
step,brain,mode=state.mode,,mode=next steps=steps,command,
step,brain_mod,mode=state.mode,,mode=next,command,0
step,label,mode=clause.0.next,,code=code,,
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
	auto refusal = [](std::string const& source) { return RefusalOf(ReadDocument(source, "machine.tabular")); };
	std::string const notNext = refusal(Replace(kModded, "mode=next,command,0\n", "mode=next,command,2\n"));
	EXPECT_TRUE(Mentions(notNext, "an override names the clause directly before it")) << notNext;

	std::string const gap = refusal(Replace(kModded, "step,brain_mod,", "step,label,mode=state.mode,,,,\nstep,brain_mod,"));
	EXPECT_TRUE(Mentions(gap, "which does not directly precede it")) << gap;

	std::string const wider = refusal(Replace(Replace(kModded, "  (column next hash)\n  (order mode)\n  (suffix command)]\n[table label",
		"  (column next hash)\n  (column code int)\n  (order mode)\n  (suffix command)]\n[table label"),
		"step,brain_mod,mode=state.mode,,mode=next,command,0", "step,brain_mod,mode=state.mode,,mode=next code=code,command,0"));
	EXPECT_TRUE(Mentions(wider, "sets state column 'code', which clause 0 it overrides does not")) << wider;

	std::string const twice = refusal(Replace(kModded, "mode=next,command,0\n",
		"mode=next,command,0\nstep,brain_mod,mode=state.mode,,mode=next,,1\n"));
	EXPECT_TRUE(Mentions(twice, "which is itself an override")) << twice;

	std::string const emits = refusal(Replace(kModded, "mode=next steps=steps,command,", "mode=next steps=steps,,"));
	EXPECT_TRUE(Mentions(emits, "the clause emits, and clause 0 it overrides does not")) << emits;

	std::string const direct = refusal(Replace(kModded, "mode=clause.0.next", "mode=clause.1.next"));
	EXPECT_TRUE(Mentions(direct, "reads clause 1, which overrides clause 0; read clause 0")) << direct;
}

TEST(OverrideClause, ItNamesAClauseThatAlwaysAnswers)
{
	// A base keyed on a machine input closes nothing: a value outside its
	// domain would hold it for want of a run, and the override would have
	// nothing to coalesce onto.
	std::string const source = Replace(Replace(kModded, "(state code int (initial 0))]", "(state code int (initial 0))\n  (input kind hash)]"),
		"step,brain,mode=state.mode,", "step,brain,mode=input.kind,");
	std::string const refused = RefusalOf(ReadDocument(source, "machine.tabular"));
	EXPECT_TRUE(Mentions(refused, "machine 'grazer' step clause 1 (table 'brain_mod')")) << refused;
	EXPECT_TRUE(Mentions(refused, "whose key 'mode' of table 'brain' is read from a source that closes nothing")) << refused;
}

// ---- Many surfaces, one machine -----------------------------------------------------------

namespace
{

// One hierarchical machine in two forms. One table: the substate gives way
// first, then the event's kind (spec Part 4). Two tables: the superstate's
// rules, and an override with the substates' own rows, the two giving way
// level by level (owner, 2026-10-20). Each tries the same keys in the same
// order: (super, sub, kind), (super, _, kind), then (super, sub, _),
// (super, _, _), so a superstate's row for the event beats a substate's
// row for no event.
constexpr std::string_view kOneTable = R"(
[table rules
  (key super hash (domain rows))
  (key sub hash fallthrough)
  (key kind hash fallthrough)
  (column next_super hash)
  (column next_sub hash)
  (order super sub kind)
  (suffix command)]
[machine creature
  (state super hash (members move rest) (initial move))
  (state sub hash (members walk run sit sleep) (initial walk))
  (input kind hash)]
list,from,where,given,set,emit,overrides
step,rules,super=state.super kind=input.kind sub=state.sub,,super=next_super sub=next_sub,command,
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
[table super_rules
  (key super hash (domain rows))
  (key kind hash fallthrough)
  (column next_super hash)
  (column next_sub hash)
  (order super kind)
  (suffix command)]
[table sub_rules
  (key super hash (domain rows))
  (key kind hash fallthrough)
  (key sub hash (domain rows))
  (column next_super hash)
  (column next_sub hash)
  (order super kind sub)
  (suffix command)]
[machine creature
  (state super hash (members move rest) (initial move))
  (state sub hash (members walk run sit sleep) (initial walk))
  (input kind hash)]
list,from,where,given,set,emit,overrides
step,super_rules,super=state.super kind=input.kind,,super=next_super sub=next_sub,command,
step,sub_rules,super=state.super kind=input.kind sub=state.sub,,super=next_super sub=next_sub,command,0
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
	EXPECT_EQ(a.State(0).i, Hash("move"));
	EXPECT_EQ(a.State(1).i, Hash("walk")) << "wake got the sleeper up, and nap and wake move nothing while walking";
}

// The owner's case (2026-10-20): asleep, `wake` finds the superstate's
// (rest, wake) before the substate's (rest, sleep, _), and gets up; `dance`,
// which no one has a row for, finds (rest, sleep, _) before (rest, _), and
// snores on. In both forms.
TEST(ManySurfaces, ASuperstatesRowForTheEventBeatsASubstatesRowForNone)
{
	machine_fixture one(kOneTable, {kOneTableRows});
	machine_fixture two(kTwoTables, {kSuperRows, kSubRows});
	ASSERT_TRUE(one.binding) << one.refusal;
	ASSERT_TRUE(two.binding) << two.refusal;
	for (machine_fixture const* form : {&one, &two})
	{
		host_object host;
		instance creature = CreateInstance(*form->binding, 1, 1, &host);
		Dispatch(creature, host, "hit", 2);
		Dispatch(creature, host, "nap", 3);
		ASSERT_EQ(creature.State(1).i, Hash("sleep"));
		EXPECT_EQ(Dispatch(creature, host, "dance", 4), std::vector<std::string>{"((snore))"});
		EXPECT_EQ(creature.State(1).i, Hash("sleep")) << "no row for dance: the substate's own row for none";
		EXPECT_EQ(Dispatch(creature, host, "wake", 5), std::vector<std::string>{"((get up))"});
		EXPECT_EQ(creature.State(0).i, Hash("move"));
		EXPECT_EQ(creature.State(1).i, Hash("walk")) << "the superstate's row for wake, not the substate's for none";
	}
	host_object host;
	instance creature = CreateInstance(*two.binding, 1, 1, &host);
	Dispatch(creature, host, "hit", 2);
	Dispatch(creature, host, "nap", 3);
	Dispatch(creature, host, "wake", 4);
	EXPECT_EQ(creature.Answer(trigger::Step, 1).outcome, clause_outcome::Missed)
		<< "the override found (rest, _, sleep), less specific than the base's (rest, wake)";
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

// ---- Review close-out: the two forms with entry and exit, and overrides in
// every list ----------------------------------------------------------------------------

namespace
{

// Entry and exit tables for the superstate and the substate, appended to both
// forms of kOneTable / kTwoTables so a mode change's leaving and entering are
// compared too.
constexpr std::string_view kEntryExitTables = R"([table super_enter
  (key super hash (domain rows))
  (order super)
  (suffix command)]
[table sub_enter
  (key sub hash (domain rows))
  (order sub)
  (suffix command)]
[table super_leave
  (key super hash (domain rows))
  (order super)
  (suffix command)]
[table sub_leave
  (key sub hash (domain rows))
  (order sub)
  (suffix command)]
)";

constexpr std::string_view kEntryExitLists = R"(exit,super_leave,super=state.super,,,command,
exit,sub_leave,sub=state.sub,,,command,
entry,super_enter,super=state.super,,,command,
entry,sub_enter,sub=state.sub,,,command,
)";

constexpr std::string_view kSuperEnter = "super,command\nmove,enter-move\nrest,enter-rest\n";
constexpr std::string_view kSubEnter   = "sub,command\nwalk,enter-walk\nrun,enter-run\nsit,enter-sit\nsleep,enter-sleep\n";
constexpr std::string_view kSuperLeave = "super,command\nmove,leave-move\nrest,leave-rest\n";
constexpr std::string_view kSubLeave   = "sub,command\nwalk,leave-walk\nrun,leave-run\nsit,leave-sit\nsleep,leave-sleep\n";

std::string WithEntryAndExit(std::string_view source)
{
	return Replace(source, "[machine creature", std::string(kEntryExitTables) + "[machine creature") + std::string(kEntryExitLists);
}

}

TEST(ManySurfaces, TheTwoFormsLeaveAndEnterTheSameAcrossModeChanges)
{
	machine_fixture one(WithEntryAndExit(kOneTable), {kOneTableRows, kSuperEnter, kSubEnter, kSuperLeave, kSubLeave});
	machine_fixture two(WithEntryAndExit(kTwoTables), {kSuperRows, kSubRows, kSuperEnter, kSubEnter, kSuperLeave, kSubLeave});
	ASSERT_TRUE(one.binding) << one.refusal;
	ASSERT_TRUE(two.binding) << two.refusal;
	host_object host;
	instance a = CreateInstance(*one.binding, 1, 1, &host);
	instance b = CreateInstance(*two.binding, 1, 1, &host);
	EXPECT_EQ(Texts(a.Emitted()), Texts(b.Emitted()));
	EXPECT_EQ(Texts(a.Emitted()), (std::vector<std::string>{"(enter-move)", "(enter-walk)"}));
	std::string_view const events[] = {"", "fast", "dance", "hit", "hit", "wake", "hit", "", "nap", "wake", ""};
	uint64_t seed = 2;
	int superChanges = 0;
	for (std::string_view kind : events)
	{
		std::vector<std::string> const fromOne = Dispatch(a, host, kind, seed);
		std::vector<std::string> const fromTwo = Dispatch(b, host, kind, seed);
		EXPECT_EQ(fromOne, fromTwo) << "step " << seed << " on '" << kind << "'";
		EXPECT_EQ(a.State(0).i, b.State(0).i) << "step " << seed << " on '" << kind << "'";
		EXPECT_EQ(a.State(1).i, b.State(1).i) << "step " << seed << " on '" << kind << "'";
		for (std::string const& text : fromOne)
		{
			superChanges += text == "(leave-move)" || text == "(leave-rest)" ? 1 : 0;
		}
		++seed;
	}
	EXPECT_GE(superChanges, 3) << "the events cross between move and rest, so the superstate's exit is compared";
	EXPECT_EQ(Texts(Close(a, &host, seed)), Texts(Close(b, &host, seed)));
}

namespace
{

// An exit list with an override: leaving walk while crouched has its own
// command. The override keys on pose as well as mode, so the two run as one
// group on both columns.
constexpr std::string_view kCrouch = R"(
[table brain
  (key mode hash (domain rows))
  (key kind hash fallthrough)
  (column next_mode hash)
  (column next_pose hash)
  (order mode kind)]
[table leave
  (key mode hash (domain rows))
  (order mode)
  (suffix command)]
[table leave_mod
  (key mode hash (domain rows))
  (key pose hash (domain rows))
  (order mode pose)
  (suffix command)]
[machine walker
  (state mode hash (members idle walk) (initial idle))
  (state pose hash (members up down) (initial up))
  (input kind hash)]
list,from,where,given,set,emit,overrides
step,brain,mode=state.mode kind=input.kind,,mode=next_mode pose=next_pose,,
exit,leave,mode=state.mode,,,command,
exit,leave_mod,mode=state.mode pose=state.pose,,,command,0
)";

constexpr std::string_view kCrouchRows = R"(mode, kind, next_mode, next_pose
idle, ,     idle,      up
idle, go,   walk,      up
idle, duck, idle,      down
walk, ,     walk,      up
walk, stop, idle,      up
walk, duck, walk,      down
)";

constexpr std::string_view kLeaveRows    = "mode,command\nidle,leave idle\nwalk,leave walk\n";
constexpr std::string_view kLeaveModRows = "mode,pose,command\nwalk,down,leave walk crouched\n";

}

TEST(OverrideClause, InAnExitListItRunsWithItsBaseOnTheUnionOfTheirColumns)
{
	machine_fixture f(kCrouch, {kCrouchRows, kLeaveRows, kLeaveModRows});
	ASSERT_TRUE(f.binding) << f.refusal;
	host_object host;
	instance walker = CreateInstance(*f.binding, 1, 1, &host);
	EXPECT_EQ(Dispatch(walker, host, "duck", 2), (std::vector<std::string>{"((leave idle))"}))
		<< "a pose-only change runs the group, the base with its override; (idle, up) is a miss";
	EXPECT_EQ(walker.Answer(trigger::Exit, 1).outcome, clause_outcome::Missed);
	EXPECT_EQ(Dispatch(walker, host, "go", 3), (std::vector<std::string>{"((leave idle))"}));
	EXPECT_EQ(Dispatch(walker, host, "duck", 4), (std::vector<std::string>{"((leave walk))"}));
	EXPECT_EQ(Dispatch(walker, host, "stop", 5), (std::vector<std::string>{"((leave walk crouched))"}))
		<< "leaving (walk, down): the override's hit replaces the base's command";
	EXPECT_EQ(walker.Answer(trigger::Exit, 1).outcome, clause_outcome::Answered);
	EXPECT_EQ(Dispatch(walker, host, "", 6), (std::vector<std::string>{})) << "nothing changed: the group does not run";
	EXPECT_EQ(walker.Answer(trigger::Exit, 0).outcome, clause_outcome::NotRun);
	EXPECT_EQ(walker.Answer(trigger::Exit, 1).outcome, clause_outcome::NotRun);
}

TEST(OverrideClause, CloseRunsAnExitListsOverride)
{
	machine_fixture f(kCrouch, {kCrouchRows, kLeaveRows, kLeaveModRows});
	ASSERT_TRUE(f.binding) << f.refusal;
	host_object host;
	instance crouched = CreateInstance(*f.binding, 1, 1, &host);
	Dispatch(crouched, host, "go", 2);
	Dispatch(crouched, host, "duck", 3);
	EXPECT_EQ(Texts(Close(crouched, &host, 4)), (std::vector<std::string>{"((leave walk crouched))"}));
	EXPECT_EQ(crouched.Answer(trigger::Exit, 1).outcome, clause_outcome::Answered);

	instance standing = CreateInstance(*f.binding, 2, 1, &host);
	Dispatch(standing, host, "go", 2);
	EXPECT_EQ(Texts(Close(standing, &host, 3)), (std::vector<std::string>{"((leave walk))"}));
	EXPECT_EQ(standing.Answer(trigger::Exit, 1).outcome, clause_outcome::Missed);
#if TABULAR_PROVENANCE
	provenance_view const picks = standing.Provenance();
	ASSERT_GE(picks.size(), 2u);
	EXPECT_EQ(picks[picks.size() - 2].run, provenance_run::Close);
	EXPECT_EQ(picks[picks.size() - 1].run, provenance_run::Close);
	EXPECT_EQ(picks[picks.size() - 1].clause, 1u);
	EXPECT_EQ(picks[picks.size() - 1].answer.outcome, clause_outcome::Missed);
#endif
}

TEST(OverrideClause, AnOverrideThatFailsIsReportedAndItsBasesAnswerStands)
{
	// The mod's first row is a script condition, the second its fallback: the
	// override answers either way, unless the script raises.
	std::string const source = Replace(kModded, "[table brain_mod\n  (key mode hash (domain rows))\n",
		"[table brain_mod\n  (key mode hash (domain rows))\n  (column when condition)\n");
	machine_fixture f(source, {kBaseRows, "mode,when,next,command\nidle,mod_on,sleep,anim yawn\nidle,,walk,anim stroll\n",
		kLabelRows});
	ASSERT_TRUE(f.binding) << f.refusal;
	int reports = 0;
	host_binding host = KindHost();
	host.report = [&reports](uint64_t, uint32_t, size_t, ErrorContext const&) { ++reports; };
	auto binding = Bind(f.machine, std::move(host));
	ASSERT_TRUE(binding.has_value()) << RefusalOf(binding);

	host_object object;
	object.mod_on = true;
	instance on = CreateInstance(**binding, 1, 1, &object);
	EXPECT_EQ(Texts(Step(on, &object, 2)), (std::vector<std::string>{"((anim yawn))"}));
	object.mod_on = false;
	instance off = CreateInstance(**binding, 2, 1, &object);
	EXPECT_EQ(Texts(Step(off, &object, 2)), (std::vector<std::string>{"((anim stroll))"}))
		<< "a condition that holds false is not a miss: the mod's fallback row answers";
	EXPECT_EQ(reports, 0);

	object.broken = true;
	instance broken = CreateInstance(**binding, 3, 1, &object);
	EXPECT_EQ(Texts(Step(broken, &object, 2)), (std::vector<std::string>{"((anim walk))"}))
		<< "the base's command, since the override did not answer";
	EXPECT_EQ(broken.Answer(trigger::Step, 1).outcome, clause_outcome::Failed);
	EXPECT_EQ(broken.Answer(trigger::Step, 0).outcome, clause_outcome::Answered);
	EXPECT_EQ(broken.State(0).i, Hash("walk")) << "the base's sets are written whole";
	EXPECT_EQ(broken.State(1).i, 1);
	EXPECT_EQ(broken.State(2).i, 20) << "the dependent read the base's next";
	EXPECT_EQ(reports, 1) << "a failed override is reported as any clause is";
}

TEST(OverrideClause, AHitWithNoEmitKeepsTheBasesCommand)
{
	// The column-subset rule: an override that sets mode but declares no emit
	// replaces mode only, so the base's command goes out with the mod's state.
	std::string const source = Replace(Replace(kModded, "  (column next hash)\n  (order mode)\n  (suffix command)]\n[table label",
		"  (column next hash)\n  (order mode)]\n[table label"),
		"mode=next,command,0", "mode=next,,0");
	machine_fixture f(source, {kBaseRows, "mode,next\nidle,sleep\n", kLabelRows});
	ASSERT_TRUE(f.binding) << f.refusal;
	host_object host;
	instance grazer = CreateInstance(*f.binding, 1, 1, &host);
	EXPECT_EQ(Texts(Step(grazer, &host, 2)), (std::vector<std::string>{"((anim walk))"}));
	EXPECT_EQ(grazer.State(0).i, Hash("sleep"));
	EXPECT_EQ(grazer.State(1).i, 1);
	EXPECT_EQ(grazer.State(2).i, 30);
	EXPECT_EQ(grazer.Answer(trigger::Step, 1).outcome, clause_outcome::Answered);
}

TEST(OverrideClause, AnOverridesOwnFallThroughFeedsTheCoalescedAnswer)
{
	// kTwoTables' override falls through on kind; a reader of clause 0's
	// next_sub reads what the override answered, retry included.
	std::string const source = Replace(Replace(Replace(std::string(kTwoTables), "[machine creature",
		"[table sub_code\n  (key sub hash (domain rows))\n  (column code int)\n  (order sub)]\n[machine creature"),
		"(state sub hash (members walk run sit sleep) (initial walk))",
		"(state sub hash (members walk run sit sleep) (initial walk))\n  (state code int (initial 0))"),
		"sub=state.sub,,super=next_super sub=next_sub,command,0\n",
		"sub=state.sub,,super=next_super sub=next_sub,command,0\nstep,sub_code,sub=clause.0.next_sub,,code=code,,\n");
	machine_fixture f(source, {kSuperRows, kSubRows, "sub,code\nwalk,1\nrun,2\nsit,3\nsleep,4\n"});
	ASSERT_TRUE(f.binding) << f.refusal;
	host_object host;
	instance creature = CreateInstance(*f.binding, 1, 1, &host);
	Dispatch(creature, host, "fast", 2);
	EXPECT_EQ(creature.Answer(trigger::Step, 1).outcome, clause_outcome::Missed);
	EXPECT_EQ(creature.State(2).i, 2) << "(move, fast, walk) and (move, _, walk) miss: the base's run";
	Dispatch(creature, host, "dance", 3);
	EXPECT_TRUE(creature.Answer(trigger::Step, 1).fellThrough);
	EXPECT_EQ(creature.State(1).i, Hash("run"));
	EXPECT_EQ(creature.State(2).i, 2) << "(move, _, run) answered by retry; the base alone would say walk, 1";
	Dispatch(creature, host, "hit", 4);
	EXPECT_EQ(creature.State(1).i, Hash("walk"));
	EXPECT_EQ(creature.State(2).i, 1) << "(move, hit, run): the override's stumble, not the base's sit";
}

TEST(Hierarchy, ANinthFallThroughKeyIsRefused)
{
	auto table = [](int keys)
	{
		std::string source = "[table wide\n";
		std::string order;
		for (int k = 1; k <= keys; ++k)
		{
			source += "  (key k" + std::to_string(k) + " hash fallthrough)\n";
			order += (k == 1 ? "" : " ") + std::string("k") + std::to_string(k);
		}
		return source + "  (column said int)\n  (order " + order + ")]\n";
	};
	EXPECT_TRUE(ReadSchema(table(8), "wide.tabular").has_value()) << RefusalOf(ReadSchema(table(8), "wide.tabular"));
	std::string const refused = RefusalOf(ReadSchema(table(9), "wide.tabular"));
	EXPECT_TRUE(Mentions(refused, "is fall-through key 9")) << refused;
	EXPECT_TRUE(Mentions(refused, "at most 8")) << refused;
}

TEST(OverrideClause, ANonTotalOverrideTableNeedsARowsDomain)
{
	// "Need not be total" is the machine's rule; a table's own load still
	// checks it is total over its declared domain. A mod naming only idle
	// over members [idle, walk, sleep] is refused at load, so a partial
	// override declares domain: rows (its domain is the keys it has).
	std::string const source = Replace(kModded, "[table brain_mod\n  (key mode hash (domain rows))",
		"[table brain_mod\n  (key mode hash (domain (members idle walk sleep)))");
	auto doc = ReadDocument(source, "machine.tabular");
	ASSERT_TRUE(doc.has_value()) << RefusalOf(doc);
	relation_store store(std::move(doc->tables));
	std::string const refused = RefusalOf(LoadCsv(store, csv_rows{1, 1, kModRows, "mod.csv"}));
	EXPECT_TRUE(Mentions(refused, "table 'brain_mod' key")) << refused;
	EXPECT_TRUE(Mentions(refused, "the key is in the table's domain")) << refused;
	EXPECT_EQ(RefusalOf(LoadCsv(store, csv_rows{1, 1, "mode,next,command\nidle,sleep,()\nwalk,walk,()\nsleep,idle,()\n",
		"mod.csv"})), "(accepted)") << "the same table, total over its members, loads";
}

// The levels line up only when the override falls through on its base's
// fall-through keys, read from the same source.
TEST(OverrideClause, ItFallsThroughOnItsBasesFallThroughKeys)
{
	std::string const keyed = Replace(kTwoTables, "(key kind hash fallthrough)\n  (key sub hash (domain rows))",
		"(key kind hash (domain rows))\n  (key sub hash (domain rows))");
	std::string const refused = RefusalOf(ReadDocument(keyed, "machine.tabular"));
	EXPECT_TRUE(Mentions(refused, "machine 'creature' step clause 1 (table 'sub_rules')")) << refused;
	EXPECT_TRUE(Mentions(refused, "whose fall-through key 'kind' of table 'super_rules' it does not fall through on")) << refused;
}

namespace
{

// kTwoTables with a substate key that falls through in the override too, and
// after the event's kind in key order: the override's own key, sub, still
// gives way first, inside each level of the base's kind.
std::string WithFallingSub()
{
	return Replace(kTwoTables, "(key kind hash fallthrough)\n  (key sub hash (domain rows))",
		"(key kind hash fallthrough)\n  (key sub hash fallthrough)");
}

// The override's catch-all for each superstate, a row for wake with any
// substate of rest, and the sleeper's own row for none.
constexpr std::string_view kFallingSubRows = R"(super, kind, sub,   next_super, next_sub, command
move,  ,     ,      move,       walk,     (o-move)
rest,  ,     ,      rest,       sit,      (o-rest)
rest,  wake, ,      rest,       sit,      (o-wake)
rest,  ,     sleep, rest,       sleep,    (snore)
)";

}

// Resting and sitting, wake: the override's (rest, wake, _) blanks only its
// own key, so it ties the base's (rest, wake) and wins the tie. With no event,
// the override's (rest, _, _) ties the base's (rest, _): both read kind blank.
// Asleep, dance: the override's (rest, _, sleep) and the base's (rest, _) are
// both at the level with kind blanked, and the override's wins.
TEST(OverrideClause, ItsOwnFallThroughKeysGiveWayFirstInsideEachLevel)
{
	machine_fixture f(WithFallingSub(), {kSuperRows, kFallingSubRows});
	ASSERT_TRUE(f.binding) << f.refusal;
	host_object host;
	instance creature = CreateInstance(*f.binding, 1, 1, &host);
	Dispatch(creature, host, "hit", 2);
	ASSERT_EQ(creature.State(1).i, Hash("sit"));
	EXPECT_EQ(Dispatch(creature, host, "wake", 3), std::vector<std::string>{"((o-wake))"})
		<< "(rest, wake, _) is as specific as (rest, wake) once sub, the override's own key, gives way";
	EXPECT_EQ(Dispatch(creature, host, "", 4), std::vector<std::string>{"((o-rest))"});
	Dispatch(creature, host, "nap", 5);
	ASSERT_EQ(creature.State(1).i, Hash("sleep"));
	EXPECT_EQ(Dispatch(creature, host, "dance", 6), std::vector<std::string>{"((snore))"});
}

// A twin is found by source, not by name: here both of the override's
// fall-through keys read state.sub, so none reads the base's input.kind.
TEST(OverrideClause, ItsTwinKeyReadsTheSameSourceAsTheBasesKey)
{
	std::string const crossed = Replace(WithFallingSub(), "super=state.super kind=input.kind sub=state.sub,,super=next_super sub=next_sub,command,0",
		"super=state.super kind=state.sub sub=state.sub,,super=next_super sub=next_sub,command,0");
	std::string const refused = RefusalOf(ReadDocument(crossed, "machine.tabular"));
	EXPECT_TRUE(Mentions(refused, "whose fall-through key 'kind' of table 'super_rules' it does not fall through on")) << refused;
}

// Two fall-through keys the override declares in the other key order: its
// levels follow the base's order (x, the base's first, gives way first), not
// its own key order.
TEST(OverrideClause, ItsLevelsFollowTheBasesOrderOfFallThroughKeys)
{
	constexpr std::string_view source = R"(
[table base
  (key x hash fallthrough)
  (key y hash fallthrough)
  (column next hash)
  (order x y)]
[table over
  (key y hash fallthrough)
  (key x hash fallthrough)
  (column next hash)
  (order y x)]
[machine twins
  (state mood hash (members p calm) (initial p))
  (state last hash (members base0 basep baseq over0 overq) (initial base0))
  (input kind hash)]
list,from,where,given,set,emit,overrides
step,base,x=state.mood y=input.kind,,last=next,,
step,over,x=state.mood y=input.kind,,last=next,,0
)";
	machine_fixture f(source, {"x, y, next\n , , base0\np, , basep\n , q, baseq\n", "y, x, next\n , , over0\nq, , overq\n"});
	ASSERT_TRUE(f.binding) << f.refusal;
	host_object host;
	instance subject = CreateInstance(*f.binding, 1, 1, &host);
	Dispatch(subject, host, "q", 2);
	EXPECT_EQ(subject.State(1).i, Hash("overq"))
		<< "the base answers (_, q) with x blanked; the override's (q, _) is x blanked too, a tie it wins";
}
