#include <gtest/gtest.h>
#include "../src/machine_internal.h"
#include "Support/allocationtestingsupport.h"
#include "tabular/document.h"
#include "tabular/draw.h"
#include "tabular/instance.h"
#include "tabular/machine.h"
#include "tabular/pick.h"
#include "tabular/relation.h"
#include "tabular/rows_csv.h"
#include "cityhash/src/city.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace tabular;

// Binding, instances and Step: a prepared machine bound to a host's getters,
// an instance holding its current and next state vectors, and the step that
// runs the prepared query. The interpreted picks (PickBest, PickOr,
// PickWeighted) are the oracle the step is held to.

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

// The host object an instance belongs to: what its getters read and its
// script conditions test. broken makes every condition fail to answer.
struct host_object
{
	double   energy    = 0.8;
	double   threat    = 0.1;
	double   t         = 0;
	int64_t  food_near = 0;   // a flag set, widened; wider than 32 bits on purpose in the parity test
	int64_t  message   = 0;
	bool     cornered  = false;
	bool     broken    = false;
	bool     unwired   = false;   // the threat getter cannot answer
	bool     terse     = false;   // failures say so in a message short enough to allocate nothing
};

host_object const& Host(void const* self)
{
	return *static_cast<host_object const*>(self);
}

cell ReadEnergy(void*, void const* self, ErrorContext*)
{
	return cell::Real(Host(self).energy);
}

cell ReadThreat(void*, void const* self, ErrorContext* error_out)
{
	if (Host(self).unwired)
	{
		*error_out = ErrorContext(Host(self).terse ? "no target" : "threat's wire has no target");
		return cell::Real(0);
	}
	return cell::Real(Host(self).threat);
}

cell ReadT(void*, void const* self, ErrorContext*)
{
	return cell::Real(Host(self).t);
}

cell ReadFoodNear(void*, void const* self, ErrorContext*)
{
	return cell::Integer(Host(self).food_near);
}

cell ReadMessage(void*, void const* self, ErrorContext*)
{
	return cell::Integer(Host(self).message);
}

// Resolves the inputs every test machine here declares, by name, once.
host_binding HostGetters()
{
	host_binding host;
	host.resolve = [](std::string_view name) -> tl::expected<input_getter, ErrorContext>
	{
		static std::map<std::string_view, cell (*)(void*, void const*, ErrorContext*)> const reads = {
			{"energy", &ReadEnergy}, {"threat", &ReadThreat}, {"t", &ReadT}, {"food_near", &ReadFoodNear},
			{"message", &ReadMessage}};
		auto found = reads.find(name);
		if (found == reads.end())
		{
			return tl::make_unexpected(ErrorContext("no wire named '", name, "'"));
		}
		return input_getter{found->second, nullptr, nullptr};
	};
	return host;
}

// cornered() holds when the host is cornered; panicky() when it is not. Both
// fail to answer when the host is broken.
bool Cornered(void*, void const* self, ErrorContext* error_out)
{
	if (Host(self).broken)
	{
		*error_out = ErrorContext(Host(self).terse ? "dead target" : "cornered() read a dead target");
		return false;
	}
	return Host(self).cornered;
}

bool Panicky(void*, void const* self, ErrorContext* error_out)
{
	if (Host(self).broken)
	{
		*error_out = ErrorContext(Host(self).terse ? "dead target" : "panicky() read a dead target");
		return false;
	}
	return !Host(self).cornered;
}

tl::expected<closure, ErrorContext> Conditions(std::string_view text)
{
	if (text == "cornered")
	{
		return closure{&Cornered, nullptr, nullptr};
	}
	if (text == "panicky")
	{
		return closure{&Panicky, nullptr, nullptr};
	}
	return tl::make_unexpected(ErrorContext("no condition '", text, "'"));
}

// A document's tables in a store, with the rows given per table in table
// order, and its first machine prepared and bound to HostGetters.
struct machine_fixture
{
	schema_document                 document;
	relation_store                  store;
	counted_ptr<Machine const>      machine;
	counted_ptr<Binding const>      binding;

	machine_fixture(std::string_view source, std::vector<std::string_view> const& rows)
		: document(Parse(source)), store(std::move(document.tables))
	{
		// A refused document already failed the test; a run on it would crash it.
		if (document.machines().empty())
		{
			return;
		}
		for (uint32_t t = 0; t < rows.size(); ++t)
		{
			csv_rows load;
			load.table     = t;
			load.owner     = 1;
			load.text      = rows[t];
			load.label     = "rows.csv";
			load.condition = &Conditions;
			EXPECT_EQ(RefusalOf(LoadCsv(store, load)), "(accepted)") << "table " << t;
		}
		auto prepared = Prepare(document.machines()[0], store);
		EXPECT_TRUE(prepared.has_value()) << RefusalOf(prepared);
		if (!prepared)
		{
			return;
		}
		machine = *prepared;
		auto bound = Bind(machine, HostGetters());
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

	TableVersion const& Version(trigger list, size_t clause) const
	{
		return *machine_access::Of(*machine).lists[size_t(list)][clause].version;
	}

	instance Create(host_object const& host, uint64_t seed = 1)
	{
		return CreateInstance(*binding, 7, seed, &host);
	}
};

cell ValueOf(TableVersion const& version, size_t row, std::string_view column)
{
	for (uint32_t c = 0; c < version.definition().columns.size(); ++c)
	{
		if (version.definition().columns[c].name == column)
		{
			auto value = version.Value(row, c);
			EXPECT_TRUE(value.has_value()) << RefusalOf(value);
			return value ? *value : cell::Integer(0);
		}
	}
	ADD_FAILURE() << "no column " << column;
	return cell::Integer(0);
}

// A step on an event of the named type: the host fills the message input for
// this one step, as a host dispatching an event does.
std::span<emitted const> StepOn(instance& subject, host_object& host, uint64_t seed, std::string_view type)
{
	host.message = Hash(type);
	std::span<emitted const> const results = Step(subject, &host, seed);
	host.message = 0;
	return results;
}

// ---- The worked example: the grazer of the design sketch ------------------------
//
// One hash state, mode, and an int anim that the entry clause sets from the
// state entered and a step clause clears every step, so anim is non-zero
// exactly on a step that entered a state. Threat drives graze -> flee; a
// cornered grazer fights instead (the one script condition); falling threat
// brings it back to graze.

constexpr std::string_view kGrazer = R"(
[table settle
  (key mode hash (domain rows))
  (column anim int)
  (order mode)]
[table grazer_brain
  (input energy double 0..1)
  (input threat double 0..1)
  (key mode hash (domain rows))
  (column energy band (input energy))
  (column threat band (input threat))
  (column when condition)
  (column next hash)
  (order mode)]
[table grazer_enter
  (key mode hash (domain rows))
  (column anim int)
  (order mode)
  (suffix command)]
[machine grazer
  (state mode hash (members graze flee freeze fight) (initial graze))
  (state anim int (initial 0))
  (input energy double 0..1)
  (input threat double 0..1)]
list,from,where,given,set,emit,overrides
entry,grazer_enter,mode=state.mode,,anim=anim,command,
step,settle,mode=state.mode,,anim=anim,,
step,grazer_brain,mode=state.mode,energy=input.energy threat=input.threat,mode=next,,
)";

constexpr std::string_view kSettleRows = "mode,anim\ngraze,0\nflee,0\nfreeze,0\nfight,0\n";

constexpr std::string_view kGrazerBrainRows = R"(mode, energy, threat, when, next
graze,  0..0.1, 0..0.5, ,         freeze
graze,  0..1,   0..0.5, ,         graze
graze,  0..1,   0.5..1, cornered, fight
graze,  0..1,   0.5..1, ,         flee
flee,   0..1,   0..0.3, ,         graze
flee,   0..1,   0.3..1, cornered, fight
flee,   0..1,   0.3..1, ,         flee
fight,  0..1,   0..0.3, ,         graze
fight,  0..1,   0.3..1, ,         fight
freeze, 0..0.2, 0..1,   ,         freeze
freeze, 0.2..1, 0..1,   ,         graze
)";

constexpr std::string_view kGrazerEnterRows = R"(mode, anim, command
graze,  1, (anim graze)
flee,   2, (anim run)
fight,  3, (anim fight)
freeze, 4, (anim freeze)
)";

enum grazer_state : size_t { Mode, Anim };

machine_fixture Grazer()
{
	return machine_fixture(kGrazer, {kSettleRows, kGrazerBrainRows, kGrazerEnterRows});
}

}

TEST(GrazerStep, TheWorkedExampleRunsThroughFleeFightAndGraze)
{
	machine_fixture f = Grazer();
	ASSERT_TRUE(f.binding);
	host_object host;
	instance grazer = f.Create(host);
	EXPECT_EQ(grazer.id(), 7u);
	EXPECT_EQ(grazer.State(Mode).i, Hash("graze"));
	EXPECT_EQ(grazer.State(Anim).i, 1) << "creation enters the initial state";

	Step(grazer, &host, 2);
	EXPECT_EQ(grazer.State(Mode).i, Hash("graze"));
	EXPECT_EQ(grazer.State(Anim).i, 0) << "nothing was entered, so entry did not run";
	EXPECT_EQ(grazer.Answer(trigger::Entry, 0).outcome, clause_outcome::NotRun);

	host.threat = 0.7;
	Step(grazer, &host, 3);
	EXPECT_EQ(grazer.State(Mode).i, Hash("flee")) << "threat rose";
	EXPECT_EQ(grazer.State(Anim).i, 2) << "entering flee ran its entry clause";
	EXPECT_EQ(grazer.Answer(trigger::Entry, 0).outcome, clause_outcome::Answered);

	Step(grazer, &host, 4);
	EXPECT_EQ(grazer.State(Mode).i, Hash("flee"));
	EXPECT_EQ(grazer.State(Anim).i, 0);

	host.cornered = true;
	Step(grazer, &host, 5);
	EXPECT_EQ(grazer.State(Mode).i, Hash("fight")) << "cornered";
	EXPECT_EQ(grazer.State(Anim).i, 3);

	host.threat   = 0.1;
	host.cornered = false;
	Step(grazer, &host, 6);
	EXPECT_EQ(grazer.State(Mode).i, Hash("graze")) << "threat fell";
	EXPECT_EQ(grazer.State(Anim).i, 1);

	host.energy = 0.05;
	Step(grazer, &host, 7);
	EXPECT_EQ(grazer.State(Mode).i, Hash("freeze")) << "starving";
	EXPECT_EQ(grazer.State(Anim).i, 4);
}

TEST(GrazerStep, TheEntryClausePicksTheEnteredStatesRowAndReturnsItsCommand)
{
	machine_fixture f = Grazer();
	ASSERT_TRUE(f.binding);
	host_object host;
	instance grazer = f.Create(host);
	host.threat = 0.9;
	std::span<emitted const> const results = Step(grazer, &host, 1);
	clause_answer const entry = grazer.Answer(trigger::Entry, 0);
	ASSERT_EQ(entry.outcome, clause_outcome::Answered);
	TableVersion const& enter = f.Version(trigger::Entry, 0);
	auto command = enter.CommandAt(entry.row, uint32_t(enter.definition().columns.size() - 1));
	ASSERT_TRUE(command.has_value()) << RefusalOf(command);
	EXPECT_EQ(*command, "((anim run))");
	ASSERT_EQ(results.size(), 1u);
	EXPECT_EQ(results[0].list, trigger::Entry);
	EXPECT_EQ(results[0].command, *command);
}

TEST(GrazerStep, ACornerCheckThatCannotAnswerHoldsTheBrain)
{
	machine_fixture f = Grazer();
	ASSERT_TRUE(f.binding);
	host_object host;
	instance grazer = f.Create(host);
	host.threat = 0.9;
	host.broken = true;
	Step(grazer, &host, 1);
	EXPECT_EQ(grazer.Answer(trigger::Step, 1).outcome, clause_outcome::Failed);
	EXPECT_EQ(grazer.State(Mode).i, Hash("graze")) << "the brain held: mode is unwritten, so it keeps its value";
	EXPECT_EQ(grazer.Answer(trigger::Step, 0).outcome, clause_outcome::Answered) << "settle has no condition";
	EXPECT_EQ(grazer.State(Anim).i, 0);
}

TEST(GrazerStep, ANanThreatHoldsTheBrain)
{
	machine_fixture f = Grazer();
	ASSERT_TRUE(f.binding);
	host_object host;
	instance grazer = f.Create(host);
	host.threat = std::numeric_limits<double>::quiet_NaN();
	Step(grazer, &host, 1);
	EXPECT_EQ(grazer.Answer(trigger::Step, 1).outcome, clause_outcome::Failed);
	EXPECT_EQ(grazer.State(Mode).i, Hash("graze"));
}

TEST(GrazerStep, AGetterThatCannotAnswerHoldsTheClausesReadingIt)
{
	machine_fixture f = Grazer();
	ASSERT_TRUE(f.binding);
	host_object host;
	instance grazer = f.Create(host);
	host.threat  = 0.9;
	host.unwired = true;
	Step(grazer, &host, 1);
	EXPECT_EQ(grazer.Answer(trigger::Step, 1).outcome, clause_outcome::SourceHeld);
	EXPECT_EQ(grazer.State(Mode).i, Hash("graze"));
	EXPECT_EQ(grazer.Answer(trigger::Step, 0).outcome, clause_outcome::Answered) << "settle reads no input";
}

TEST(GrazerStep, InputsOutsideTheRangeAreClampedAsAPickClampsThem)
{
	machine_fixture f = Grazer();
	ASSERT_TRUE(f.binding);
	host_object host;
	instance grazer = f.Create(host);
	host.threat = 5.0;
	Step(grazer, &host, 1);
	EXPECT_EQ(grazer.State(Mode).i, Hash("flee")) << "5 clamps to the top of 0..1, in the 0.5..1 band";
	host.threat = 1.0;
	Step(grazer, &host, 2);
	EXPECT_EQ(grazer.State(Mode).i, Hash("flee")) << "the range's top is in the top band";
	host.threat = -3.0;
	Step(grazer, &host, 3);
	EXPECT_EQ(grazer.State(Mode).i, Hash("graze")) << "below the range clamps to its bottom";
}

TEST(GrazerStep, AnInputOnACutFallsInTheBandItStarts)
{
	machine_fixture f = Grazer();
	ASSERT_TRUE(f.binding);
	host_object host;
	instance grazer = f.Create(host);
	host.threat = 0.5;   // graze's bands cut at 0.5: half-open, so 0.5 is in 0.5..1
	Step(grazer, &host, 1);
	EXPECT_EQ(grazer.State(Mode).i, Hash("flee"));
	host.threat = 0.3;   // flee's cut
	Step(grazer, &host, 2);
	EXPECT_EQ(grazer.State(Mode).i, Hash("flee"));
	host.threat = std::nextafter(0.3f, 0.f);
	Step(grazer, &host, 3);
	EXPECT_EQ(grazer.State(Mode).i, Hash("graze"));
}

TEST(GrazerStep, TheInstanceKeepsItsBindingAndMachineForStaleness)
{
	machine_fixture f = Grazer();
	ASSERT_TRUE(f.binding);
	host_object host;
	instance grazer = f.Create(host);
	EXPECT_EQ(&grazer.binding(), f.binding.get());
	EXPECT_EQ(&grazer.binding().machine(), f.machine.get());
	EXPECT_FALSE(grazer.binding().machine().Stale(f.store));
	csv_rows reload;
	reload.table     = 1;
	reload.owner     = 1;
	reload.label     = "rows.csv";
	reload.condition = &Conditions;
	std::string const changed = Replace(kGrazerBrainRows, "fight,  0..1,   0.3..1, ,         fight",
		"fight,  0..1,   0.3..1, ,         flee");
	reload.text = changed;
	ASSERT_EQ(RefusalOf(LoadCsv(f.store, reload)), "(accepted)");
	EXPECT_TRUE(grazer.binding().machine().Stale(f.store));
	// The running binding still steps the versions it was proved against.
	host.threat   = 0.9;
	host.cornered = true;
	Step(grazer, &host, 1);
	Step(grazer, &host, 2);
	EXPECT_EQ(grazer.State(Mode).i, Hash("fight"));
}

namespace
{

// What the watched clauses of a counted run answered, by clause_outcome.
using outcome_counts = std::array<int, 5>;

struct watched_clause
{
	trigger list;
	size_t  clause;
};

// Steps subject once per i in [0, steps) under an AllocationCount, after
// vary(i) has set the host, tallying what the watched clauses answered;
// returns what the loop allocated. Nothing in the loop allocates but Step.
template<typename Vary>
size_t CountedSteps(instance& subject, host_object const& host, int steps, Vary const& vary,
	std::span<watched_clause const> watched, outcome_counts& seen)
{
	AllocationCount count;
	for (int i = 0; i < steps; ++i)
	{
		vary(i);
		Step(subject, &host, uint64_t(i));
		for (watched_clause const& w : watched)
		{
			++seen[size_t(subject.Answer(w.list, w.clause).outcome)];
		}
	}
	return count.Count();
}

constexpr char const* kNoCounting = "a sanitizer owns operator new in this build, so nothing counts allocations";

}

TEST(GrazerStep, AStepAllocatesNothing)
{
	if (!kCountsAllocations)
	{
		GTEST_SKIP() << kNoCounting;
	}
	machine_fixture f = Grazer();
	ASSERT_TRUE(f.binding);
	host_object host;
	host.terse      = true;
	instance grazer = f.Create(host);
	// Five steps round: a corner check that raises, a NaN threat, a threat
	// getter that fails, then a high threat and a low one that answer.
	auto vary = [&](int i)
	{
		host.broken   = i % 5 == 0;
		host.unwired  = i % 5 == 2;
		host.threat   = i % 5 == 1 ? std::numeric_limits<double>::quiet_NaN() : (i % 5 == 4 ? 0.1 : 0.9);
		host.cornered = (i / 5) % 2 == 0;
	};
	watched_clause const watched[] = {{trigger::Step, 1}, {trigger::Entry, 0}};
	outcome_counts       seen{};
	EXPECT_EQ(CountedSteps(grazer, host, 40, vary, watched, seen), 0u);
	EXPECT_EQ(seen[size_t(clause_outcome::Failed)], 16) << "the raise and the NaN, 8 of each";
	EXPECT_EQ(seen[size_t(clause_outcome::SourceHeld)], 8) << "the failed getter";
	EXPECT_GT(seen[size_t(clause_outcome::Answered)], 20) << "brain and entry";
	// The counter counts: creating an instance sizes its buffers.
	AllocationCount count;
	instance        another = CreateInstance(*f.binding, 8, 1, &host);
	EXPECT_GT(count.Count(), 0u);
}

// ---- Bind --------------------------------------------------------------------------

namespace
{

struct getter_log
{
	std::vector<std::string> resolved;
	int                      released = 0;
};

void ReleaseLogged(void* data)
{
	++static_cast<getter_log*>(data)->released;
}

// Resolves through HostGetters, logging each name and giving each getter a
// release that counts; refuses the input named refuse.
host_binding LoggedGetters(getter_log& log, std::string_view refuse = {})
{
	host_binding host;
	host.resolve = [&log, refuse, inner = HostGetters()](std::string_view name) -> tl::expected<input_getter, ErrorContext>
	{
		log.resolved.emplace_back(name);
		if (name == refuse)
		{
			return tl::make_unexpected(ErrorContext("the host has no wire for it"));
		}
		auto getter = inner.resolve(name);
		if (getter)
		{
			getter->data    = &log;
			getter->release = &ReleaseLogged;
		}
		return getter;
	};
	return host;
}

}

TEST(MachineBind, ResolvesEveryInputOnceInDeclarationOrder)
{
	machine_fixture f = Grazer();
	ASSERT_TRUE(f.machine);
	getter_log log;
	{
		auto bound = Bind(f.machine, LoggedGetters(log));
		ASSERT_TRUE(bound.has_value()) << RefusalOf(bound);
		EXPECT_EQ(&(*bound)->machine(), f.machine.get());
		EXPECT_EQ(log.resolved, (std::vector<std::string>{"energy", "threat"}));
		host_object host;
		instance grazer = CreateInstance(**bound, 1, 1, &host);
		Step(grazer, &host, 1);
		EXPECT_EQ(log.resolved.size(), 2u) << "a step resolves no name";
		EXPECT_EQ(log.released, 0);
	}
	EXPECT_EQ(log.released, 2) << "each getter released once, with the last holder of the binding";
}

TEST(MachineBind, AnInputTheHostCannotSupplyIsRefusedAndTheGettersReleased)
{
	machine_fixture f = Grazer();
	ASSERT_TRUE(f.machine);
	getter_log log;
	auto bound = Bind(f.machine, LoggedGetters(log, "threat"));
	std::string const error = RefusalOf(bound);
	EXPECT_TRUE(Mentions(error, "machine 'grazer' input 'threat'")) << error;
	EXPECT_TRUE(Mentions(error, "the host has no wire for it")) << error;
	EXPECT_EQ(log.released, 1) << "energy's getter, returned before the refusal";
}

TEST(MachineBind, AGetterWithNoReadIsRefused)
{
	machine_fixture f = Grazer();
	ASSERT_TRUE(f.machine);
	int        released = 0;
	host_binding host;
	host.resolve = [&released](std::string_view) -> tl::expected<input_getter, ErrorContext>
	{
		return input_getter{nullptr, &released, [](void* d) { ++*static_cast<int*>(d); }};
	};
	std::string const error = RefusalOf(Bind(f.machine, std::move(host)));
	EXPECT_TRUE(Mentions(error, "machine 'grazer' input 'energy'")) << error;
	EXPECT_TRUE(Mentions(error, "the host returned a getter with no read function")) << error;
	EXPECT_EQ(released, 1) << "a refused getter is adopted too";
}

TEST(MachineBind, AMissingResolverOrMachineIsRefused)
{
	machine_fixture f = Grazer();
	ASSERT_TRUE(f.machine);
	std::string const noResolver = RefusalOf(Bind(f.machine, host_binding{}));
	EXPECT_TRUE(Mentions(noResolver, "machine 'grazer' has inputs, and the host binding has no resolve")) << noResolver;
	std::string const noMachine = RefusalOf(Bind(counted_ptr<Machine const>(), HostGetters()));
	EXPECT_TRUE(Mentions(noMachine, "Bind was given no machine")) << noMachine;
}

// ---- Step: the next vector, clause order, holds -------------------------------------

namespace
{

// flip moves mode; show reads mode as the step has written it. hold is a
// column no clause writes.
constexpr std::string_view kFlip = R"(
[table flip
  (key mode hash (domain rows))
  (column next hash)
  (order mode)]
[table show
  (key mode hash (domain rows))
  (column label hash)
  (order mode)]
[machine flipper
  (state mode hash (members a b) (initial a))
  (state label hash (members x y z) (initial z))
  (state hold int (initial 42))]
list,from,where,given,set,emit,overrides
step,flip,mode=state.mode,,mode=next,,
step,show,mode=state.mode,,label=label,,
)";

constexpr std::string_view kFlipRows = "mode,next\na,b\nb,a\n";
constexpr std::string_view kShowRows = "mode,label\na,x\nb,y\n";

}

TEST(MachineStep, AChainSettlesInOneStepBecauseStateIsReadFromNext)
{
	machine_fixture f(kFlip, {kFlipRows, kShowRows});
	ASSERT_TRUE(f.binding);
	host_object host;
	instance flipper = f.Create(host);
	EXPECT_EQ(flipper.State(0).i, Hash("a"));
	EXPECT_EQ(flipper.State(1).i, Hash("z")) << "no create or entry clause wrote it";
	Step(flipper, &host, 1);
	EXPECT_EQ(flipper.State(0).i, Hash("b"));
	EXPECT_EQ(flipper.State(1).i, Hash("y")) << "show read the mode flip wrote this step, not the current a";
	Step(flipper, &host, 2);
	EXPECT_EQ(flipper.State(0).i, Hash("a"));
	EXPECT_EQ(flipper.State(1).i, Hash("x"));
}

TEST(MachineStep, AColumnNoClauseWritesHolds)
{
	machine_fixture f(kFlip, {kFlipRows, kShowRows});
	ASSERT_TRUE(f.binding);
	host_object host;
	instance flipper = f.Create(host);
	for (int i = 0; i < 5; ++i)
	{
		Step(flipper, &host, uint64_t(i));
		EXPECT_EQ(flipper.State(2).i, 42);
	}
}

TEST(MachineStep, OfTwoClausesSettingOneColumnTheLaterWins)
{
	std::string const both = Replace(kFlip, "step,show,mode=state.mode,,label=label,,\n",
		"step,show,mode=state.mode,,label=label,,\nstep,flip,mode=state.mode,,mode=next,,\n");
	machine_fixture f(both, {kFlipRows, kShowRows});
	ASSERT_TRUE(f.binding);
	host_object host;
	instance flipper = f.Create(host);
	Step(flipper, &host, 1);
	EXPECT_EQ(flipper.State(0).i, Hash("a")) << "the second flip read b from next and wrote a over the first's b";
	EXPECT_EQ(flipper.State(1).i, Hash("y")) << "show ran between them";
}

TEST(MachineStep, ALiteralSourceIsItsValueEveryStep)
{
	std::string const literal = Replace(kFlip, "step,show,mode=state.mode,", "step,show,mode=(literal b),");
	machine_fixture f(literal, {kFlipRows, kShowRows});
	ASSERT_TRUE(f.binding);
	host_object host;
	instance flipper = f.Create(host);
	for (int i = 0; i < 3; ++i)
	{
		Step(flipper, &host, uint64_t(i));
		EXPECT_EQ(flipper.State(1).i, Hash("y")) << "show always reads b";
	}
}

TEST(MachineBind, AMachineWithNoInputsNeedsNoResolver)
{
	machine_fixture f(kFlip, {kFlipRows, kShowRows});
	ASSERT_TRUE(f.machine);
	auto bound = Bind(f.machine, host_binding{});
	ASSERT_TRUE(bound.has_value()) << RefusalOf(bound);
	host_object host;
	instance flipper = CreateInstance(**bound, 1, 1, &host);
	Step(flipper, &host, 1);
	EXPECT_EQ(flipper.State(0).i, Hash("b"));
}

TEST(MachineStep, SeveralMachinesOnOneObjectAreSeveralInstances)
{
	machine_fixture grazerMachine = Grazer();
	machine_fixture flipMachine(kFlip, {kFlipRows, kShowRows});
	ASSERT_TRUE(grazerMachine.binding);
	ASSERT_TRUE(flipMachine.binding);
	host_object host;
	instance grazer  = grazerMachine.Create(host);
	instance flipper = flipMachine.Create(host);
	host.threat = 0.9;
	Step(grazer, &host, 1);
	Step(flipper, &host, 1);
	EXPECT_EQ(grazer.State(Mode).i, Hash("flee"));
	EXPECT_EQ(flipper.State(0).i, Hash("b"));
	Step(flipper, &host, 2);
	EXPECT_EQ(flipper.State(0).i, Hash("a"));
	EXPECT_EQ(grazer.State(Mode).i, Hash("flee")) << "a region steps alone";
}

TEST(MachineStep, AnEarlierClausesAnswerIsReadThisStep)
{
	constexpr std::string_view chain = R"(
[table mood_of
  (key mode hash (domain rows))
  (column mood hash)
  (order mode)]
[table react
  (key mood hash (domain rows))
  (column next hash)
  (order mood)]
[machine chain (state mode hash (members calm alert) (initial calm))]
list,from,where,given,set,emit,overrides
step,mood_of,mode=state.mode,,,,
step,react,mood=clause.0.mood,,mode=next,,
)";
	machine_fixture f(chain, {"mode,mood\ncalm,happy\nalert,tense\n", "mood,next\nhappy,alert\ntense,calm\n"});
	ASSERT_TRUE(f.binding);
	host_object host;
	instance subject = f.Create(host);
	Step(subject, &host, 1);
	EXPECT_EQ(subject.State(0).i, Hash("alert"));
	clause_answer const mood = subject.Answer(trigger::Step, 0);
	ASSERT_EQ(mood.outcome, clause_outcome::Answered);
	EXPECT_EQ(ValueOf(f.Version(trigger::Step, 0), mood.row, "mood").i, Hash("happy"));
	Step(subject, &host, 2);
	EXPECT_EQ(subject.State(0).i, Hash("calm"));
}

namespace
{

// The spec's grazer (§4), as the front ends read it today (machine_gtest's
// GrazerTables), with more flag groups and two script conditions so the
// ranked fall-through has groups to rank and rows to fall past.
constexpr std::string_view kSpecGrazer = R"(
[table grazer_brain
  (input energy double 0..1)
  (input food_near flags)
  (key mode hash (domain rows))
  (key message hash fallthrough)
  (column energy band (input energy))
  (column near flags (input food_near))
  (column when condition)
  (column next hash)
  (order mode message)]
[table grazer_enter
  (key mode hash (domain rows))
  (order mode)
  (suffix command)]
[machine grazer
  (state mode hash (members idle walk eat panic) (initial idle))
  (input energy double 0..1)
  (input food_near flags)
  (input message hash)]
list,from,where,given,set,emit,overrides
entry,grazer_enter,mode=state.mode,,,command,
step,grazer_brain,mode=state.mode message=input.message,energy=input.energy food_near=input.food_near,mode=next,,
)";

constexpr std::string_view kSpecBrainRows = R"(# grazer_brain
mode,  message, energy,   near, when,     next
idle,      ,    0..0.3,   1,    ,         eat
idle,      ,    0..1,     2,    cornered, panic
idle,      ,    0..1,     0,    ,         walk
walk,      ,    0.5..1,   2,    ,         eat
walk,      ,    0..0.5,   3,    ,         idle
walk,      ,    0.2..0.8, 1,    panicky,  panic
walk,      ,    0..1,     0,    ,         idle
eat,       ,    0..0.5,   0,    cornered, panic
eat,       ,    0..1,     0,    ,         idle
eat,       ,    0..1,     6,    ,         walk
eat,       ,    0..1,     9,    ,         panic
idle,  scared,  0..1,     0,    ,         panic
walk,  scared,  0..1,     0,    ,         panic
eat,   scared,  0..1,     0,    ,         panic
panic,     ,    0..1,     0,    ,         panic
panic, calm,    0..1,     0,    ,         idle
panic, scared,  0..1,     0,    ,         panic
idle,  calm,    0..1,     0,    ,         idle
walk,  calm,    0..0.5,   0,    panicky,  panic
walk,  calm,    0..1,     0,    ,         walk
eat,   calm,    0..1,     0,    ,         eat
)";

constexpr std::string_view kSpecEnterRows = R"csv(# grazer_enter
mode,  command
eat,   (anim chew), "(message food ""eat"")"
panic, (anim run), "(message this ""calm"" :delay random(0.4, 1.0))"
idle,  (anim stand)
walk,  (anim walk)
)csv";

machine_fixture SpecGrazer()
{
	return machine_fixture(kSpecGrazer, {kSpecBrainRows, kSpecEnterRows});
}

}

TEST(MachineStep, AValueNoRowKeysOnFallsThroughToTheBlankRun)
{
	machine_fixture f = SpecGrazer();
	ASSERT_TRUE(f.binding);
	TableVersion const& brain = f.Version(trigger::Step, 0);
	host_object host;
	instance grazer = f.Create(host);
	StepOn(grazer, host, 1, "scared");
	EXPECT_EQ(grazer.State(0).i, Hash("panic"));
	EXPECT_FALSE(grazer.Answer(trigger::Step, 0).fellThrough) << "idle has a scared run";
	StepOn(grazer, host, 2, "sneeze");   // no run keys on sneeze
	clause_answer const fell = grazer.Answer(trigger::Step, 0);
	ASSERT_EQ(fell.outcome, clause_outcome::Answered) << "panic's blank run answered";
	EXPECT_TRUE(fell.fellThrough);
	EXPECT_EQ(ValueOf(brain, fell.row, "message").i, 0);
	EXPECT_EQ(grazer.State(0).i, Hash("panic"));
	Step(grazer, &host, 3);   // no event: 0, the blank run directly
	EXPECT_EQ(grazer.Answer(trigger::Step, 0).row, fell.row);
	EXPECT_FALSE(grazer.Answer(trigger::Step, 0).fellThrough);
	StepOn(grazer, host, 4, "calm");
	EXPECT_EQ(grazer.State(0).i, Hash("idle"));
	EXPECT_FALSE(grazer.Answer(trigger::Step, 0).fellThrough);
}

TEST(MachineStep, TheFlagsRankTheGroupsAsPickBestRanksRows)
{
	machine_fixture f = SpecGrazer();
	ASSERT_TRUE(f.binding);
	host_object host;
	host.energy  = 0.1;
	instance grazer = f.Create(host);
	host.food_near = 3;   // idle has groups 1 and 2: both full matches, 2 & 3 is larger, so cornered() first
	host.cornered  = true;
	Step(grazer, &host, 1);
	EXPECT_EQ(grazer.State(0).i, Hash("panic"));
	grazer = f.Create(host);
	host.cornered = false;   // falls past it to group 1, whose band holds at 0.1
	Step(grazer, &host, 2);
	EXPECT_EQ(grazer.State(0).i, Hash("eat"));
	grazer = f.Create(host);
	host.energy = 0.5;   // group 1's band misses: down to the default
	Step(grazer, &host, 3);
	EXPECT_EQ(grazer.State(0).i, Hash("walk"));
}

TEST(MachineStep, AClauseReadingAHeldClauseHolds)
{
	constexpr std::string_view chain = R"(
[table mood_of
  (input threat double 0..1)
  (key mode hash (domain rows))
  (column threat band (input threat))
  (column mood hash)
  (order mode)]
[table react
  (key mood hash (domain rows))
  (column next hash)
  (order mood)]
[machine chain
  (state mode hash (members calm alert) (initial calm))
  (input threat double 0..1)]
list,from,where,given,set,emit,overrides
step,mood_of,mode=state.mode,threat=input.threat,,,
step,react,mood=clause.0.mood,,mode=next,,
)";
	machine_fixture f(chain, {"mode,threat,mood\ncalm,0..1,happy\nalert,0..1,tense\n", "mood,next\nhappy,alert\ntense,calm\n"});
	ASSERT_TRUE(f.binding);
	host_object host;
	instance subject = f.Create(host);
	host.threat = std::numeric_limits<double>::quiet_NaN();
	Step(subject, &host, 1);
	EXPECT_EQ(subject.Answer(trigger::Step, 0).outcome, clause_outcome::Failed);
	EXPECT_EQ(subject.Answer(trigger::Step, 1).outcome, clause_outcome::SourceHeld);
	EXPECT_EQ(subject.State(0).i, Hash("calm"));
}

namespace
{

// Creation and entry: start sets mode on creation; go moves it on a step;
// enter answers for the state entered, writing data (an int), never a state.
constexpr std::string_view kEnter = R"(
[table start
  (key mode hash (domain rows))
  (column next hash)
  (order mode)]
[table go
  (key mode hash (domain rows))
  (column next hash)
  (order mode)]
[table enter
  (key mode hash (domain rows))
  (column next hash)
  (column count int)
  (order mode)]
[machine door
  (state mode hash (members a b c d) (initial a))
  (state entered int (initial 0))]
list,from,where,given,set,emit,overrides
create,start,mode=state.mode,,mode=next,,
entry,enter,mode=state.mode,,entered=count,,
step,go,mode=state.mode,,mode=next,,
)";

constexpr std::string_view kStartRows = "mode,next\na,d\nb,b\nc,c\nd,d\n";
constexpr std::string_view kGoRows    = "mode,next\na,b\nb,b\nc,c\nd,a\n";
constexpr std::string_view kEnterRows = "mode,next,count\na,a,1\nb,c,2\nc,c,3\nd,d,4\n";

}

TEST(MachineStep, CreationRunsCreateThenEntryAgainstTheInitialState)
{
	machine_fixture f(kEnter, {kStartRows, kGoRows, kEnterRows});
	ASSERT_TRUE(f.binding);
	host_object host;
	instance door = f.Create(host);
	EXPECT_EQ(door.State(0).i, Hash("d")) << "create moved a to d";
	EXPECT_EQ(door.State(1).i, 4) << "entry ran after create, and read the d it wrote";
	EXPECT_EQ(door.Answer(trigger::Create, 0).outcome, clause_outcome::Answered);
	EXPECT_EQ(door.Answer(trigger::Entry, 0).outcome, clause_outcome::Answered);
}

TEST(MachineStep, AnEntryClauseMayNotSetAHashState)
{
	// Entry runs once per step, after the step list: a state an entry write
	// moved to would get no entry of its own, so the write is refused.
	std::string const transition = Replace(kEnter, "entry,enter,mode=state.mode,,entered=count,,", "entry,enter,mode=state.mode,,mode=next entered=count,,");
	std::string const error = RefusalOf(ReadDocument(transition, "machine.tabular"));
	EXPECT_TRUE(Mentions(error, "machine 'door' entry clause 0 (table 'enter')")) << error;
	EXPECT_TRUE(Mentions(error, "sets hash state column 'mode'")) << error;
	EXPECT_TRUE(Mentions(error, "belongs in the step list")) << error;
}

namespace
{

// Entry counts its own runs: tally maps n to n + 1, so entered rises by one
// for each run of the entry list.
constexpr std::string_view kTally = R"(
[table go
  (key mode hash (domain rows))
  (column next hash)
  (order mode)]
[table tally
  (key n int (domain rows))
  (column next int)
  (order n)]
[machine counter
  (state mode hash (members a b c) (initial a))
  (state entered int (initial 0))]
list,from,where,given,set,emit,overrides
entry,tally,n=state.entered,,entered=next,,
step,go,mode=state.mode,,mode=next,,
)";

constexpr std::string_view kTallyGoRows = "mode,next\na,b\nb,c\nc,c\n";
constexpr std::string_view kTallyRows   = "n,next\n0,1\n1,2\n2,3\n3,4\n4,5\n5,6\n";

}

TEST(MachineStep, EntryRunsOncePerStepThatChangesAHashState)
{
	machine_fixture f(kTally, {kTallyGoRows, kTallyRows});
	ASSERT_TRUE(f.binding);
	host_object host;
	instance counter = f.Create(host);
	EXPECT_EQ(counter.State(1).i, 1) << "creation entered a, once";
	Step(counter, &host, 1);   // a -> b
	EXPECT_EQ(counter.State(0).i, Hash("b"));
	EXPECT_EQ(counter.State(1).i, 2) << "entered b: entry ran once, on top of the step";
	Step(counter, &host, 2);   // b -> c
	EXPECT_EQ(counter.State(1).i, 3);
	Step(counter, &host, 3);   // c -> c: nothing entered
	EXPECT_EQ(counter.State(1).i, 3);
	EXPECT_EQ(counter.Answer(trigger::Entry, 0).outcome, clause_outcome::NotRun);
}

// ---- Parity with the interpreted picks -----------------------------------------------

namespace
{

// The oracle's answer, read as the outcome a step must have.
template<typename Picked>
void ExpectSameAnswer(tl::expected<std::optional<size_t>, ErrorContext> const& oracle, clause_answer got,
	Picked const& where)
{
	if (!oracle)
	{
		EXPECT_EQ(got.outcome, clause_outcome::Failed) << where() << oracle.error().format();
		return;
	}
	if (!*oracle)
	{
		EXPECT_EQ(got.outcome, clause_outcome::NoRun) << where();
		return;
	}
	EXPECT_EQ(got.outcome, clause_outcome::Answered) << where();
	EXPECT_EQ(got.row, **oracle) << where();
}

// Inputs a step must treat as a pick does: cuts, the range's ends and beyond,
// a float just under a cut, NaN and the infinities, and anything in between.
double PickEnergy(std::mt19937_64& rng)
{
	static double const edges[] = {-1.0, 0.0, 0.1, 0.2, 0.3, 0.5, 0.8, 1.0, 2.0, 1e300,
		double(std::nextafter(1.f, 0.f)), double(std::nextafter(0.3f, 0.f)), double(std::nextafter(0.5f, 1.f)),
		std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
		-std::numeric_limits<double>::infinity()};
	std::uniform_int_distribution<size_t> pick(0, std::size(edges) + 3);
	size_t const i = pick(rng);
	if (i < std::size(edges))
	{
		return edges[i];
	}
	return std::uniform_real_distribution<double>(-0.2, 1.2)(rng);
}

}

TEST(StepParity, TheSpecGrazerMatchesPickBestStepByStep)
{
	machine_fixture f = SpecGrazer();
	ASSERT_TRUE(f.binding);
	TableVersion const& brain = f.Version(trigger::Step, 0);
	TableVersion const& enter = f.Version(trigger::Entry, 0);
	std::mt19937_64 rng(0x7ab1a);
	int64_t const   messages[] = {0, Hash("scared"), Hash("calm"), Hash("sneeze"), 0};
	// 5 against eat's groups 6 and 9 matches neither fully, and the larger R & F
	// (6's 4) outranks the larger R (9).
	int64_t const   flags[]    = {0, 1, 2, 3, 4, 5, 6, 13, 0xFFFFFFFF, int64_t(1) << 32};
	host_object host;
	instance grazer = f.Create(host);
	int answered = 0;
	int held     = 0;
	int entered  = 0;
	int fell     = 0;
	for (int i = 0; i < 4000; ++i)
	{
		host.message   = messages[rng() % std::size(messages)];
		host.energy    = PickEnergy(rng);
		host.food_near = flags[rng() % std::size(flags)];
		host.cornered  = rng() % 2 == 0;
		host.broken    = rng() % 25 == 0;
		cell const before = grazer.State(0);
		// The fall-through is the table's: PickBest retries a value with no run
		// on the blank run itself. Whether the value has a run of its own is
		// read off the version, for fellThrough.
		std::vector<cell> const keys = {before, cell::Integer(host.message)};
		auto const run = brain.FindOrdered(keys);
		ASSERT_TRUE(run.has_value()) << RefusalOf(run);
		bool const falls   = host.message != 0 && run->first == run->last;
		std::vector<cell> const inputs = {cell::Real(host.energy), cell::Integer(host.food_near)};
		auto const oracle = PickBest(brain, pick_query{keys, inputs, &host});
		Step(grazer, &host, uint64_t(i));
		auto where = [&]() { return "step " + std::to_string(i) + " "; };
		clause_answer const got = grazer.Answer(trigger::Step, 0);
		ExpectSameAnswer(oracle, got, where);
		cell const wanted = oracle && *oracle ? ValueOf(brain, **oracle, "next") : before;
		EXPECT_EQ(grazer.State(0).i, wanted.i) << where();
		(got.outcome == clause_outcome::Answered ? answered : held) += 1;
		// Both sides fall through, or neither would answer a type with no run.
		fell += falls && got.outcome == clause_outcome::Answered ? 1 : 0;
		if (got.outcome == clause_outcome::Answered)
		{
			EXPECT_EQ(got.fellThrough, falls) << where();
		}
		if (wanted.i != before.i)
		{
			++entered;
			std::vector<cell> const enterKeys = {wanted};
			ExpectSameAnswer(PickBest(enter, pick_query{enterKeys, {}, &host}), grazer.Answer(trigger::Entry, 0), where);
		}
		else
		{
			EXPECT_EQ(grazer.Answer(trigger::Entry, 0).outcome, clause_outcome::NotRun) << where();
		}
		if (::testing::Test::HasFailure())
		{
			break;
		}
	}
	EXPECT_GT(answered, 1000);
	EXPECT_GT(held, 500);
	EXPECT_GT(entered, 300);
	EXPECT_GT(fell, 300) << "sneeze, which no run keys on, answered from the blank run";
}

TEST(StepParity, TheWorkedGrazerMatchesPickBestStepByStep)
{
	machine_fixture f = Grazer();
	ASSERT_TRUE(f.binding);
	TableVersion const& settle = f.Version(trigger::Step, 0);
	TableVersion const& brain  = f.Version(trigger::Step, 1);
	std::mt19937_64 rng(0x5eed);
	host_object host;
	instance grazer = f.Create(host);
	for (int i = 0; i < 4000; ++i)
	{
		host.energy   = PickEnergy(rng);
		host.threat   = PickEnergy(rng);
		host.cornered = rng() % 2 == 0;
		host.broken   = rng() % 25 == 0;
		cell const before = grazer.State(Mode);
		std::vector<cell> const keys   = {before};
		std::vector<cell> const inputs = {cell::Real(host.energy), cell::Real(host.threat)};
		auto const settled = PickBest(settle, pick_query{keys, {}, &host});
		auto const oracle  = PickBest(brain, pick_query{keys, inputs, &host});
		Step(grazer, &host, uint64_t(i));
		auto where = [&]() { return "step " + std::to_string(i) + " "; };
		ExpectSameAnswer(settled, grazer.Answer(trigger::Step, 0), where);
		ExpectSameAnswer(oracle, grazer.Answer(trigger::Step, 1), where);
		cell const wanted = oracle && *oracle ? ValueOf(brain, **oracle, "next") : before;
		EXPECT_EQ(grazer.State(Mode).i, wanted.i) << where();
		if (::testing::Test::HasFailure())
		{
			break;
		}
	}
}

namespace
{

// Ranked groups where each term of the rank decides: R & F, a full match over
// a larger R & F, and R itself between two groups equal in both (3, 5 and 7
// against F = 1, or 5 and 7 against F = 4). b..f each send it back to a.
constexpr std::string_view kRanker = R"(
[table ranked
  (input food_near flags)
  (key mode hash (domain rows))
  (column near flags (input food_near))
  (column when condition)
  (column next hash)
  (order mode)]
[machine ranker
  (state mode hash (members a b c d e f) (initial a))
  (input food_near flags)]
list,from,where,given,set,emit,overrides
step,ranked,mode=state.mode,food_near=input.food_near,mode=next,,
)";

constexpr std::string_view kRankerRows = R"(mode, near, when, next
a, 3, ,         b
a, 5, ,         c
a, 2, cornered, d
a, 7, ,         e
a, 0, ,         f
b, 0, ,         a
c, 0, ,         a
d, 0, ,         a
e, 0, ,         a
f, 0, ,         a
)";

}

TEST(StepParity, EachTermOfTheRankDecidesAsInPickBest)
{
	machine_fixture f(kRanker, {kRankerRows});
	ASSERT_TRUE(f.binding);
	TableVersion const& ranked = f.Version(trigger::Step, 0);
	struct ranked_case
	{
		int64_t          F;
		bool             cornered;
		std::string_view next;
		char const*      why;
	};
	ranked_case const cases[] = {
		{1, false, "e", "3, 5 and 7 each hit 1 and none is full: the larger R, 7"},
		{4, true,  "e", "5 and 7 each hit 4: the larger R, 7"},
		{6, true,  "d", "2 is all in 6 and beats 7's larger hit of 6"},
		{6, false, "e", "2's condition fails: 7 hits 6, more than 5's 4 or 3's 2"},
		{3, false, "b", "3 and 2 are full; 3 hits more"},
		{8, false, "f", "nothing matches: the default"},
	};
	host_object host;
	for (ranked_case const& c : cases)
	{
		host.food_near  = c.F;
		host.cornered   = c.cornered;
		instance ranker = f.Create(host);
		Step(ranker, &host, 1);
		EXPECT_EQ(ranker.State(0).i, Hash(c.next)) << "F = " << c.F << ": " << c.why;
	}
	// And every flag set against the oracle, from a.
	for (int64_t F = 0; F < 16; ++F)
	{
		for (bool cornered : {false, true})
		{
			host.food_near  = F;
			host.cornered   = cornered;
			instance ranker = f.Create(host);
			std::vector<cell> const keys   = {cell::Integer(Hash("a"))};
			std::vector<cell> const inputs = {cell::Integer(F)};
			auto const oracle = PickBest(ranked, pick_query{keys, inputs, &host});
			Step(ranker, &host, 1);
			ExpectSameAnswer(oracle, ranker.Answer(trigger::Step, 0),
				[&]() { return "F = " + std::to_string(F) + (cornered ? " cornered " : " "); });
		}
	}
}

namespace
{

// Two signed int keys: a walker steps through every (x, y) of its domain, each
// row naming the next tuple of a cycle that strides across the sorted order,
// so a key that ties sends the search to the next key, and negative keys sort
// below zero as int64 does.
constexpr std::string_view kWalker = R"(
[table walk
  (key x int (domain rows))
  (key y int (domain rows))
  (column nx int)
  (column ny int)
  (order x y)]
[machine walker
  (state x int (initial -7))
  (state y int (initial -4))]
list,from,where,given,set,emit,overrides
step,walk,x=state.x y=state.y,,x=nx y=ny,,
)";

constexpr int64_t kWalkX[] = {-7, -2, -1, 0, 3};
constexpr int64_t kWalkY[] = {-4, -1, 0, 2};
constexpr size_t  kWalkTuples = std::size(kWalkX) * std::size(kWalkY);

// The cycle's i-th tuple: a stride of 7 through the 20, so its first is (-7, -4).
std::pair<int64_t, int64_t> WalkTuple(size_t i)
{
	size_t const k = (i * 7) % kWalkTuples;
	return {kWalkX[k / std::size(kWalkY)], kWalkY[k % std::size(kWalkY)]};
}

std::string WalkRows()
{
	std::string rows = "x,y,nx,ny\n";
	for (size_t i = 0; i < kWalkTuples; ++i)
	{
		auto const [x, y]   = WalkTuple(i);
		auto const [nx, ny] = WalkTuple(i + 1);
		rows += std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(nx) + "," + std::to_string(ny) + "\n";
	}
	return rows;
}

}

TEST(StepParity, SignedMultiKeyTuplesAreFoundInInt64Order)
{
	std::string const rows = WalkRows();
	machine_fixture   f(kWalker, {rows});
	ASSERT_TRUE(f.binding);
	TableVersion const& walk = f.Version(trigger::Step, 0);
	host_object host;
	instance walker = f.Create(host);
	for (size_t i = 0; i < 2 * kWalkTuples; ++i)
	{
		std::vector<cell> const keys = {walker.State(0), walker.State(1)};
		auto const oracle = PickBest(walk, pick_query{keys, {}, &host});
		Step(walker, &host, uint64_t(i));
		auto const [x, y] = WalkTuple(i + 1);
		auto where = [&]() { return "step " + std::to_string(i) + " "; };
		ExpectSameAnswer(oracle, walker.Answer(trigger::Step, 0), where);
		EXPECT_EQ(walker.State(0).i, x) << where();
		EXPECT_EQ(walker.State(1).i, y) << where();
		if (::testing::Test::HasFailure())
		{
			break;
		}
	}
}

namespace
{

// The definition's chain: an or clause answers flags, a best clause keyed by
// them moves mode, and a weighted clause keyed by the moved mode draws a mood.
constexpr std::string_view kPhaseChain = R"(
[table phase
  (input t double 0..1)
  (key mode hash)
  (column t band (input t))
  (column when condition)
  (column bits flags)
  (pick or bits)
  (order mode)]
[table lookup
  (key bits flags (domain rows))
  (column next hash)
  (order bits)]
[table wander
  (key mode hash)
  (column w double)
  (column mood hash)
  (pick weighted w)
  (order mode)]
[machine chain
  (state mode hash (members idle walk) (initial idle))
  (state mood hash (members calm edgy) (initial calm))
  (input t double 0..1)]
list,from,where,given,set,emit,overrides
step,phase,mode=state.mode,t=input.t,,,
step,lookup,bits=clause.0.bits,,mode=next,,
step,wander,mode=state.mode,,mood=mood,,
)";

// idle answers 1, 2 or 3, and 4 more when cornered; walk 1 or 9. lookup has no
// run for 6 or 7, so it holds there.
constexpr std::string_view kPhaseRows = R"(mode, t, when, bits
idle, 0..0.5,  ,         1
idle, 0.25..1, ,         2
idle, 0..1,    cornered, 4
walk, 0..1,    ,         1
walk, 0.5..1,  ,         8
)";
constexpr std::string_view kLookupRows = "bits,next\n0,idle\n1,walk\n2,idle\n3,walk\n5,walk\n9,idle\n";
constexpr std::string_view kWanderRows = "mode,w,mood\nidle,1,calm\nidle,3,edgy\nwalk,1,calm\nwalk,0.5,edgy\n";

}

TEST(StepParity, ThePhaseChainMatchesPickOrPickBestAndPickWeighted)
{
	machine_fixture f(kPhaseChain, {kPhaseRows, kLookupRows, kWanderRows});
	ASSERT_TRUE(f.binding);
	TableVersion const& phase  = f.Version(trigger::Step, 0);
	TableVersion const& lookup = f.Version(trigger::Step, 1);
	TableVersion const& wander = f.Version(trigger::Step, 2);
	uint64_t const      site   = DrawSite("chain", trigger::Step, 2);
	std::mt19937_64 rng(0xc4a1);
	host_object host;
	instance subject = f.Create(host);
	int noRun = 0;
	int edgy  = 0;
	for (int i = 0; i < 4000; ++i)
	{
		host.t        = PickEnergy(rng);
		host.cornered = rng() % 2 == 0;
		host.broken   = rng() % 25 == 0;
		uint64_t const seed = rng();
		cell const mode = subject.State(0);
		cell const mood = subject.State(1);
		std::vector<cell> const keys   = {mode};
		std::vector<cell> const inputs = {cell::Real(host.t)};
		auto const bits = PickOr(phase, pick_query{keys, inputs, &host});
		Step(subject, &host, seed);
		auto where = [&]() { return "step " + std::to_string(i) + " "; };

		clause_answer const p = subject.Answer(trigger::Step, 0);
		cell moved = mode;
		if (!bits)
		{
			EXPECT_EQ(p.outcome, clause_outcome::Failed) << where();
			EXPECT_EQ(subject.Answer(trigger::Step, 1).outcome, clause_outcome::SourceHeld) << where();
		}
		else
		{
			EXPECT_EQ(p.outcome, clause_outcome::Answered) << where();
			EXPECT_EQ(p.bits, *bits) << where();
			std::vector<cell> const lookupKeys = {cell::Integer(*bits)};
			auto const next = PickBest(lookup, pick_query{lookupKeys, {}, &host});
			ExpectSameAnswer(next, subject.Answer(trigger::Step, 1), where);
			if (next && *next)
			{
				moved = ValueOf(lookup, **next, "next");
			}
			noRun += next && !*next ? 1 : 0;
		}
		EXPECT_EQ(subject.State(0).i, moved.i) << where();

		std::vector<cell> const wanderKeys = {moved};
		auto const drawn = PickWeighted(wander, pick_query{wanderKeys, {}, &host}, seed, site);
		ExpectSameAnswer(drawn, subject.Answer(trigger::Step, 2), where);
		cell const wantedMood = drawn && *drawn ? ValueOf(wander, **drawn, "mood") : mood;
		EXPECT_EQ(subject.State(1).i, wantedMood.i) << where();
		edgy += wantedMood.i == Hash("edgy") ? 1 : 0;
		if (::testing::Test::HasFailure())
		{
			break;
		}
	}
	EXPECT_GT(noRun, 100) << "lookup held for 6 and 7";
	EXPECT_GT(edgy, 1000);
	EXPECT_LT(edgy, 3500);
}

TEST(StepParity, ADrawSiteSaltsTheMachineTheListAndTheClause)
{
	uint64_t const a = DrawSite("chain", trigger::Step, 2);
	EXPECT_NE(a, DrawSite("chained", trigger::Step, 2));
	EXPECT_NE(a, DrawSite("chain", trigger::Entry, 2));
	EXPECT_NE(a, DrawSite("chain", trigger::Step, 1));
	EXPECT_EQ(a >> 32, uint64_t(CityHash32("chain", 5)));
}

TEST(MachineStep, AnOrClauseWithNoRunAnswersZero)
{
	// walk has no phase rows: its tuple is an empty run, answering 0, and
	// lookup's 0 row sends it back to idle.
	machine_fixture f(kPhaseChain, {"mode, t, when, bits\nidle, 0..1, , 1\n", kLookupRows, kWanderRows});
	ASSERT_TRUE(f.binding);
	host_object host;
	instance subject = f.Create(host);
	Step(subject, &host, 1);
	EXPECT_EQ(subject.Answer(trigger::Step, 0).bits, 1u);
	EXPECT_EQ(subject.State(0).i, Hash("walk"));
	Step(subject, &host, 2);
	clause_answer const phase = subject.Answer(trigger::Step, 0);
	EXPECT_EQ(phase.outcome, clause_outcome::Answered);
	EXPECT_EQ(phase.bits, 0u);
	EXPECT_EQ(subject.State(0).i, Hash("idle"));
}

TEST(MachineStep, AStepThatHoldsOrDrawsAllocatesNothing)
{
	if (!kCountsAllocations)
	{
		GTEST_SKIP() << kNoCounting;
	}
	// A message no row keys on, falling through, and scripted rows that raise.
	machine_fixture spec = SpecGrazer();
	ASSERT_TRUE(spec.binding);
	host_object host;
	host.terse      = true;
	instance grazer = spec.Create(host);
	int64_t const messages[] = {0, Hash("sneeze"), Hash("scared"), Hash("calm")};
	auto varyMessage = [&](int i)
	{
		host.message   = messages[i % 4];
		host.food_near = i % 7;
		host.energy    = (i % 10) / 10.0;
		host.broken    = i % 6 == 0;
		host.cornered  = i % 3 == 0;
	};
	watched_clause const brain[] = {{trigger::Step, 0}};
	outcome_counts       seen{};
	EXPECT_EQ(CountedSteps(grazer, host, 40, varyMessage, brain, seen), 0u);
	EXPECT_EQ(seen[size_t(clause_outcome::NoRun)], 0) << "every sneeze falls through to the blank run";
	EXPECT_GT(seen[size_t(clause_outcome::Failed)], 0) << "the scripted rows raised";

	// The phase chain: an or clause whose scripted row raises, a best clause
	// keyed by its answer, and a weighted draw.
	machine_fixture chain(kPhaseChain, {kPhaseRows, kLookupRows, kWanderRows});
	ASSERT_TRUE(chain.binding);
	host       = host_object{};
	host.terse = true;
	instance subject = chain.Create(host);
	auto varyPhase = [&](int i)
	{
		host.t        = (i % 9) / 8.0;
		host.cornered = i % 2 == 0;
		host.broken   = i % 7 == 0;
	};
	watched_clause const clauses[] = {{trigger::Step, 0}, {trigger::Step, 1}, {trigger::Step, 2}};
	outcome_counts       chainSeen{};
	EXPECT_EQ(CountedSteps(subject, host, 60, varyPhase, clauses, chainSeen), 0u);
	EXPECT_GT(chainSeen[size_t(clause_outcome::Failed)], 0) << "the or clause's condition raised";
	EXPECT_GT(chainSeen[size_t(clause_outcome::Answered)], 100) << "the draws among them";
}
