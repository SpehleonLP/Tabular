#include <gtest/gtest.h>
#include "Support/allocationtestingsupport.h"
#include "tabular/instance.h"
#include "tabular/machine.h"
#include "tabular/relation.h"
#include "tabular/rows_csv.h"
#include "tabular/document.h"
#include "cityhash/src/city.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace tabular;

// Reload (P14): Rebind migrates an instance onto a new binding by name, runs
// the old exit list and then the new entry list. The reload cycle itself is
// composed by the test from what already exists: rows change, the machine is
// Stale, it is prepared again, bound, and each instance rebound. Bridges
// (R5(b)) and state functions (R6) are consequences: their tests use only
// the machine features every other test uses.

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

// An event the host dispatches, by its type (empty for none).
struct posted
{
	int64_t type = 0;
};

posted Message(std::string_view type)
{
	return posted{Hash(type)};
}

// The host object: what its getters read.
struct host_object
{
	double  energy    = 0.8;
	int64_t food_near = 0;
	double  threat    = 0.1;
	double  alarm     = 0.1;
	bool    unwired   = false;   // the threat getter cannot answer
	bool    broken    = false;   // a script condition raises
	int64_t event     = 0;       // filled by the host for one step: the event it dispatches
};

host_object const& Host(void const* self)
{
	return *static_cast<host_object const*>(self);
}

cell ReadEnergy(void*, void const* self, ErrorContext*)
{
	return cell::Real(Host(self).energy);
}

cell ReadFoodNear(void*, void const* self, ErrorContext*)
{
	return cell::Integer(Host(self).food_near);
}

cell ReadEvent(void*, void const* self, ErrorContext*)
{
	return cell::Integer(Host(self).event);
}

cell ReadThreat(void*, void const* self, ErrorContext* error_out)
{
	if (Host(self).unwired)
	{
		*error_out = ErrorContext("no target");
		return cell::Real(0);
	}
	return cell::Real(Host(self).threat);
}

// An int-typed wire of the same name: a hundredth of the host's threat.
cell ReadThreatInteger(void*, void const* self, ErrorContext*)
{
	return cell::Integer(static_cast<int64_t>(Host(self).threat * 100));
}

bool Cornered(void*, void const* self, ErrorContext* error_out)
{
	if (Host(self).broken)
	{
		*error_out = ErrorContext("dead target");
		return false;
	}
	return true;
}

tl::expected<closure, ErrorContext> Conditions(std::string_view text)
{
	if (text == "cornered")
	{
		return closure{&Cornered, nullptr, nullptr};
	}
	return tl::make_unexpected(ErrorContext("no condition '", text, "'"));
}

cell ReadAlarm(void*, void const* self, ErrorContext*)
{
	return cell::Real(Host(self).alarm);
}

// Every binding's commands land in one order, each tagged with the binding
// whose clause emitted it, so the order across an old and a new binding is
// visible.
struct reload_log
{
	std::vector<std::string> order;
	int                      reports = 0;
	std::vector<std::string> reported;   // the tag of the binding each report went to
	bool                     record  = true;   // off: a counted step allocates nothing on the host's side

	std::vector<std::string> Take()
	{
		std::vector<std::string> out = std::move(order);
		order.clear();
		return out;
	}
};

host_binding TaggedHost(reload_log& log, std::string tag, bool integerThreat = false)
{
	host_binding host;
	host.resolve = [integerThreat](std::string_view name) -> tl::expected<input_getter, ErrorContext>
	{
		if (name == "energy")
		{
			return input_getter{&ReadEnergy};
		}
		if (name == "food_near")
		{
			return input_getter{&ReadFoodNear};
		}
		if (name == "threat")
		{
			return input_getter{integerThreat ? &ReadThreatInteger : &ReadThreat};
		}
		if (name == "alarm")
		{
			return input_getter{&ReadAlarm};
		}
		if (name == "message")
		{
			return input_getter{&ReadEvent};
		}
		return tl::make_unexpected(ErrorContext("no wire named '", name, "'"));
	};
	host.report = [&log, tag](uint64_t, uint32_t, size_t, ErrorContext const&)
	{
		++log.reports;
		log.reported.push_back(tag);
	};
	return host;
}

// The log and tag each binding's commands go to, set when a world binds.
struct log_target
{
	reload_log* log = nullptr;
	std::string tag;
};

std::unordered_map<Binding const*, log_target>& Targets()
{
	static std::unordered_map<Binding const*, log_target> targets;
	return targets;
}

void Log(Binding const& binding, emitted const& result)
{
	auto const found = Targets().find(&binding);
	ASSERT_NE(found, Targets().end()) << "a binding no world made";
	if (found->second.log->record)
	{
		found->second.log->order.push_back(found->second.tag + ":" + std::string(result.command));
	}
}

// Creation, a step and a rebind with their results logged.
instance Created(Binding const& binding, uint64_t id, uint64_t seed, void const* self)
{
	instance out = CreateInstance(binding, id, seed, self);
	for (emitted const& result : out.Emitted())
	{
		Log(binding, result);
	}
	return out;
}

void Stepped(instance& subject, void const* self, uint64_t seed)
{
	for (emitted const& result : Step(subject, self, seed))
	{
		Log(subject.binding(), result);
	}
}

// A step with an event: the host fills the message input for this one step.
void Stepped(instance& subject, host_object* self, uint64_t seed, posted msg)
{
	self->event = msg.type;
	Stepped(subject, static_cast<void const*>(self), seed);
	self->event = 0;
}

// A rebind's exit commands are the old binding's, and its entry's the new one's.
void Rebound(instance& subject, Binding const& binding, void const* self, uint64_t seed)
{
	counted_ptr<Binding const> const old = CountedWrap(&subject.binding());
	for (emitted const& result : Rebind(subject, binding, self, seed))
	{
		Log(result.list == trigger::Exit ? *old : binding, result);
	}
}

schema_document Parse(std::string_view source)
{
	auto doc = ReadDocument(source, "reload.tabular");
	EXPECT_TRUE(doc.has_value()) << RefusalOf(doc);
	return doc ? std::move(*doc) : schema_document{};
}

// One store, built from the first document's tables, and the machine
// definitions read from every document since: a later document repeats the
// tables (they are not used) and changes its machine. What a host keeps
// across a reload, and nothing more.
struct reload_world
{
	reload_log                                    log;
	std::vector<std::unique_ptr<schema_document>> documents;
	relation_store                                store;

	bool                                          integerThreat = false;   // Bound's threat wire answers an int

	explicit reload_world(std::string_view source) : store(TablesOf(source)) {}

	std::vector<std::string> Take() { return log.Take(); }

	schema TablesOf(std::string_view source)
	{
		documents.push_back(std::make_unique<schema_document>(Parse(source)));
		return std::move(documents.back()->tables);
	}

	void Load(uint32_t table, std::string_view rows)
	{
		csv_rows load;
		load.table = table;
		load.owner = 1;
		load.text  = rows;
		load.label = "rows.csv";
		load.condition = &Conditions;
		EXPECT_EQ(RefusalOf(LoadCsv(store, load)), "(accepted)") << "table " << table;
	}

	// The first document's machine prepared against the store as it is now.
	tl::expected<counted_ptr<Machine const>, ErrorContext> Prepared(std::string_view source = {})
	{
		if (!source.empty())
		{
			documents.push_back(std::make_unique<schema_document>(Parse(source)));
		}
		schema_document const& doc = source.empty() ? *documents.front() : *documents.back();
		if (doc.machines().empty())
		{
			return tl::make_unexpected(ErrorContext("the document has no machine"));
		}
		return Prepare(doc.machines()[0], store);
	}

	// Prepared (from source, or the first document when empty) and bound, its
	// commands logged tagged tag.
	counted_ptr<Binding const> Bound(std::string_view tag, std::string_view source = {})
	{
		auto prepared = Prepared(source);
		EXPECT_TRUE(prepared.has_value()) << RefusalOf(prepared);
		if (!prepared)
		{
			return {};
		}
		auto bound = Bind(*prepared, TaggedHost(log, std::string(tag), integerThreat));
		EXPECT_TRUE(bound.has_value()) << RefusalOf(bound);
		if (!bound)
		{
			return {};
		}
		Targets()[bound->get()] = log_target{&log, std::string(tag)};
		return std::move(*bound);
	}
};

constexpr char const* kNoCounting = "a sanitizer owns operator new in this build, so nothing counts allocations";

// ---- Migration ---------------------------------------------------------------------
//
// A machine of three columns: the hash mode, the int level and the double
// speed. Entering a state sets level and speed and emits (enter X); leaving
// one sets level and emits (exit X). The quiet variant of the machine sets
// nothing on entry or exit, so after a rebind onto it every value is what
// migration left.

constexpr std::string_view kMover = R"(
[table m_step
  (key mode hash (domain rows))
  (key message hash fallthrough)
  (column next hash)
  (order mode message)]
[table m_enter
  (key mode hash (domain rows))
  (column lv int)
  (column sp double)
  (order mode)
  (suffix command)]
[table m_exit
  (key mode hash (domain rows))
  (column lv int)
  (order mode)
  (suffix command)]
[machine m
  (state mode hash (members a b c) (initial a))
  (state level int (initial 5))
  (state speed double (initial 1.5))
  (input message hash)]
list,from,where,given,set,emit,overrides
entry,m_enter,mode=state.mode,,level=lv speed=sp,command,
step,m_step,mode=state.mode message=input.message,,mode=next,,
exit,m_exit,mode=state.mode,,level=lv,command,
)";

constexpr std::string_view kMoverStepRows = R"(mode, message, next
a, ,     a
b, ,     b
c, ,     c
a, to_b, b
a, to_c, c
b, to_c, c
)";

constexpr std::string_view kMoverEnterRows = R"(mode, lv, sp, command
a, 1, 0.25, (enter a)
b, 2, 0.5,  (enter b)
c, 3, 0.75, (enter c)
)";

constexpr std::string_view kMoverExitRows = R"(mode, lv, command
a, 10, (exit a)
b, 20, (exit b)
c, 30, (exit c)
)";

enum mover_table : uint32_t { MoverStep, MoverEnter, MoverExit };

std::string Quiet(std::string_view source)
{
	std::string out = Replace(source, ",,level=lv speed=sp,command,", ",,,command,");
	return Replace(out, ",,level=lv,command,", ",,,command,");
}

// The mover's world, loaded.
std::unique_ptr<reload_world> Mover()
{
	auto world = std::make_unique<reload_world>(kMover);
	world->Load(MoverStep, kMoverStepRows);
	world->Load(MoverEnter, kMoverEnterRows);
	world->Load(MoverExit, kMoverExitRows);
	return world;
}

}

// A rebind returns the old exit's commands, views into the old binding's
// versions, and a host commonly drops the old binding straight after: the
// instance keeps it until its next run. The exit table is reloaded first, so
// once v1 is dropped the instance is the only holder of the version those
// views point into. A dangling view may still read the freed bytes, so only an
// ASan build is sure to catch the instance letting go too early; elsewhere
// this checks the views name the old version's text, not the new one's.
TEST(MachineRebind, TheOldExitsCommandsOutliveTheOldBindingUntilTheNextRun)
{
	auto w = Mover();
	counted_ptr<Binding const> v1 = w->Bound("v1");
	ASSERT_TRUE(v1);
	host_object host;
	instance mover = CreateInstance(*v1, 3, 0, &host);
	Stepped(mover, &host, 1, Message("to_b"));
	w->Load(MoverExit, Replace(kMoverExitRows, "(exit b)", "(exit b reloaded)"));
	counted_ptr<Binding const> v2 = w->Bound("v2", Quiet(kMover));
	ASSERT_TRUE(v2);
	std::span<emitted const> const results = Rebind(mover, *v2, &host, 2);
	v1 = counted_ptr<Binding const>{};
	ASSERT_EQ(results.size(), 2u);
	EXPECT_EQ(results[0].list, trigger::Exit);
	EXPECT_EQ(results[0].command, "((exit b))") << "the old version's exit, which only the instance still holds";
	EXPECT_EQ(results[1].list, trigger::Entry);
	EXPECT_EQ(results[1].command, "((enter b))");
}

TEST(MachineRebind, ValuesStillValidAreKeptAndTheOldExitRunsBeforeTheNewEntry)
{
	auto w = Mover();
	counted_ptr<Binding const> v1 = w->Bound("v1");
	ASSERT_TRUE(v1);
	host_object host;
	instance mover = Created(*v1, 3, 0, &host);
	Stepped(mover, &host, 1, Message("to_b"));
	ASSERT_EQ(mover.State(0).i, Hash("b"));
	EXPECT_EQ(mover.State(1).i, 2);
	EXPECT_EQ(w->Take(), (std::vector<std::string>{"v1:((enter a))", "v1:((exit a))", "v1:((enter b))"}));

	counted_ptr<Binding const> v2 = w->Bound("v2", Quiet(kMover));
	ASSERT_TRUE(v2);
	Rebound(mover, *v2, &host, 2);
	EXPECT_EQ(&mover.binding(), v2.get());
	EXPECT_EQ(w->Take(), (std::vector<std::string>{"v1:((exit b))", "v2:((enter b))"}))
		<< "the old code's exit, then the new code's entry";
	ASSERT_EQ(mover.StateCount(), 3u);
	EXPECT_EQ(mover.State(0).i, Hash("b")) << "still a member";
	EXPECT_EQ(mover.State(1).i, 20) << "the old exit's write, migrated: exit writes next, which becomes current";
	EXPECT_EQ(mover.State(2).d, 0.5);
}

TEST(MachineRebind, AMemberThatIsGoneResetsToTheNewInitialValue)
{
	auto w = Mover();
	counted_ptr<Binding const> v1 = w->Bound("v1");
	ASSERT_TRUE(v1);
	host_object host;
	instance mover = Created(*v1, 3, 0, &host);
	Stepped(mover, &host, 1, Message("to_c"));
	ASSERT_EQ(mover.State(0).i, Hash("c"));
	w->Take();

	// c leaves the members, and the rows that moved to it go with it.
	w->Load(MoverStep, "mode, message, next\na, , a\nb, , b\na, to_b, b\n");
	EXPECT_TRUE(mover.binding().machine().Stale(w->store));
	std::string const v2source = Replace(Quiet(kMover), "(members a b c) (initial a)", "(members a b) (initial b)");
	counted_ptr<Binding const> v2 = w->Bound("v2", v2source);
	ASSERT_TRUE(v2);
	Rebound(mover, *v2, &host, 2);
	EXPECT_EQ(mover.State(0).i, Hash("b")) << "c is no member: the new initial value, not the first member";
	EXPECT_EQ(w->Take(), (std::vector<std::string>{"v1:((exit c))", "v2:((enter b))"}));
	EXPECT_EQ(mover.State(1).i, 30);
}

TEST(MachineRebind, ColumnsMigrateByNameNotPositionAndAGoneColumnIsDropped)
{
	auto w = Mover();
	counted_ptr<Binding const> v1 = w->Bound("v1");
	ASSERT_TRUE(v1);
	host_object host;
	instance mover = Created(*v1, 3, 0, &host);
	Stepped(mover, &host, 1, Message("to_b"));
	ASSERT_EQ(mover.State(2).d, 0.5);

	std::string const v2source = Replace(Quiet(kMover),
		"  (state mode hash (members a b c) (initial a))\n"
		"  (state level int (initial 5))\n"
		"  (state speed double (initial 1.5))\n",
		"  (state speed double (initial 9))\n"
		"  (state mood hash (members calm cross) (initial cross))\n"
		"  (state mode hash (members a b c) (initial a))\n");
	counted_ptr<Binding const> v2 = w->Bound("v2", v2source);
	ASSERT_TRUE(v2);
	Rebound(mover, *v2, &host, 2);
	ASSERT_EQ(mover.StateCount(), 3u) << "level is gone";
	EXPECT_EQ(mover.State(0).d, 0.5) << "speed, kept by name";
	EXPECT_EQ(mover.State(1).i, Hash("cross")) << "mood is new: its initial value";
	EXPECT_EQ(mover.State(2).i, Hash("b")) << "mode, kept by name";
}

TEST(MachineRebind, ATypeChangeResetsIncludingIntToDoubleAndBack)
{
	auto w = Mover();
	counted_ptr<Binding const> v1 = w->Bound("v1");
	ASSERT_TRUE(v1);
	host_object host;
	instance mover = Created(*v1, 3, 0, &host);
	Stepped(mover, &host, 1, Message("to_b"));
	ASSERT_EQ(mover.State(1).i, 2);

	std::string v2source = Replace(Quiet(kMover), "(state level int (initial 5))", "(state level double (initial 0.125))");
	v2source = Replace(v2source, "(state speed double (initial 1.5))", "(state speed int (initial 7))");
	counted_ptr<Binding const> v2 = w->Bound("v2", v2source);
	ASSERT_TRUE(v2);
	Rebound(mover, *v2, &host, 2);
	EXPECT_EQ(mover.State(0).i, Hash("b"));
	EXPECT_EQ(mover.State(1).d, 0.125) << "an int that became a double is reset, not converted";
	EXPECT_EQ(mover.State(2).i, 7) << "a double that became an int is reset, not truncated";
}

TEST(MachineRebind, AfterRebindOnlyTheNewEntryHasAnswered)
{
	auto w = Mover();
	counted_ptr<Binding const> v1 = w->Bound("v1");
	ASSERT_TRUE(v1);
	host_object host;
	instance mover = Created(*v1, 3, 0, &host);
	Stepped(mover, &host, 1, Message("to_b"));
	ASSERT_EQ(mover.Answer(trigger::Step, 0).outcome, clause_outcome::Answered);

	counted_ptr<Binding const> v2 = w->Bound("v2");
	ASSERT_TRUE(v2);
	Rebound(mover, *v2, &host, 2);
	EXPECT_EQ(mover.Answer(trigger::Step, 0).outcome, clause_outcome::NotRun);
	EXPECT_EQ(mover.Answer(trigger::Exit, 0).outcome, clause_outcome::NotRun)
		<< "the exit that ran was the old binding's clause, which the new answers do not index";
	EXPECT_EQ(mover.Answer(trigger::Entry, 0).outcome, clause_outcome::Answered);
	EXPECT_EQ(mover.State(1).i, 2) << "the new entry set level again, over the old exit's 20";
}

TEST(MachineRebind, RebindingOntoItsOwnBindingRestartsTheState)
{
	auto w = Mover();
	counted_ptr<Binding const> v1 = w->Bound("v1");
	ASSERT_TRUE(v1);
	host_object host;
	instance mover = Created(*v1, 3, 0, &host);
	Stepped(mover, &host, 1, Message("to_c"));
	w->Take();
	v1 = counted_ptr<Binding const>{};   // the instance holds the only reference
	Rebound(mover, mover.binding(), &host, 2);
	EXPECT_EQ(w->Take(), (std::vector<std::string>{"v1:((exit c))", "v1:((enter c))"}));
	EXPECT_EQ(mover.State(0).i, Hash("c"));
	EXPECT_EQ(mover.State(1).i, 3);
}

// ---- Edges and reports across a rebind -------------------------------------------
//
// A grazer startles when threat crosses 0.5 upwards. Its entry has an edge
// too: entering graze as threat crosses emits (anim graze startled).

namespace
{

constexpr std::string_view kStartle = R"(
[table startle
  (input threat double 0..1)
  (key mode hash (domain rows))
  (column rise band (input threat) edge)
  (column next hash)
  (order mode)]
[table startle_enter
  (input threat double 0..1)
  (key mode hash (domain rows))
  (column rise band (input threat) edge)
  (order mode)
  (suffix command)]
[machine grazer
  (state mode hash (members graze startled) (initial graze))
  (input threat double 0..1)]
list,from,where,given,set,emit,overrides
entry,startle_enter,mode=state.mode,threat=input.threat,,command,
step,startle,mode=state.mode,threat=input.threat,mode=next,,
)";

constexpr std::string_view kStartleRows = R"(mode, rise, next
graze,    0.5..1, startled
graze,    ,       graze
startled, ,       graze
)";

constexpr std::string_view kStartleEnterRows = R"(mode, rise, command
graze,    0.5..1, (anim graze startled)
graze,    ,       (anim graze)
startled, ,       (anim startle)
)";

// The grazer reading the host's alarm wire where it read threat: alarm is an
// input the old machine did not have.
std::string Alarmed()
{
	std::string source = Replace(kStartle, "(initial graze))\n  (input threat double 0..1)]",
		"(initial graze))\n  (input alarm double 0..1)]");
	source = Replace(source, "step,startle,mode=state.mode,threat=input.threat,", "step,startle,mode=state.mode,threat=input.alarm,");
	return Replace(source, "entry,startle_enter,mode=state.mode,threat=input.threat,", "entry,startle_enter,mode=state.mode,threat=input.alarm,");
}

// The grazer with an exit table reading threat both ways: a plain band (is
// threat high as graze is left?) and an edge (did it just cross?).
std::string WithExit()
{
	std::string source = Replace(kStartle, "[machine grazer", R"([table startle_exit
  (input threat double 0..1)
  (key mode hash (domain rows))
  (column rise band (input threat) edge)
  (column high band (input threat))
  (order mode)
  (suffix command)]
[machine grazer)");
	return source + "exit,startle_exit,mode=state.mode,threat=input.threat,,command,\n";
}

constexpr std::string_view kStartleExitRows = R"(mode, rise, high, command
graze,    0.5..1, 0..1,   (exit graze startled)
graze,    ,       0.5..1, (exit graze high)
graze,    ,       0..0.5, (exit graze)
startled, ,       0..1,   (exit startled)
)";

std::unique_ptr<reload_world> Startle()
{
	auto world = std::make_unique<reload_world>(kStartle);
	world->Load(0, kStartleRows);
	world->Load(1, kStartleEnterRows);
	return world;
}

}

TEST(MachineRebind, AReloadCrossesNoEdgeAndSpendsNone)
{
	auto w = Startle();
	counted_ptr<Binding const> v1 = w->Bound("v1");
	ASSERT_TRUE(v1);
	host_object host;
	host.threat     = 0.1;
	instance grazer = Created(*v1, 1, 0, &host);
	EXPECT_EQ(w->Take(), (std::vector<std::string>{"v1:((anim graze))"}));

	host.threat = 0.9;   // crossed since the last read, and no step has seen it
	counted_ptr<Binding const> v2 = w->Bound("v2");
	ASSERT_TRUE(v2);
	Rebound(grazer, *v2, &host, 1);
	EXPECT_EQ(w->Take(), (std::vector<std::string>{"v2:((anim graze))"}))
		<< "entering again is no movement of threat: the entry's edge does not hold";
	EXPECT_EQ(grazer.State(0).i, Hash("graze"));

	Stepped(grazer, &host, 2);
	EXPECT_EQ(grazer.State(0).i, Hash("startled")) << "the crossing from 0.1 is still the next step's";
	EXPECT_EQ(w->Take(), (std::vector<std::string>{"v2:((anim startle))"}));
}

TEST(MachineRebind, TheOldExitReadsTheInputsAsTheyAreAndCrossesNoEdge)
{
	reload_world w(WithExit());
	w.Load(0, kStartleRows);
	w.Load(1, kStartleEnterRows);
	w.Load(2, kStartleExitRows);
	counted_ptr<Binding const> v1 = w.Bound("v1");
	ASSERT_TRUE(v1);
	host_object host;
	host.threat     = 0.1;
	instance grazer = Created(*v1, 1, 0, &host);
	w.Take();

	host.threat = 0.9;
	counted_ptr<Binding const> v2 = w.Bound("v2");
	ASSERT_TRUE(v2);
	Rebound(grazer, *v2, &host, 1);
	EXPECT_EQ(w.Take(), (std::vector<std::string>{"v1:((exit graze high))", "v2:((anim graze))"}))
		<< "exit reads threat now (high), not as the last step read it, and sees no crossing";
}

TEST(MachineRebind, AnInputNewToTheMachineTakesTheValueReadAtRebindAsItsPrevious)
{
	auto w = Startle();
	counted_ptr<Binding const> v1 = w->Bound("v1");
	ASSERT_TRUE(v1);
	host_object host;
	host.threat     = 0.1;
	host.alarm      = 0.1;
	instance grazer = Created(*v1, 1, 0, &host);
	counted_ptr<Binding const> v2 = w->Bound("v2", Alarmed());
	ASSERT_TRUE(v2);
	Rebound(grazer, *v2, &host, 1);
	w->Take();

	host.alarm = 0.9;
	Stepped(grazer, &host, 2);
	EXPECT_EQ(grazer.State(0).i, Hash("startled"))
		<< "alarm's previous value is the 0.1 read at rebind, as at creation; with none, nothing would cross";
}

TEST(MachineRebind, AnInputThatNeverHadAValueTakesTheValueReadAtRebind)
{
	auto w = Startle();
	counted_ptr<Binding const> v1 = w->Bound("v1");
	ASSERT_TRUE(v1);
	host_object host;
	host.unwired    = true;   // threat has no previous value
	instance grazer = Created(*v1, 1, 0, &host);
	host.unwired = false;
	host.threat  = 0.1;
	counted_ptr<Binding const> v2 = w->Bound("v2");
	ASSERT_TRUE(v2);
	Rebound(grazer, *v2, &host, 1);

	host.threat = 0.9;
	Stepped(grazer, &host, 2);
	EXPECT_EQ(grazer.State(0).i, Hash("startled"))
		<< "with no value to keep, threat's previous is the 0.1 read at rebind, as for a new input";
}

TEST(MachineRebind, AnInputWhoseTypeChangedIsANewInput)
{
	auto w = Startle();
	// The old machine reads alarm, and declares threat an int whose wire answers
	// a hundredth of the host's threat (90 for 0.9), which it never reads.
	std::string const v1source = Replace(Alarmed(), "(input alarm double 0..1)]",
		"(input alarm double 0..1)\n  (input threat int)]");
	w->integerThreat = true;
	counted_ptr<Binding const> v1 = w->Bound("v1", v1source);
	ASSERT_TRUE(v1);
	host_object host;
	host.threat     = 0.9;
	instance grazer = Created(*v1, 1, 0, &host);
	w->integerThreat = false;
	counted_ptr<Binding const> v2 = w->Bound("v2");
	ASSERT_TRUE(v2);
	Rebound(grazer, *v2, &host, 1);

	Stepped(grazer, &host, 2);
	EXPECT_EQ(grazer.State(0).i, Hash("graze"))
		<< "threat's previous is the 0.9 read at rebind, so it crosses nothing; the int's 90, read as the "
		   "double's bits, would be a value near 0 and cross";
	host.threat = 0.1;
	Stepped(grazer, &host, 3);
	host.threat = 0.9;
	Stepped(grazer, &host, 4);
	EXPECT_EQ(grazer.State(0).i, Hash("startled")) << "the machine still crosses an edge it should";
}

TEST(MachineRebind, TheReportOnceMemoryIsClearedForTheNewBinding)
{
	auto w = Startle();
	counted_ptr<Binding const> v1 = w->Bound("v1");
	ASSERT_TRUE(v1);
	host_object host;
	host.unwired    = true;
	instance grazer = Created(*v1, 1, 0, &host);
	Stepped(grazer, &host, 1);
	EXPECT_EQ(w->log.reports, 1) << "the lost getter, once";

	counted_ptr<Binding const> v2 = w->Bound("v2");
	ASSERT_TRUE(v2);
	Rebound(grazer, *v2, &host, 2);
	EXPECT_EQ(w->log.reports, 2) << "a new binding is a new set of failures: the getter is reported again";
	Stepped(grazer, &host, 3);
	Stepped(grazer, &host, 4);
	EXPECT_EQ(w->log.reports, 2);
}

TEST(MachineRebind, AnOldExitThatHoldsReportsToTheOldBindingAndSendsNoAbort)
{
	reload_world w(R"(
[table bye
  (key mode hash (domain rows))
  (column when condition)
  (order mode)
  (suffix command)]
[table hello
  (key mode hash (domain rows))
  (order mode)
  (suffix command)]
[machine leaver (state mode hash (members a) (initial a))]
list,from,where,given,set,emit,overrides
entry,hello,mode=state.mode,,,command,
exit,bye,mode=state.mode,,,command,
)");
	w.Load(0, "mode, when, command\na, cornered, (abort a)\na, , (abort plain)\n");
	w.Load(1, "mode, command\na, (hello)\n");
	counted_ptr<Binding const> v1 = w.Bound("v1");
	counted_ptr<Binding const> v2 = w.Bound("v2");
	ASSERT_TRUE(v1 && v2);
	host_object host;
	instance    healthy = Created(*v1, 1, 0, &host);
	instance    broken  = Created(*v1, 2, 0, &host);
	w.Take();

	Rebound(healthy, *v2, &host, 1);
	EXPECT_EQ(w.Take(), (std::vector<std::string>{"v1:((abort a))", "v2:((hello))"})) << "the control: a condition that answers aborts";
	EXPECT_EQ(w.log.reports, 0);

	host.broken = true;
	Rebound(broken, *v2, &host, 2);
	EXPECT_EQ(broken.Answer(trigger::Entry, 0).outcome, clause_outcome::Answered);
	EXPECT_EQ(w.Take(), (std::vector<std::string>{"v2:((hello))"})) << "no abort is sent; the new entry still runs";
	EXPECT_EQ(w.log.reported, (std::vector<std::string>{"v1"})) << "the failure reached the old binding's report, once";
}

TEST(MachineRebind, StepsAfterARebindOntoAWiderMachineAllocateNothing)
{
	if (!kCountsAllocations)
	{
		GTEST_SKIP() << kNoCounting;
	}
	auto mover = Mover();
	auto startle = Startle();
	counted_ptr<Binding const> narrow = mover->Bound("m");
	counted_ptr<Binding const> wide   = startle->Bound("s");
	ASSERT_TRUE(narrow && wide);
	host_object host;
	instance subject = Created(*narrow, 1, 0, &host);
	Rebound(subject, *wide, &host, 1);
	EXPECT_EQ(subject.State(0).i, Hash("graze")) << "mode's a is no member of the grazer's mode";
	startle->Take();
	startle->log.record = false;

	AllocationCount count;
	for (int i = 0; i < 40; ++i)
	{
		host.threat = (i % 4) * 0.3;
		Stepped(subject, &host, uint64_t(i));
	}
	EXPECT_EQ(count.Count(), 0u);
}

// ---- The worked example's reload (spec §4) ---------------------------------------
//
// The spec's grazer with an exit table that aborts what entry started. The
// whole reload cycle is the host's composition of what exists: load the
// changed rows, ask Stale, Prepare the definition again, Bind, Rebind.

namespace
{

constexpr std::string_view kGrazer = R"(
[table grazer_brain
  (input energy double 0..1)
  (input food_near flags)
  (key mode hash (domain rows))
  (key message hash fallthrough)
  (column energy band (input energy))
  (column near flags (input food_near))
  (column next hash)
  (order mode message)]
[table grazer_enter
  (key mode hash (domain rows))
  (order mode)
  (suffix command)]
[table grazer_exit
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
exit,grazer_exit,mode=state.mode,,,command,
)";

constexpr std::string_view kBrainRows = R"(# grazer_brain
mode,  message, energy, near, next
idle,  ,        0..0.3, 1,    eat
idle,  ,        0..1,   0,    walk
walk,  ,        0..1,   0,    walk
eat,   ,        0..1,   0,    eat
idle,  scared,  0..1,   0,    panic
walk,  scared,  0..1,   0,    panic
eat,   scared,  0..1,   0,    panic
panic, ,        0..1,   0,    panic
panic, calm,    0..1,   0,    idle
)";

constexpr std::string_view kEnterRows = R"csv(# grazer_enter
mode,  command
idle,  (anim stand)
walk,  (anim walk)
eat,   (anim chew), "(message food ""eat"")"
panic, (anim run), "(message this ""calm"" :delay random(0.4, 1.0))"
)csv";

constexpr std::string_view kExitRows = R"(# grazer_exit
mode,  command
idle,  (abort stand)
walk,  (abort walk)
eat,   (abort chew), (drop food)
panic, (abort run)
)";

enum grazer_table : uint32_t { Brain, Enter, Exit };

std::unique_ptr<reload_world> Grazer()
{
	auto world = std::make_unique<reload_world>(kGrazer);
	world->Load(Brain, kBrainRows);
	world->Load(Enter, kEnterRows);
	world->Load(Exit, kExitRows);
	return world;
}

// What a host does on a reload, for every instance of one binding: the
// machine is prepared again (from source when its definition changed too),
// bound, and each instance rebound. Nothing here is Tabular's.
counted_ptr<Binding const> Reload(reload_world& world, std::span<instance* const> instances, host_object const& host,
	std::string_view tag, std::string_view source = {})
{
	counted_ptr<Binding const> fresh = world.Bound(tag, source);
	if (!fresh)
	{
		return fresh;
	}
	for (instance* subject : instances)
	{
		Rebound(*subject, *fresh, &host, 9);
	}
	return fresh;
}

}

TEST(GrazerReload, ARowsChangeIsStaleAndTheReloadRestartsTheStateWithTheNewRows)
{
	auto w = Grazer();
	counted_ptr<Binding const> v1 = w->Bound("v1");
	ASSERT_TRUE(v1);
	host_object host;
	instance    first  = Created(*v1, 1, 0, &host);
	instance    second = Created(*v1, 2, 0, &host);
	Stepped(second, &host, 1);
	ASSERT_EQ(second.State(0).i, Hash("walk"));
	w->Take();

	EXPECT_FALSE(v1->machine().Stale(w->store));
	w->Load(Enter, Replace(kEnterRows, "idle,  (anim stand)", "idle,  (anim stand tall)"));
	ASSERT_TRUE(v1->machine().Stale(w->store)) << "the rows changed: the host prepares again";
	instance* const both[] = {&first, &second};
	counted_ptr<Binding const> v2 = Reload(*w, both, host, "v2");
	ASSERT_TRUE(v2);
	EXPECT_FALSE(v2->machine().Stale(w->store));
	EXPECT_EQ(w->Take(), (std::vector<std::string>{"v1:((abort stand))", "v2:((anim stand tall))", "v1:((abort walk))",
		"v2:((anim walk))"}));
	v1 = counted_ptr<Binding const>{};   // the host drops the old binding; nothing refers to it now

	host.food_near = 1;
	host.energy    = 0.1;
	Stepped(first, &host, 2);
	EXPECT_EQ(first.State(0).i, Hash("eat"));
	EXPECT_EQ(w->Take(), (std::vector<std::string>{"v2:((abort stand))", "v2:((anim chew) (message food \"eat\"))"}));
}

TEST(GrazerReload, AddingTheMemberSleepKeepsTheMode)
{
	auto w = Grazer();
	counted_ptr<Binding const> v1 = w->Bound("v1");
	ASSERT_TRUE(v1);
	host_object host;
	instance    grazer = Created(*v1, 1, 0, &host);
	Stepped(grazer, &host, 1);
	ASSERT_EQ(grazer.State(0).i, Hash("walk"));
	w->Take();

	std::string const brain = std::string(kBrainRows) + "sleep, ,        0..1,   0,    sleep\n"
		"walk,  tired,   0..1,   0,    sleep\nsleep, scared,  0..1,   0,    panic\n";
	w->Load(Brain, brain);
	w->Load(Enter, std::string(kEnterRows) + "sleep, (anim sleep)\n");
	w->Load(Exit, std::string(kExitRows) + "sleep, (abort sleep)\n");
	ASSERT_TRUE(v1->machine().Stale(w->store));
	std::string const v2source = Replace(kGrazer, "(members idle walk eat panic)", "(members idle walk eat panic sleep)");
	instance* const one[] = {&grazer};
	counted_ptr<Binding const> v2 = Reload(*w, one, host, "v2", v2source);
	ASSERT_TRUE(v2);
	EXPECT_EQ(grazer.State(0).i, Hash("walk")) << "walk is still a member";
	EXPECT_EQ(w->Take(), (std::vector<std::string>{"v1:((abort walk))", "v2:((anim walk))"}));

	Stepped(grazer, &host, 2, Message("tired"));
	EXPECT_EQ(grazer.State(0).i, Hash("sleep"));
	EXPECT_EQ(w->Take(), (std::vector<std::string>{"v2:((abort walk))", "v2:((anim sleep))"}));
}

TEST(GrazerReload, DeletingPanicWhileAnInstancePanicsResetsItToIdleAndEntersIdle)
{
	auto w = Grazer();
	counted_ptr<Binding const> v1 = w->Bound("v1");
	ASSERT_TRUE(v1);
	host_object host;
	instance    grazer = Created(*v1, 1, 0, &host);
	Stepped(grazer, &host, 1, Message("scared"));
	ASSERT_EQ(grazer.State(0).i, Hash("panic"));
	w->Take();

	w->Load(Brain, R"(# grazer_brain
mode,  message, energy, near, next
idle,  ,        0..0.3, 1,    eat
idle,  ,        0..1,   0,    walk
walk,  ,        0..1,   0,    walk
eat,   ,        0..1,   0,    eat
)");
	w->Load(Enter, Replace(kEnterRows, "panic, (anim run), \"(message this \"\"calm\"\" :delay random(0.4, 1.0))\"\n", ""));
	w->Load(Exit, Replace(kExitRows, "panic, (abort run)\n", ""));
	ASSERT_TRUE(v1->machine().Stale(w->store));
	std::string const v2source = Replace(kGrazer, "(members idle walk eat panic)", "(members idle walk eat)");
	instance* const one[] = {&grazer};
	counted_ptr<Binding const> v2 = Reload(*w, one, host, "v2", v2source);
	ASSERT_TRUE(v2);
	EXPECT_EQ(grazer.State(0).i, Hash("idle")) << "panic is no member: the initial value";
	EXPECT_EQ(w->Take(), (std::vector<std::string>{"v1:((abort run))", "v2:((anim stand))"}))
		<< "the old exit aborts the run that the old entry started; the new entry starts idle's";
}

TEST(GrazerReload, AFailedPrepareLeavesTheOldBindingRunning)
{
	auto w = Grazer();
	counted_ptr<Binding const> v1 = w->Bound("v1");
	ASSERT_TRUE(v1);
	host_object host;
	instance    grazer = Created(*v1, 1, 0, &host);
	w->Take();
	// Deleting panic from the members without deleting the rows that move to
	// it: the new machine is refused, and the host keeps stepping the old one.
	std::string const v2source = Replace(kGrazer, "(members idle walk eat panic)", "(members idle walk eat)");
	auto refused = w->Prepared(v2source);
	EXPECT_TRUE(Mentions(RefusalOf(refused), "member")) << RefusalOf(refused);
	Stepped(grazer, &host, 1, Message("scared"));
	EXPECT_EQ(grazer.State(0).i, Hash("panic"));
	EXPECT_EQ(&grazer.binding(), v1.get());
}

// ---- Bridges are states (R5(b)) ----------------------------------------------------
//
// A transition writes mode = bridge_c and target = walk; the bridge states have
// rows keyed on target that move along when the clip_done message says the
// clip ended, and the last one moves to the target. Nothing here is new: two
// hash state columns, a step clause keyed on both and the message, an entry
// clause emitting each state's clip.

namespace
{

constexpr std::string_view kWalker = R"(
[table walker_brain
  (key mode hash (domain rows))
  (key target hash (domain rows))
  (key message hash fallthrough)
  (column next hash)
  (column goal hash)
  (order mode target message)]
[table walker_enter
  (key mode hash (domain rows))
  (order mode)
  (suffix command)]
[machine walker
  (state mode hash (members idle walk bridge_c bridge_d bridge_e) (initial idle))
  (state target hash (members idle walk) (initial idle))
  (input message hash)]
list,from,where,given,set,emit,overrides
entry,walker_enter,mode=state.mode,,,command,
step,walker_brain,mode=state.mode target=state.target message=input.message,,mode=next target=goal,,
)";

constexpr std::string_view kWalkerStayRows = R"(mode, target, message, next, goal
idle,     idle, ,          idle,     idle
idle,     walk, ,          idle,     walk
walk,     idle, ,          walk,     idle
walk,     walk, ,          walk,     walk
bridge_c, idle, ,          bridge_c, idle
bridge_c, walk, ,          bridge_c, walk
bridge_d, idle, ,          bridge_d, idle
bridge_d, walk, ,          bridge_d, walk
bridge_e, idle, ,          bridge_e, idle
bridge_e, walk, ,          bridge_e, walk
)";

constexpr std::string_view kWalkerMoveRows = R"(idle,     idle, go,         bridge_c, walk
bridge_c, walk, clip_done,  bridge_d, walk
bridge_d, walk, clip_done,  bridge_e, walk
bridge_e, walk, clip_done,  walk,     walk
)";

constexpr std::string_view kWalkerEnterRows = R"(mode, command
idle,     (anim stand)
walk,     (anim walk)
bridge_c, (anim rise_c)
bridge_d, (anim rise_d)
bridge_e, (anim rise_e)
)";

}

TEST(MachineBridge, ABridgeIsStatesThatMoveAlongWhenTheClipEnds)
{
	reload_world w(kWalker);
	w.Load(0, std::string(kWalkerStayRows) + std::string(kWalkerMoveRows));
	w.Load(1, kWalkerEnterRows);
	counted_ptr<Binding const> bound = w.Bound("w");
	ASSERT_TRUE(bound);
	host_object host;
	instance    walker = Created(*bound, 1, 0, &host);
	EXPECT_EQ(w.Take(), (std::vector<std::string>{"w:((anim stand))"}));

	Stepped(walker, &host, 1, Message("go"));
	EXPECT_EQ(walker.State(0).i, Hash("bridge_c"));
	EXPECT_EQ(walker.State(1).i, Hash("walk")) << "the transition wrote where the bridge leads";
	EXPECT_EQ(w.Take(), (std::vector<std::string>{"w:((anim rise_c))"}));

	Stepped(walker, &host, 2);
	EXPECT_EQ(walker.State(0).i, Hash("bridge_c")) << "the clip plays: the bridge holds";
	EXPECT_TRUE(w.Take().empty());

	char const* const expected[][2] = {{"bridge_d", "w:((anim rise_d))"}, {"bridge_e", "w:((anim rise_e))"}, {"walk", "w:((anim walk))"}};
	for (auto const& [mode, command] : expected)
	{
		Stepped(walker, &host, 3, Message("clip_done"));
		EXPECT_EQ(walker.State(0).i, Hash(mode));
		EXPECT_FALSE(walker.Answer(trigger::Step, 0).fellThrough);
		EXPECT_EQ(w.Take(), (std::vector<std::string>{command}));
	}
	Stepped(walker, &host, 4, Message("clip_done"));
	EXPECT_EQ(walker.State(0).i, Hash("walk")) << "a stray clip_done falls through to walk's blank run";
	EXPECT_TRUE(walker.Answer(trigger::Step, 0).fellThrough);
}

TEST(MachineBridge, AFunctionChainIsStatesThatStartFunctionsAndMoveAlongOnTheReturn)
{
	// The bridge's machine with functions where it had clips: bridge_c runs f,
	// bridge_d runs g, and a returned message moves along from each.
	reload_world w(kWalker);
	w.Load(0, std::string(kWalkerStayRows) + R"(idle,     idle, go,         bridge_c, walk
bridge_c, walk, returned,   bridge_d, walk
bridge_d, walk, returned,   walk,     walk
)");
	w.Load(1, R"(mode, command
idle,     (idle)
walk,     (done)
bridge_c, (start f)
bridge_d, (start g)
bridge_e, (start h)
)");
	counted_ptr<Binding const> bound = w.Bound("w");
	ASSERT_TRUE(bound);
	host_object host;
	instance    walker = Created(*bound, 1, 0, &host);
	w.Take();

	Stepped(walker, &host, 1, Message("go"));
	EXPECT_EQ(w.Take(), (std::vector<std::string>{"w:((start f))"}));
	Stepped(walker, &host, 2);
	EXPECT_TRUE(w.Take().empty()) << "f runs: the chain holds";
	Stepped(walker, &host, 3, Message("returned"));
	EXPECT_EQ(w.Take(), (std::vector<std::string>{"w:((start g))"}));
	Stepped(walker, &host, 4);
	EXPECT_EQ(walker.State(0).i, Hash("bridge_d")) << "g runs: the chain holds";
	Stepped(walker, &host, 5, Message("returned"));
	EXPECT_EQ(walker.State(0).i, Hash("walk"));
	EXPECT_EQ(w.Take(), (std::vector<std::string>{"w:((done))"}));
}

TEST(MachineBridge, ABridgeStateWithNoRowsIsRefusedByTheMachinesProof)
{
	reload_world w(kWalker);
	// bridge_d has no rows for target walk: each table's own domain (rows) is
	// total, but the machine's, closed by both columns' members, is not.
	std::string rows = Replace(kWalkerStayRows, "bridge_d, walk, ,          bridge_d, walk\n", "");
	rows += Replace(kWalkerMoveRows, "bridge_d, walk, clip_done,  bridge_e, walk\n", "");
	w.Load(0, rows);
	w.Load(1, kWalkerEnterRows);
	auto prepared = w.Prepared();
	std::string const refusal = RefusalOf(prepared);
	EXPECT_TRUE(Mentions(refusal, "machine 'walker' step clause 0")) << refusal;
	EXPECT_TRUE(Mentions(refusal, "the key is in the clause's domain")) << refusal;
}

// ---- State functions (R6) ----------------------------------------------------------
//
// Entering graze starts the host's graze_fn; leaving graze aborts it; the
// function's return is a message the step list keys on. While it runs the
// machine holds graze. A reload aborts it with the old code and starts it
// with the new. Nothing here is new: an entry and an exit table emitting
// commands, and a step row keyed on a message.

namespace
{

constexpr std::string_view kForager = R"(
[table forager_brain
  (key mode hash (domain rows))
  (key message hash fallthrough)
  (column next hash)
  (order mode message)]
[table forager_enter
  (key mode hash (domain rows))
  (order mode)
  (suffix command)]
[table forager_exit
  (key mode hash (domain rows))
  (order mode)
  (suffix command)]
[machine forager
  (state mode hash (members idle graze) (initial idle))
  (input message hash)]
list,from,where,given,set,emit,overrides
entry,forager_enter,mode=state.mode,,,command,
step,forager_brain,mode=state.mode message=input.message,,mode=next,,
exit,forager_exit,mode=state.mode,,,command,
)";

constexpr std::string_view kForagerBrainRows = R"(mode, message, next
idle,  ,           idle
idle,  hungry,     graze
graze, ,           graze
graze, fn_returned, idle
)";

constexpr std::string_view kForagerEnterRows = "mode, command\nidle, (start stand_fn)\ngraze, (start graze_fn)\n";
constexpr std::string_view kForagerExitRows  = "mode, command\nidle, (abort stand_fn)\ngraze, (abort graze_fn)\n";

std::unique_ptr<reload_world> Forager()
{
	auto world = std::make_unique<reload_world>(kForager);
	world->Load(0, kForagerBrainRows);
	world->Load(1, kForagerEnterRows);
	world->Load(2, kForagerExitRows);
	return world;
}

}

TEST(StateFunction, EntryStartsItTheMachineHoldsUntilItReturnsAndExitAbortsIt)
{
	auto w = Forager();
	counted_ptr<Binding const> bound = w->Bound("f");
	ASSERT_TRUE(bound);
	host_object host;
	instance    forager = Created(*bound, 1, 0, &host);
	EXPECT_EQ(w->Take(), (std::vector<std::string>{"f:((start stand_fn))"}));

	Stepped(forager, &host, 1, Message("hungry"));
	EXPECT_EQ(w->Take(), (std::vector<std::string>{"f:((abort stand_fn))", "f:((start graze_fn))"}));
	for (int i = 0; i < 3; ++i)
	{
		Stepped(forager, &host, 2);
		EXPECT_EQ(forager.State(0).i, Hash("graze")) << "graze_fn runs; the state holds";
	}
	EXPECT_TRUE(w->Take().empty());

	Stepped(forager, &host, 3, Message("fn_returned"));
	EXPECT_EQ(forager.State(0).i, Hash("idle"));
	EXPECT_FALSE(forager.Answer(trigger::Step, 0).fellThrough);
	EXPECT_EQ(w->Take(), (std::vector<std::string>{"f:((abort graze_fn))", "f:((start stand_fn))"}))
		<< "leaving aborts it whether or not it returned; for one that returned the host has nothing to abort";
}

TEST(StateFunction, AReloadAbortsItWithTheOldCodeAndStartsItWithTheNew)
{
	auto w = Forager();
	counted_ptr<Binding const> v1 = w->Bound("v1");
	ASSERT_TRUE(v1);
	host_object host;
	instance    forager = Created(*v1, 1, 0, &host);
	Stepped(forager, &host, 1, Message("hungry"));
	w->Take();

	w->Load(1, Replace(kForagerEnterRows, "(start graze_fn)", "(start graze_fn_v2)"));
	w->Load(2, Replace(kForagerExitRows, "(abort graze_fn)", "(abort graze_fn_v2)"));
	ASSERT_TRUE(v1->machine().Stale(w->store));
	instance* const one[] = {&forager};
	counted_ptr<Binding const> v2 = Reload(*w, one, host, "v2");
	ASSERT_TRUE(v2);
	EXPECT_EQ(forager.State(0).i, Hash("graze"));
	EXPECT_EQ(w->Take(), (std::vector<std::string>{"v1:((abort graze_fn))", "v2:((start graze_fn_v2))"}))
		<< "the abort names the function the old code started; the new code starts its own";

	Stepped(forager, &host, 2, Message("fn_returned"));
	EXPECT_EQ(w->Take(), (std::vector<std::string>{"v2:((abort graze_fn_v2))", "v2:((start stand_fn))"}));
}
