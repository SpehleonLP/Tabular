#include <gtest/gtest.h>
#include "Support/allocationtestingsupport.h"
#include "tabular/document.h"
#include "tabular/instance.h"
#include "tabular/machine.h"
#include "tabular/pick.h"
#include "tabular/relation.h"
#include "tabular/rows_csv.h"
#include "cityhash/src/city.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace tabular;

// A step's effects: the commands it returns (P12), the failures it
// reports once (P15), the fall-through key a host-filled event feeds
// (commands-and-messages spec, Part 3) and the exit list it runs when a state
// is left (R6). The worked example is the spec's grazer (§4) with an exit
// table that aborts what entry started.

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

// The host object: what its getters read and its conditions test.
struct host_object
{
	double  energy    = 0.8;
	int64_t food_near = 0;
	int64_t level     = 0;
	double  t         = 0.5;
	// What the host fills for one step from the event it dispatches: its type
	// (0 for none) and the food's distance it carries.
	int64_t event     = 0;
	double  distance  = 0;
	// A collision's payload, which a handler machine's getters read.
	double  ny        = 0;
	double  speed     = 0;
	// What the craft bench's gambit conditions test.
	bool    sort_open = false;
	bool    can_back  = false;
	bool    cornered  = false;
	bool    broken    = false;   // every condition fails to answer
	bool    unwired   = false;   // the level getter fails to answer
	// How often each flaky condition was asked: it fails the first time only.
	mutable int flakyCalls   = 0;
	mutable int flakyOrCalls = 0;
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

cell ReadT(void*, void const* self, ErrorContext*)
{
	return cell::Real(Host(self).t);
}

cell ReadEvent(void*, void const* self, ErrorContext*)
{
	return cell::Integer(Host(self).event);
}

cell ReadDistance(void*, void const* self, ErrorContext*)
{
	return cell::Real(Host(self).distance);
}

cell ReadNy(void*, void const* self, ErrorContext*)
{
	return cell::Real(Host(self).ny);
}

cell ReadSpeed(void*, void const* self, ErrorContext*)
{
	return cell::Real(Host(self).speed);
}

bool SortOpen(void*, void const* self, ErrorContext*)
{
	return Host(self).sort_open;
}

bool CanBack(void*, void const* self, ErrorContext*)
{
	return Host(self).can_back;
}

cell ReadLevel(void*, void const* self, ErrorContext* error_out)
{
	if (Host(self).unwired)
	{
		*error_out = ErrorContext("no wire");
		return cell::Integer(0);
	}
	return cell::Integer(Host(self).level);
}

bool Cornered(void*, void const* self, ErrorContext* error_out)
{
	if (Host(self).broken)
	{
		*error_out = ErrorContext("dead target");
		return false;
	}
	return Host(self).cornered;
}

// Fails to answer the first time it is asked, and holds every time after: a
// second asking would see a different answer than the step did.
bool Flaky(int& calls, ErrorContext* error_out)
{
	if (calls++ == 0)
	{
		*error_out = ErrorContext("first failure");
		return false;
	}
	return true;
}

bool FlakyBest(void*, void const* self, ErrorContext* error_out)
{
	return Flaky(Host(self).flakyCalls, error_out);
}

bool FlakyOr(void*, void const* self, ErrorContext* error_out)
{
	return Flaky(Host(self).flakyOrCalls, error_out);
}

tl::expected<closure, ErrorContext> Conditions(std::string_view text)
{
	if (text == "cornered")
	{
		return closure{&Cornered, nullptr, nullptr};
	}
	if (text == "flaky")
	{
		return closure{&FlakyBest, nullptr, nullptr};
	}
	if (text == "flaky_or")
	{
		return closure{&FlakyOr, nullptr, nullptr};
	}
	if (text == "sort_open")
	{
		return closure{&SortOpen, nullptr, nullptr};
	}
	if (text == "can_back")
	{
		return closure{&CanBack, nullptr, nullptr};
	}
	return tl::make_unexpected(ErrorContext("no condition '", text, "'"));
}

// What the runs returned and the report was handed. With record off nothing
// is kept but the counts, so a counted step allocates nothing on the host's
// side.
struct effect_log
{
	struct command
	{
		uint64_t    instance;
		uint32_t    table;
		std::string text;
	};
	struct report
	{
		uint64_t    instance;
		uint32_t    table;
		size_t      row;
		std::string error;
	};
	std::vector<command> commands;
	std::vector<report>  reports;
	int                  commandCount = 0;
	int                  reportCalls = 0;
	bool                 record      = true;

	void Record(uint64_t instance, std::span<emitted const> results)
	{
		commandCount += static_cast<int>(results.size());
		if (!record)
		{
			return;
		}
		for (emitted const& e : results)
		{
			commands.push_back(command{instance, e.table, std::string(e.command)});
		}
	}

	// The commands since the last call, as "table-index:text".
	std::vector<std::string> Take()
	{
		std::vector<std::string> out;
		for (command const& c : commands)
		{
			out.push_back(std::to_string(c.table) + ":" + c.text);
		}
		commands.clear();
		return out;
	}
};

host_binding LoggingHost(effect_log& log)
{
	host_binding host;
	host.resolve = [](std::string_view name) -> tl::expected<input_getter, ErrorContext>
	{
		if (name == "energy")
		{
			return input_getter{&ReadEnergy};
		}
		if (name == "food_near")
		{
			return input_getter{&ReadFoodNear};
		}
		if (name == "level")
		{
			return input_getter{&ReadLevel};
		}
		if (name == "t")
		{
			return input_getter{&ReadT};
		}
		if (name == "event")
		{
			return input_getter{&ReadEvent};
		}
		if (name == "distance")
		{
			return input_getter{&ReadDistance};
		}
		if (name == "ny")
		{
			return input_getter{&ReadNy};
		}
		if (name == "speed")
		{
			return input_getter{&ReadSpeed};
		}
		return tl::make_unexpected(ErrorContext("no wire named '", name, "'"));
	};
	host.report = [&log](uint64_t instance, uint32_t table, size_t row, ErrorContext const& error)
	{
		++log.reportCalls;
		if (log.record)
		{
			log.reports.push_back(effect_log::report{instance, table, row, error.format()});
		}
	};
	return host;
}

// An event the host dispatches: its type and its payload.
struct event
{
	int64_t               type = 0;
	std::span<cell const> arguments;
};

// A document's tables loaded with rows (one text per table, in table order),
// its first machine prepared and bound to LoggingHost(log).
struct effects_fixture
{
	effect_log                 log;
	schema_document            document;
	relation_store             store;
	counted_ptr<Machine const> machine;
	counted_ptr<Binding const> binding;

	effects_fixture(std::string_view source, std::vector<std::string_view> const& rows)
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
		auto bound = Bind(machine, LoggingHost(log));
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

	instance Create(host_object const& host, uint64_t id = 7)
	{
		instance out = CreateInstance(*binding, id, 1, &host);
		log.Record(out.id(), out.Emitted());
		return out;
	}

	// A step, its commands kept in log. With an event, the host fills the
	// event input and the distance it carries (0 when it carries none) for
	// this one step, as a host dispatching an event does; Tabular sees an
	// ordinary step.
	std::span<emitted const> Step(instance& subject, host_object* self, uint64_t seed, event const& e)
	{
		self->event    = e.type;
		self->distance = e.arguments.empty() ? 0 : e.arguments[0].d;
		std::span<emitted const> const results = Step(subject, static_cast<void const*>(self), seed);
		self->event    = 0;
		self->distance = 0;
		return results;
	}
	std::span<emitted const> Step(instance& subject, void const* self, uint64_t seed)
	{
		std::span<emitted const> const results = tabular::Step(subject, self, seed);
		log.Record(subject.id(), results);
		return results;
	}
};

event Message(std::string_view type, std::span<cell const> arguments = {})
{
	return event{Hash(type), arguments};
}

// ---- The worked example ------------------------------------------------------------
//
// The spec's grazer, keyed on a host-filled event, with an exit table: entering
// a state starts its animation (grazer_enter), leaving it aborts it
// (grazer_exit). food_found carries the food's distance, which the host
// fills as the distance input: near food is eaten, far food is noted and the
// grazer keeps walking. An event no row keys on falls through to the blank
// run. A cornered check in panic raises when the host
// object is broken.

constexpr std::string_view kGrazer = R"(
[table grazer_brain
  (input energy double 0..1)
  (input food_near flags)
  (input distance double 0..10)
  (key mode hash (domain rows))
  (key event hash fallthrough)
  (column energy band (input energy))
  (column near flags (input food_near))
  (column dist band (input distance))
  (column when condition)
  (column next hash)
  (order mode event)]
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
  (input event hash)
  (input distance double 0..10)]
list,from,where,given,set,emit,overrides
entry,grazer_enter,mode=state.mode,,,command,
step,grazer_brain,mode=state.mode event=input.event,energy=input.energy food_near=input.food_near distance=input.distance,mode=next,,
exit,grazer_exit,mode=state.mode,,,command,
)";

constexpr std::string_view kBrainRows = R"(# grazer_brain
mode,  event,      energy, near, dist,  when,     next
idle,  ,           0..0.3, 1,    0..10, ,         eat
idle,  ,           0..1,   0,    0..10, ,         walk
walk,  ,           0..1,   0,    0..10, ,         walk
walk,  food_found, 0..1,   0,    0..2,  ,         eat
walk,  food_found, 0..1,   0,    2..10, ,         walk
eat,   ,           0..1,   0,    0..10, ,         eat
idle,  scared,     0..1,   0,    0..10, ,         panic
walk,  scared,     0..1,   0,    0..10, ,         panic
eat,   scared,     0..1,   0,    0..10, ,         panic
panic, ,           0..1,   0,    0..10, cornered, idle
panic, ,           0..1,   0,    0..10, ,         panic
panic, calm,       0..1,   0,    0..10, ,         idle
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

}

TEST(GrazerEffects, EveryEntryAndExitIsReturnedInOrder)
{
	effects_fixture f(kGrazer, {kBrainRows, kEnterRows, kExitRows});
	ASSERT_TRUE(f.binding);
	host_object host;
	instance grazer = f.Create(host);
	EXPECT_EQ(f.log.Take(), (std::vector<std::string>{"1:((anim stand))"})) << "creation enters idle and leaves nothing";
	EXPECT_EQ(grazer.Answer(trigger::Exit, 0).outcome, clause_outcome::NotRun);

	f.Step(grazer, &host, 1);   // no event: idle's blank run, energy 0.8, no food
	EXPECT_EQ(grazer.State(0).i, Hash("walk"));
	EXPECT_EQ(f.log.Take(), (std::vector<std::string>{"2:((abort stand))", "1:((anim walk))"}))
		<< "exit, keyed on the idle being left, then entry";
	EXPECT_FALSE(grazer.Answer(trigger::Step, 0).fellThrough) << "no event: the blank run itself";

	f.Step(grazer, &host, 2, Message("sneeze"));
	EXPECT_EQ(grazer.State(0).i, Hash("walk")) << "sneeze fell through to walk's blank run";
	EXPECT_TRUE(grazer.Answer(trigger::Step, 0).fellThrough) << "walk has no sneeze run";
	EXPECT_TRUE(f.log.Take().empty()) << "nothing was entered, so nothing was emitted";
	EXPECT_EQ(grazer.Answer(trigger::Exit, 0).outcome, clause_outcome::NotRun);

	cell const far[] = {cell::Real(5)};
	f.Step(grazer, &host, 3, Message("food_found", far));
	EXPECT_EQ(grazer.State(0).i, Hash("walk")) << "far food: noted, still walking";
	EXPECT_FALSE(grazer.Answer(trigger::Step, 0).fellThrough) << "walk has a food_found run, and it answered";
	EXPECT_TRUE(f.log.Take().empty());

	cell const near[] = {cell::Real(1)};
	f.Step(grazer, &host, 4, Message("food_found", near));
	EXPECT_EQ(grazer.State(0).i, Hash("eat"));
	EXPECT_EQ(f.log.Take(), (std::vector<std::string>{"2:((abort walk))", "1:((anim chew) (message food \"eat\"))"}));

	f.Step(grazer, &host, 5, Message("scared"));
	EXPECT_EQ(grazer.State(0).i, Hash("panic"));
	EXPECT_EQ(f.log.Take(), (std::vector<std::string>{"2:((abort chew) (drop food))",
		"1:((anim run) (message this \"calm\" :delay random(0.4, 1.0)))"}));

	f.Step(grazer, &host, 6, Message("calm"));
	EXPECT_EQ(grazer.State(0).i, Hash("idle"));
	EXPECT_EQ(f.log.Take(), (std::vector<std::string>{"2:((abort run))", "1:((anim stand))"}));
	for (effect_log::command const& c : f.log.commands)
	{
		EXPECT_EQ(c.instance, 7u);
	}
	EXPECT_TRUE(f.log.reports.empty());
}

TEST(GrazerEffects, ARaisingConditionHoldsAndIsReportedOnce)
{
	effects_fixture f(kGrazer, {kBrainRows, kEnterRows, kExitRows});
	ASSERT_TRUE(f.binding);
	host_object host;
	instance grazer = f.Create(host);
	f.Step(grazer, &host, 1, Message("scared"));
	ASSERT_EQ(grazer.State(0).i, Hash("panic"));
	f.log.Take();

	host.broken = true;
	f.Step(grazer, &host, 2);
	clause_answer const first = grazer.Answer(trigger::Step, 0);
	ASSERT_EQ(first.outcome, clause_outcome::Failed);
	EXPECT_EQ(grazer.State(0).i, Hash("panic")) << "the brain held";
	EXPECT_TRUE(f.log.Take().empty()) << "a held clause emits nothing, and nothing was entered";
	ASSERT_EQ(f.log.reports.size(), 1u);
	effect_log::report const& report = f.log.reports[0];
	EXPECT_EQ(report.instance, 7u);
	EXPECT_EQ(report.table, uint32_t(Brain));
	EXPECT_EQ(report.row, first.row) << "the row whose condition raised";
	EXPECT_TRUE(Mentions(report.error, "machine 'grazer' step clause 0 (table 'grazer_brain')")) << report.error;
	EXPECT_TRUE(Mentions(report.error, "table 'grazer_brain' row " + std::to_string(first.row))) << report.error;
	EXPECT_TRUE(Mentions(report.error, "dead target")) << report.error;

	f.Step(grazer, &host, 3);
	EXPECT_EQ(grazer.Answer(trigger::Step, 0).outcome, clause_outcome::Failed);
	EXPECT_EQ(f.log.reports.size(), 1u) << "the same (clause, row) is reported once";

	instance another = f.Create(host, 8);
	f.Step(another, &host, 1, Message("scared"));
	f.Step(another, &host, 2);
	ASSERT_EQ(f.log.reports.size(), 2u) << "the memory is the instance's, not the binding's";
	EXPECT_EQ(f.log.reports[1].instance, 8u);
}

// ---- Fall-through keys ------------------------------------------------------------------

TEST(FallThroughKey, ABlankCellIsAllowedOnlyInAFallThroughKey)
{
	effects_fixture f(kGrazer, {kBrainRows, kEnterRows, kExitRows});
	ASSERT_TRUE(f.machine);
	auto blank = f.store.Value(Brain, 0, 1);
	ASSERT_TRUE(blank.has_value()) << RefusalOf(blank);
	EXPECT_EQ(blank->i, 0) << "idle's first row, event blank";
	// The same blank in mode is refused, as every other blank value cell is.
	csv_rows load;
	load.table     = Brain;
	load.owner     = 2;
	load.text      = "mode,event,energy,near,dist,when,next\n,scared,0..1,0,0..10,,idle\n";
	load.label     = "more.csv";
	load.condition = &Conditions;
	std::string const error = RefusalOf(LoadCsv(f.store, load));
	EXPECT_TRUE(Mentions(error, "column 'mode' is empty")) << error;
	EXPECT_TRUE(Mentions(error, "more.csv:2")) << error;
}

TEST(FallThroughKey, ALoadDemandsTheBlankRunOfEveryTupleOfTheOtherKeys)
{
	auto doc = ReadDocument(kGrazer, "machine.tabular");
	ASSERT_TRUE(doc.has_value()) << RefusalOf(doc);
	relation_store store(std::move(doc->tables));
	// walk keys on food_found and has no blank run.
	csv_rows load;
	load.table     = Brain;
	load.owner     = 1;
	load.label     = "brain.csv";
	load.condition = &Conditions;
	std::string const noBlank = Replace(kBrainRows, "walk,  ,           0..1,   0,    0..10, ,         walk\n", "");
	load.text = noBlank;
	std::string const error = RefusalOf(LoadCsv(store, load));
	EXPECT_TRUE(Mentions(error, "table 'grazer_brain' key (" + std::to_string(Hash("walk"))
		+ ", 0): energy 0, distance 0 has no row with no flags and no script condition; no row has this key, and the key "
		"is in the table's domain")) << error;

	// A run keyed on a value is in the domain because it is there, so it is
	// checked: food_found's distance bands leave a gap.
	std::string const gap = Replace(kBrainRows, "walk,  food_found, 0..1,   0,    2..10, ,         walk\n", "");
	load.text = gap;
	std::string const gapError = RefusalOf(LoadCsv(store, load));
	EXPECT_TRUE(Mentions(gapError, "table 'grazer_brain' key (" + std::to_string(Hash("walk")) + ", "
		+ std::to_string(Hash("food_found")) + "): energy 0, distance 2 has no row")) << gapError;
}

TEST(FallThroughKey, ARunKeyedOnAValueWhoseOtherKeysLeftTheDomainIsAnOrphan)
{
	// mode closes over members without sleep: sleep's runs, blank or typed,
	// are outside the domain, reported and unchecked.
	std::string const tables = Replace(kGrazer, "(key mode hash (domain rows))",
		"(key mode hash (domain (members idle walk eat panic)))");
	auto doc = ReadDocument(tables, "machine.tabular");
	ASSERT_TRUE(doc.has_value()) << RefusalOf(doc);
	relation_store store(std::move(doc->tables));
	std::vector<run_note> report;
	csv_rows load;
	load.table     = Brain;
	load.owner     = 1;
	load.label     = "brain.csv";
	load.condition = &Conditions;
	std::string const rows = std::string(kBrainRows) + "sleep, scared, 0..0.5, 0, 0..10, , idle\n";
	load.text   = rows;
	load.report = &report;
	ASSERT_EQ(RefusalOf(LoadCsv(store, load)), "(accepted)");
	ASSERT_EQ(report.size(), 1u);
	EXPECT_EQ(report[0].status, run_status::Orphan);
	EXPECT_EQ(report[0].key[0].i, Hash("sleep"));
	EXPECT_EQ(report[0].key[1].i, Hash("scared"));
}

namespace
{

// The grazer with one edit, as a definition-build refusal.
std::string BuildRefusal(std::string_view from, std::string_view to)
{
	return RefusalOf(ReadDocument(Replace(kGrazer, from, to), "machine.tabular"));
}

}

// Tabular has no message concept: a machine declares no message, and every
// input is a getter's.
TEST(FallThroughKey, AMachineDeclaresNoMessage)
{
	std::string const error = BuildRefusal("  (input distance double 0..10)]",
		"  (input distance double 0..10)\n  (message (type event))]");
	EXPECT_TRUE(Mentions(error, "message")) << error;
	EXPECT_NE(error, "(accepted)") << "an unknown key fails the document";
}

// Any hash source may feed a fall-through key: a value with no run falls
// through whatever it came from, so the key is open to the proof either way.
TEST(FallThroughKey, AnyHashSourceMayFeedIt)
{
	std::string const source = Replace(kGrazer, "event=input.event", "event=(literal scared)");
	effects_fixture f(source, {kBrainRows, kEnterRows, kExitRows});
	ASSERT_TRUE(f.binding);
	host_object host;
	instance grazer = f.Create(host);
	f.Step(grazer, &host, 1);
	EXPECT_EQ(grazer.State(0).i, Hash("panic")) << "the literal scared keys idle's scared run";
	f.Step(grazer, &host, 2);
	EXPECT_EQ(grazer.State(0).i, Hash("panic")) << "panic has no scared run, so it falls through to the blank run";
	EXPECT_TRUE(grazer.Answer(trigger::Step, 0).fellThrough);
}

TEST(FallThroughKey, ItDeclaresNoOtherDomain)
{
	std::string const both = BuildRefusal("(key event hash fallthrough)",
		"(key event hash (domain rows) fallthrough)");
	EXPECT_TRUE(Mentions(both, "column 'event' of table 'grazer_brain' falls through and declares a domain")) << both;
}

TEST(FallThroughKey, IsAHashKeyAndATableHasAtMostEight)
{
	std::string const flags = RefusalOf(ReadDocument(R"(
[table t
  (key m int fallthrough)
  (column v int)
  (order m)]
)", "t.tabular"));
	EXPECT_TRUE(Mentions(flags, "key 'm' of table 't' is a int; a fall-through key is a hash")) << flags;
	// Several give way in key order (Part 4); a miss may try every
	// combination of blanks, so their number is capped.
	auto keys = [](int n)
	{
		std::string columns;
		std::string order;
		for (int k = 0; k < n; ++k)
		{
			std::string const name = "k" + std::to_string(k);
			columns += "  (key " + name + " hash fallthrough)\n";
			order += (k ? " " : "") + name;
		}
		return RefusalOf(ReadDocument("[table t\n" + columns + "  (column v int)\n  (order " + order + ")]\n", "t.tabular"));
	};
	EXPECT_EQ(keys(2), "(accepted)");
	EXPECT_EQ(keys(8), "(accepted)");
	std::string const nine = keys(9);
	EXPECT_TRUE(Mentions(nine, "key 'k8' of table 't' is fall-through key 9; a table has at most 8")) << nine;
}

TEST(FallThroughKey, OnlyABestTablesKeyFallsThrough)
{
	std::string const weighted = RefusalOf(ReadDocument(R"(
[table t
  (key a hash fallthrough)
  (column w double)
  (pick weighted w)
  (order a)]
)", "t.tabular"));
	EXPECT_TRUE(Mentions(weighted, "key 'a' of table 't' falls through, and only a best table's key falls through"))
		<< weighted;
	EXPECT_FALSE(Mentions(weighted, "declares a domain")) << "the author declared no domain: " << weighted;
}

// ---- Messages out of Tabular ---------------------------------------------------------

namespace
{

// A handler machine: the host dispatches each collision to it and steps it
// once, and its inputs are the collision's payload, read by getters from the
// message. Tabular sees an ordinary machine being stepped.
constexpr std::string_view kCollision = R"(
[table contact_kind
  (input ny double -1..1)
  (input speed double 0..50)
  (column ny band (input ny))
  (column speed band (input speed))
  (column kind hash)
  (suffix command)]
[machine on_collision
  (state contact hash (members none floor wall ceiling) (initial none))
  (input ny double -1..1)
  (input speed double 0..50)]
list,from,where,given,set,emit,overrides
step,contact_kind,,ny=input.ny speed=input.speed,contact=kind,command,
)";

constexpr std::string_view kContactRows = R"(ny,        speed,  kind,    command
0.7..1,    20..50, floor,   sound thud
0.7..1,    0..50,  floor,   ()
-0.7..0.7, 0..50,  wall,    ()
-1..-0.7,  0..50,  ceiling, ()
)";

}

TEST(MessageHandler, ACollisionHandlerClassifiesTheContactFromItsPayload)
{
	effects_fixture f(kCollision, {kContactRows});
	ASSERT_TRUE(f.binding);
	host_object host;
	instance handler = f.Create(host);
	f.log.Take();
	auto collide = [&](double ny, double speed)
	{
		host.ny    = ny;
		host.speed = speed;
		f.Step(handler, &host, 1);
		return f.log.Take();
	};
	EXPECT_EQ(collide(0.9, 5), (std::vector<std::string>{"0:()"}));
	EXPECT_EQ(handler.State(0).i, Hash("floor"));
	EXPECT_EQ(collide(0.0, 30), (std::vector<std::string>{"0:()"}));
	EXPECT_EQ(handler.State(0).i, Hash("wall")) << "a wall is not a floor, however hard the hit";
	EXPECT_EQ(collide(-0.9, 5), (std::vector<std::string>{"0:()"}));
	EXPECT_EQ(handler.State(0).i, Hash("ceiling"));
	EXPECT_EQ(collide(0.9, 30), (std::vector<std::string>{"0:((sound thud))"})) << "a hard landing sounds";
	EXPECT_EQ(handler.State(0).i, Hash("floor"));
}

namespace
{

// The craft bench's Back as a gambit list (commands-and-messages spec,
// "first match wins"): the host fills kind for the step an argument-free
// event arrives, a value with no run of its own falls through to the blank
// run, and Back's rows are tried in row order through script conditions.
constexpr std::string_view kBench = R"(
[table bench_page
  (key page hash (domain rows))
  (key kind hash fallthrough)
  (column when condition)
  (column next_page hash)
  (order page kind)
  (suffix command)]
[machine craft_bench
  (state page hash (members experiment closed) (initial experiment))
  (input event hash)]
list,from,where,given,set,emit,overrides
step,bench_page,page=state.page kind=input.event,,page=next_page,command,
)";

constexpr std::string_view kBenchRows = R"(page,       kind, when,      next_page,  command
experiment, ,     ,          experiment, ()
experiment, back, sort_open, experiment, toggle-sort
experiment, back, can_back,  experiment, basket-back
experiment, back, ,          closed,     close
closed,     ,     ,          closed,     ()
)";

}

TEST(MessageHandler, TheCraftBenchsBackIsAGambitOverAFallThroughKey)
{
	effects_fixture f(kBench, {kBenchRows});
	ASSERT_TRUE(f.binding);
	host_object host;
	instance bench = f.Create(host);
	f.log.Take();
	host.sort_open = true;
	host.can_back  = true;
	f.Step(bench, &host, 1, Message("back"));
	EXPECT_EQ(f.log.Take(), (std::vector<std::string>{"0:(toggle-sort)"})) << "the first line that matches wins";
	host.sort_open = false;
	f.Step(bench, &host, 2, Message("back"));
	EXPECT_EQ(f.log.Take(), (std::vector<std::string>{"0:(basket-back)"}));
	f.Step(bench, &host, 3, Message("select"));
	EXPECT_EQ(f.log.Take(), (std::vector<std::string>{"0:()"})) << "select has no run here: the blank run";
	EXPECT_TRUE(bench.Answer(trigger::Step, 0).fellThrough);
	host.can_back = false;
	f.Step(bench, &host, 4, Message("back"));
	EXPECT_EQ(f.log.Take(), (std::vector<std::string>{"0:(close)"}));
	EXPECT_EQ(bench.State(0).i, Hash("closed"));
}

// Closing is not a transition: the host ends the instance from any state, and
// Close runs the exit list against that state and returns its commands.
TEST(MachineClose, CloseRunsTheExitListAndReturnsItsCommands)
{
	effects_fixture f(kGrazer, {kBrainRows, kEnterRows, kExitRows});
	ASSERT_TRUE(f.binding);
	host_object host;
	instance grazer = f.Create(host);
	f.Step(grazer, &host, 1);
	ASSERT_EQ(grazer.State(0).i, Hash("walk"));
	f.log.Take();
	clause_answer const step = grazer.Answer(trigger::Step, 0);
	std::span<emitted const> const closed = Close(grazer, &host, 2);
	ASSERT_EQ(closed.size(), 1u);
	EXPECT_EQ(closed[0].list, trigger::Exit);
	EXPECT_EQ(closed[0].table, uint32_t(Exit));
	EXPECT_EQ(closed[0].command, "((abort walk))") << "the abort of what walk started";
	EXPECT_EQ(grazer.Emitted().data(), closed.data()) << "the same span as Emitted()";
	EXPECT_EQ(grazer.Answer(trigger::Exit, 0).outcome, clause_outcome::Answered);
	EXPECT_EQ(grazer.Answer(trigger::Entry, 0).outcome, clause_outcome::Answered) << "entry did not run again";
	EXPECT_EQ(grazer.Answer(trigger::Step, 0).row, step.row) << "nor did the step list";
	EXPECT_EQ(grazer.State(0).i, Hash("walk")) << "exit writes no hash state";
}

TEST(MachineClose, CloseAllocatesNothing)
{
	if (!kCountsAllocations)
	{
		GTEST_SKIP() << "a sanitizer owns operator new in this build, so nothing counts allocations";
	}
	effects_fixture f(kGrazer, {kBrainRows, kEnterRows, kExitRows});
	ASSERT_TRUE(f.binding);
	host_object host;
	instance grazer = f.Create(host);
	Close(grazer, &host, 1);
	AllocationCount count;
	for (int i = 0; i < 20; ++i)
	{
		Close(grazer, &host, uint64_t(i));
	}
	EXPECT_EQ(count.Count(), 0u);
}

// ---- The host's rules for handlers -------------------------------------------------------

namespace
{

// A handler whose entry and exit read its payload: the getter is asked at
// creation and at close, with no message in flight, and answers the neutral
// value then (the host's rule), which those lists see.
constexpr std::string_view kNudge = R"(
[table nudge
  (input ny double -1..1)
  (column ny band (input ny))
  (suffix command)]
[machine on_nudge
  (state mode hash (members still) (initial still))
  (input ny double -1..1)]
list,from,where,given,set,emit,overrides
entry,nudge,,ny=input.ny,,command,
exit,nudge,,ny=input.ny,,command,
)";

constexpr std::string_view kNudgeRows = R"(ny,         command
-1..-0.1,   (down)
-0.1..0.1,  (neutral)
0.1..1,     (up)
)";

cell ReadNyCounted(void* data, void const* self, ErrorContext*)
{
	++*static_cast<int*>(data);
	return cell::Real(Host(self).ny);
}

}

TEST(HandlerRules, CreateAndCloseReadThePayloadThroughItsGetter)
{
	auto doc = ReadDocument(kNudge, "nudge.tabular");
	ASSERT_TRUE(doc.has_value()) << RefusalOf(doc);
	relation_store store(std::move(doc->tables));
	csv_rows load;
	load.table = 0;
	load.owner = 1;
	load.text  = kNudgeRows;
	load.label = "nudge.csv";
	ASSERT_EQ(RefusalOf(LoadCsv(store, load)), "(accepted)");
	auto machine = Prepare(doc->machines()[0], store);
	ASSERT_TRUE(machine.has_value()) << RefusalOf(machine);
	int reads = 0;
	host_binding host;
	host.resolve = [&reads](std::string_view) -> tl::expected<input_getter, ErrorContext>
	{
		return input_getter{&ReadNyCounted, &reads};
	};
	auto binding = Bind(*machine, std::move(host));
	ASSERT_TRUE(binding.has_value()) << RefusalOf(binding);

	host_object object;   // no collision in flight: ny is the neutral 0
	instance handler = CreateInstance(**binding, 1, 1, &object);
	EXPECT_EQ(reads, 1) << "creation reads the payload through its getter";
	ASSERT_EQ(handler.Emitted().size(), 1u);
	EXPECT_EQ(handler.Emitted()[0].command, "((neutral))") << "entry sees the neutral value the getter answers";

	object.ny = 0.9;   // a dispatch: the host fills the payload for one step
	Step(handler, &object, 2);
	object.ny = 0;     // and resets it after
	EXPECT_EQ(reads, 2);

	std::span<emitted const> const closed = Close(handler, &object, 3);
	EXPECT_EQ(reads, 3) << "close reads the payload through its getter too";
	ASSERT_EQ(closed.size(), 1u);
	EXPECT_EQ(closed[0].command, "((neutral))") << "exit sees the neutral value, not the last message's";
}

TEST(HandlerRules, AnEventInputTheHostLeavesSetFiresItsRunAgain)
{
	// "Held for one step" is the host's: Tabular reads what the getter
	// answers, so an event left set is a second event.
	effects_fixture f(kBench, {kBenchRows});
	ASSERT_TRUE(f.binding);
	host_object host;
	instance bench = f.Create(host);
	f.log.Take();
	host.can_back = true;
	host.event    = Hash("back");   // filled, and never reset
	f.Step(bench, &host, 1);
	EXPECT_EQ(f.log.Take(), (std::vector<std::string>{"0:(basket-back)"}));
	f.Step(bench, &host, 2);
	EXPECT_EQ(f.log.Take(), (std::vector<std::string>{"0:(basket-back)"})) << "the run fires again";
	host.event = 0;
	f.Step(bench, &host, 3);
	EXPECT_EQ(f.log.Take(), (std::vector<std::string>{"0:()"})) << "reset, the blank run answers";
}

// ---- A fall-through key fed by another's ------------------------------------------------

namespace
{

// mood's fall-through key reads what the brain's fall-through key answered:
// the brain's blank run answers 0, and 0 looks up mood's blank run itself.
constexpr std::string_view kMoodChain = R"(
[table brain
  (key mode hash (domain rows))
  (key event hash fallthrough)
  (column next hash)
  (order mode event)]
[table mood
  (key kind hash fallthrough)
  (column v int)
  (order kind)]
[machine moody
  (state mode hash (members idle panic) (initial idle))
  (state mood int (initial 0))
  (input event hash)]
list,from,where,given,set,emit,overrides
step,brain,mode=state.mode event=input.event,,mode=next,,
step,mood,kind=clause.0.event,,mood=v,,
)";

constexpr std::string_view kMoodBrainRows = R"(mode,  event,  next
idle,  ,       idle
idle,  scared, panic
panic, ,       panic
panic, calm,   idle
)";

constexpr std::string_view kMoodRows = R"(kind,   v
,       1
scared, 2
)";

}

TEST(FallThroughKey, ABlankAnswerFromAnEarlierClauseLooksUpTheBlankRun)
{
	effects_fixture f(kMoodChain, {kMoodBrainRows, kMoodRows});
	ASSERT_TRUE(f.binding);
	host_object host;
	instance moody = f.Create(host);
	f.Step(moody, &host, 1, Message("scared"));
	EXPECT_EQ(moody.State(0).i, Hash("panic"));
	EXPECT_EQ(moody.State(1).i, 2) << "the brain answered scared, which mood has a run for";
	EXPECT_FALSE(moody.Answer(trigger::Step, 1).fellThrough);

	f.Step(moody, &host, 2, Message("wave"));
	EXPECT_TRUE(moody.Answer(trigger::Step, 0).fellThrough) << "wave has no brain run: the blank one answers";
	EXPECT_EQ(moody.Answer(trigger::Step, 1).outcome, clause_outcome::Answered);
	EXPECT_EQ(moody.State(1).i, 1) << "the blank run's 0 is mood's blank run";
	EXPECT_FALSE(moody.Answer(trigger::Step, 1).fellThrough) << "0 is the blank itself, not a value that fell through";

	f.Step(moody, &host, 3, Message("calm"));
	EXPECT_EQ(moody.State(0).i, Hash("idle"));
	EXPECT_EQ(moody.State(1).i, 1) << "calm has no mood run: mood falls through on its own";
	EXPECT_TRUE(moody.Answer(trigger::Step, 1).fellThrough);
}

// ---- Close after a rebind, and on a fall-through exit ---------------------------------------

TEST(MachineClose, CloseAfterARebindRunsTheNewBindingsExit)
{
	effects_fixture f(kGrazer, {kBrainRows, kEnterRows, kExitRows});
	ASSERT_TRUE(f.binding);
	host_object host;
	instance grazer = f.Create(host);
	f.Step(grazer, &host, 1);
	ASSERT_EQ(grazer.State(0).i, Hash("walk"));
	auto rebound = Bind(f.machine, LoggingHost(f.log));
	ASSERT_TRUE(rebound.has_value()) << RefusalOf(rebound);
	Rebind(grazer, **rebound, &host, 2);
	f.binding = nullptr;   // the host drops the old binding at once
	std::span<emitted const> const closed = Close(grazer, &host, 3);
	ASSERT_EQ(closed.size(), 1u);
	EXPECT_EQ(closed[0].command, "((abort walk))");
	EXPECT_EQ(&grazer.binding(), rebound->get()) << "closed under the binding it was moved onto";
}

namespace
{

// An exit keyed on a fall-through event too: what the host fills at close
// picks the abort, and an event no row names falls through to the blank run.
constexpr std::string_view kEventExitRows = R"(mode,  event,  command
idle,  ,       (abort stand)
walk,  ,       (abort walk)
walk,  scared, (abort walk) (flinch)
eat,   ,       (abort chew)
panic, ,       (abort run)
)";

}

TEST(MachineClose, CloseOnAFallThroughExitFallsThroughAndIsRecordedAsClose)
{
	std::string const source = Replace(Replace(kGrazer, R"([table grazer_exit
  (key mode hash (domain rows))
  (order mode)
  (suffix command)])", R"([table grazer_exit
  (key mode hash (domain rows))
  (key event hash fallthrough)
  (order mode event)
  (suffix command)])"), "exit,grazer_exit,mode=state.mode,,,command,",
		"exit,grazer_exit,mode=state.mode event=input.event,,,command,");
	effects_fixture f(source, {kBrainRows, kEnterRows, kEventExitRows});
	ASSERT_TRUE(f.binding);
	host_object host;
	instance grazer = f.Create(host);
	f.Step(grazer, &host, 1);
	ASSERT_EQ(grazer.State(0).i, Hash("walk"));
	host.event = Hash("wave");
	std::span<emitted const> const closed = Close(grazer, &host, 2);
	ASSERT_EQ(closed.size(), 1u);
	EXPECT_EQ(closed[0].command, "((abort walk))") << "wave has no exit run: the blank one answers";
	EXPECT_TRUE(grazer.Answer(trigger::Exit, 0).fellThrough);
	host.event = Hash("scared");
	EXPECT_EQ(Close(grazer, &host, 3)[0].command, "((abort walk) (flinch))");
#if TABULAR_PROVENANCE
	provenance_view const ring = grazer.Provenance();
	ASSERT_GE(ring.size(), 2u);
	provenance_entry const& fell = ring[ring.size() - 2];
	EXPECT_EQ(fell.run, provenance_run::Close);
	EXPECT_EQ(fell.list, trigger::Exit);
	EXPECT_TRUE(fell.fellThrough);
	ASSERT_EQ(fell.Keys().size(), 2u);
	EXPECT_EQ(fell.Keys()[1].i, Hash("wave")) << "the key as read, not the blank it retried";
	provenance_entry const& hit = ring[ring.size() - 1];
	EXPECT_EQ(hit.run, provenance_run::Close);
	EXPECT_FALSE(hit.fellThrough);
	EXPECT_EQ(hit.pass, fell.pass + 1) << "each close is a pass";
#endif
}

// ---- Exit ---------------------------------------------------------------------------

namespace
{

// go moves mode; leave reads the state being left and writes data (an int);
// enter reads the state entered and writes data too, after leave.
constexpr std::string_view kDoor = R"(
[table go
  (key mode hash (domain rows))
  (column next hash)
  (order mode)]
[table leave
  (key mode hash (domain rows))
  (column code int)
  (order mode)
  (suffix say)]
[table enter
  (key mode hash (domain rows))
  (column code int)
  (order mode)
  (suffix say)]
[machine door
  (state mode hash (members a b c) (initial a))
  (state left int (initial 0))
  (state last int (initial 0))]
list,from,where,given,set,emit,overrides
entry,enter,mode=state.mode,,last=code,say,
step,go,mode=state.mode,,mode=next,,
exit,leave,mode=state.mode,,left=code last=code,say,
)";

constexpr std::string_view kGoRows    = "mode,next\na,b\nb,c\nc,c\n";
constexpr std::string_view kLeaveRows = "mode,code,say\na,1,(left a)\nb,2,(left b)\nc,3,(left c)\n";
constexpr std::string_view kEnterRows2 = "mode,code,say\na,10,(at a)\nb,20,(at b)\nc,30,(at c)\n";

}

TEST(MachineExit, ExitReadsTheStateBeingLeftAndEntryWritesLast)
{
	effects_fixture f(kDoor, {kGoRows, kLeaveRows, kEnterRows2});
	ASSERT_TRUE(f.binding);
	host_object host;
	instance door = f.Create(host);
	EXPECT_EQ(door.State(1).i, 0) << "creation runs no exit";
	EXPECT_EQ(door.State(2).i, 10);
	EXPECT_EQ(f.log.Take(), (std::vector<std::string>{"2:((at a))"}));

	f.Step(door, &host, 1);   // a -> b
	EXPECT_EQ(door.State(0).i, Hash("b"));
	EXPECT_EQ(door.State(1).i, 1) << "exit picked a's row: it read current, the state being left";
	EXPECT_EQ(door.State(2).i, 20) << "both wrote last; entry ran after exit and its write won";
	EXPECT_EQ(f.log.Take(), (std::vector<std::string>{"1:((left a))", "2:((at b))"}));

	f.Step(door, &host, 2);   // b -> c
	EXPECT_EQ(door.State(1).i, 2);
	EXPECT_EQ(f.log.Take(), (std::vector<std::string>{"1:((left b))", "2:((at c))"}));

	f.Step(door, &host, 3);   // c -> c: nothing left, nothing entered
	EXPECT_EQ(door.Answer(trigger::Exit, 0).outcome, clause_outcome::NotRun);
	EXPECT_EQ(door.Answer(trigger::Entry, 0).outcome, clause_outcome::NotRun);
	EXPECT_EQ(door.State(1).i, 2);
	EXPECT_TRUE(f.log.Take().empty());
}

TEST(MachineExit, AnExitClauseMayNotSetAHashState)
{
	std::string const error = RefusalOf(ReadDocument(Replace(kDoor, "left=code last=code",
		"mode=mode left=code"), "machine.tabular"));
	EXPECT_TRUE(Mentions(error, "machine 'door' exit clause 0 (table 'leave')")) << error;
	EXPECT_TRUE(Mentions(error, "an exit clause sets hash state column 'mode'")) << error;
	EXPECT_TRUE(Mentions(error, "a transition belongs in the step list")) << error;
}

// ---- Results -------------------------------------------------------------------------

namespace
{

// Every list emits: start on creation, tick every step (keyed on the mode the
// step list moved to), leave and enter on a change.
constexpr std::string_view kTalker = R"(
[table go
  (key mode hash (domain rows))
  (column next hash)
  (order mode)]
[table words
  (key mode hash (domain rows))
  (order mode)
  (suffix say)]
[machine talker (state mode hash (members a b) (initial a))]
list,from,where,given,set,emit,overrides
create,words,mode=(literal start),,,say,
entry,words,mode=state.mode,,,say,
step,go,mode=state.mode,,mode=next,,
step,words,mode=state.mode,,,say,
exit,words,mode=(literal leave),,,say,
)";

constexpr std::string_view kTalkGo    = "mode,next\na,b\nb,a\n";
constexpr std::string_view kTalkWords = "mode,say\na,(in a)\nb,(in b)\nstart,(start)\nleave,(leave)\n";

}

TEST(MachineResults, CommandsComeBackInTheOrderTheListsRan)
{
	effects_fixture f(kTalker, {kTalkGo, kTalkWords});
	ASSERT_TRUE(f.binding);
	host_object host;
	instance talker = f.Create(host);
	EXPECT_EQ(f.log.Take(), (std::vector<std::string>{"1:((start))", "1:((in a))"})) << "create, then entry";
	f.Step(talker, &host, 1);
	EXPECT_EQ(f.log.Take(), (std::vector<std::string>{"1:((in b))", "1:((leave))", "1:((in b))"}))
		<< "the step list (reading the b it moved to), then exit, then entry";
}

// The span a step returns is the instance's Emitted(), which is how creation's
// commands are read; it holds the last run only.
TEST(MachineResults, EmittedHoldsTheLastRunsCommands)
{
	effects_fixture f(kTalker, {kTalkGo, kTalkWords});
	ASSERT_TRUE(f.binding);
	host_object host;
	instance talker = CreateInstance(*f.binding, 1, 1, &host);
	ASSERT_EQ(talker.Emitted().size(), 2u);
	EXPECT_EQ(talker.Emitted()[0].command, "((start))");
	std::span<emitted const> const stepped = Step(talker, &host, 1);
	EXPECT_EQ(stepped.data(), talker.Emitted().data());
	ASSERT_EQ(stepped.size(), 3u);
	EXPECT_EQ(stepped[2].command, "((in b))");
	EXPECT_EQ(talker.State(0).i, Hash("b")) << "the state is current when the results come back";
}

TEST(MachineResults, AHeldClauseEmitsNothing)
{
	// The careful row's condition raises when the host is broken: the clause
	// holds with that row as its answer's row, and its command is not sent.
	effects_fixture f(R"(
[table say
  (key mode hash (domain rows))
  (column when condition)
  (order mode)
  (suffix words)]
[machine speaker (state mode hash (members a) (initial a))]
list,from,where,given,set,emit,overrides
step,say,mode=state.mode,,,words,
)", {"mode,when,words\na,cornered,(careful)\na,,(plain)\n"});
	ASSERT_TRUE(f.binding);
	host_object host;
	instance speaker = f.Create(host);
	f.Step(speaker, &host, 1);
	EXPECT_EQ(f.log.Take(), (std::vector<std::string>{"0:((plain))"}));
	host.broken = true;
	f.Step(speaker, &host, 2);
	clause_answer const held = speaker.Answer(trigger::Step, 0);
	ASSERT_EQ(held.outcome, clause_outcome::Failed);
	EXPECT_TRUE(f.log.Take().empty());
	EXPECT_EQ(f.log.reports.size(), 1u);
}

// Commands are returned, so a machine that emits needs nothing from the host
// to take them.
TEST(MachineBind, AMachineThatEmitsBindsWithNoHostFunctions)
{
	effects_fixture f(kTalker, {kTalkGo, kTalkWords});
	ASSERT_TRUE(f.machine);
	EXPECT_EQ(RefusalOf(Bind(f.machine, host_binding{})), "(accepted)");
}

// ---- Reports ------------------------------------------------------------------------

namespace
{

// A tally keyed by an int input: a value with no run holds the clause. An or
// clause reads t through a band and raises when the host is broken; a best
// clause keyed on its answer cascades its hold.
constexpr std::string_view kCounter = R"(
[table tally
  (key level int (domain rows))
  (column next int)
  (order level)]
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
  (column next int)
  (order bits)]
[machine counter
  (state mode hash (members a) (initial a))
  (state count int (initial 0))
  (input level int)
  (input t double 0..1)]
list,from,where,given,set,emit,overrides
step,tally,level=input.level,,count=next,,
step,phase,mode=state.mode,t=input.t,,,
step,lookup,bits=clause.1.bits,,count=next,,
)";

constexpr std::string_view kTallyRows  = "level,next\n0,1\n1,2\n";
constexpr std::string_view kPhaseRows  = "mode,t,when,bits\na,0..1,,1\na,0..1,cornered,2\n";
constexpr std::string_view kLookupRows = "bits,next\n1,10\n3,30\n";

}

TEST(MachineReport, AValueWithNoRunIsReportedOncePerClause)
{
	effects_fixture f(kCounter, {kTallyRows, kPhaseRows, kLookupRows});
	ASSERT_TRUE(f.binding);
	host_object host;
	instance counter = f.Create(host);
	f.log.reports.clear();
	host.level = 5;
	f.Step(counter, &host, 1);
	EXPECT_EQ(counter.Answer(trigger::Step, 0).outcome, clause_outcome::NoRun);
	host.level = 6;
	f.Step(counter, &host, 2);
	ASSERT_EQ(f.log.reports.size(), 1u) << "a second value with no run is the same clause's NoRun";
	effect_log::report const& report = f.log.reports[0];
	EXPECT_EQ(report.table, 0u);
	EXPECT_EQ(report.row, kNoRow);
	EXPECT_TRUE(Mentions(report.error, "machine 'counter' step clause 0 (table 'tally')")) << report.error;
	EXPECT_TRUE(Mentions(report.error, "table 'tally' key (5) has no run; the value is outside the domain Prepare proved "
		"the clause over")) << report.error;
}

TEST(MachineReport, AGetterThatFailsIsReportedOnceAndItsCascadeNever)
{
	effects_fixture f(kCounter, {kTallyRows, kPhaseRows, kLookupRows});
	ASSERT_TRUE(f.binding);
	host_object host;
	instance counter = f.Create(host);
	host.unwired = true;
	f.Step(counter, &host, 1);
	f.Step(counter, &host, 2);
	EXPECT_EQ(counter.Answer(trigger::Step, 0).outcome, clause_outcome::SourceHeld);
	ASSERT_EQ(f.log.reports.size(), 1u);
	effect_log::report const& report = f.log.reports[0];
	EXPECT_EQ(report.table, kNoTable);
	EXPECT_EQ(report.row, kNoRow);
	EXPECT_TRUE(Mentions(report.error, "machine 'counter' input 'level'")) << report.error;
	EXPECT_TRUE(Mentions(report.error, "no wire")) << report.error;
}

TEST(MachineReport, AnOrClausesRaisingRowIsReportedAndTheReaderHoldsUnreported)
{
	effects_fixture f(kCounter, {kTallyRows, kPhaseRows, kLookupRows});
	ASSERT_TRUE(f.binding);
	host_object host;
	instance counter = f.Create(host);
	host.broken = true;
	f.Step(counter, &host, 1);
	f.Step(counter, &host, 2);
	clause_answer const phase = counter.Answer(trigger::Step, 1);
	ASSERT_EQ(phase.outcome, clause_outcome::Failed);
	EXPECT_EQ(counter.Answer(trigger::Step, 2).outcome, clause_outcome::SourceHeld);
	ASSERT_EQ(f.log.reports.size(), 1u);
	effect_log::report const& report = f.log.reports[0];
	EXPECT_EQ(report.table, 1u);
	EXPECT_EQ(report.row, phase.row);
	EXPECT_TRUE(Mentions(report.error, "machine 'counter' step clause 1 (table 'phase')")) << report.error;
	EXPECT_TRUE(Mentions(report.error, "table 'phase' row " + std::to_string(phase.row))) << report.error;
	EXPECT_TRUE(Mentions(report.error, "dead target")) << report.error;
}

TEST(MachineReport, ANanInputIsReportedOncePerClauseAndInput)
{
	effects_fixture f(kCounter, {kTallyRows, kPhaseRows, kLookupRows});
	ASSERT_TRUE(f.binding);
	host_object host;
	instance counter = f.Create(host);
	host.t = std::numeric_limits<double>::quiet_NaN();
	f.Step(counter, &host, 1);
	f.Step(counter, &host, 2);
	clause_answer const phase = counter.Answer(trigger::Step, 1);
	ASSERT_EQ(phase.outcome, clause_outcome::Failed);
	EXPECT_EQ(phase.input, 0u);
	EXPECT_EQ(phase.row, UINT32_MAX);
	ASSERT_EQ(f.log.reports.size(), 1u);
	EXPECT_EQ(f.log.reports[0].row, kNoRow);
	EXPECT_TRUE(Mentions(f.log.reports[0].error, "input 't' of 'phase' is NaN; a ranged input is clamped into its range"))
		<< f.log.reports[0].error;
	EXPECT_TRUE(Mentions(f.log.reports[0].error, "machine 'counter' step clause 1 (table 'phase')"))
		<< f.log.reports[0].error;
}

TEST(MachineReport, WithNoReportTheHoldsStillHold)
{
	effects_fixture f(kCounter, {kTallyRows, kPhaseRows, kLookupRows});
	ASSERT_TRUE(f.machine);
	auto bound = Bind(f.machine, [&]()
	{
		host_binding h = LoggingHost(f.log);
		h.report       = nullptr;
		return h;
	}());
	ASSERT_TRUE(bound.has_value()) << RefusalOf(bound);
	host_object host;
	host.level      = 5;
	instance counter = CreateInstance(**bound, 1, 1, &host);
	f.Step(counter, &host, 1);
	EXPECT_EQ(counter.Answer(trigger::Step, 0).outcome, clause_outcome::NoRun);
	EXPECT_EQ(counter.State(1).i, 10) << "lookup still wrote; tally held";
}

// ---- Allocation --------------------------------------------------------------------

TEST(GrazerEffects, StepsThatEmitFallThroughExitAndRepeatReportsAllocateNothing)
{
	if (!kCountsAllocations)
	{
		GTEST_SKIP() << "a sanitizer owns operator new in this build, so nothing counts allocations";
	}
	effects_fixture f(kGrazer, {kBrainRows, kEnterRows, kExitRows});
	ASSERT_TRUE(f.binding);
	host_object host;
	instance    grazer = f.Create(host);
	cell const  near[] = {cell::Real(1)};
	cell const  nan[]  = {cell::Real(std::numeric_limits<double>::quiet_NaN())};
	// A round of eight steps: walk, a sneeze that falls through, near food,
	// scared, a broken corner check that raises, a NaN distance, calm.
	auto round = [&](int i)
	{
		host.broken = false;
		switch (i % 8)
		{
		case 0: f.Step(grazer, &host, 1); break;
		case 1: f.Step(grazer, &host, 2, Message("sneeze")); break;
		case 2: f.Step(grazer, &host, 3, Message("food_found", near)); break;
		case 3: f.Step(grazer, &host, 4, Message("scared")); break;
		case 4: host.broken = true; f.Step(grazer, &host, 5); break;
		case 5: f.Step(grazer, &host, 6, Message("calm", nan)); break;
		case 6: f.Step(grazer, &host, 7, Message("calm")); break;
		default: f.Step(grazer, &host, 8); break;
		}
	};
	// The first round reports the raise and the NaN, building their errors.
	for (int i = 0; i < 8; ++i)
	{
		round(i);
	}
	ASSERT_EQ(f.log.reportCalls, 2) << "the raise and the NaN";
	f.log.record     = false;
	int const commands = f.log.commandCount;
	AllocationCount count;
	for (int i = 0; i < 80; ++i)
	{
		round(i);
	}
	EXPECT_EQ(count.Count(), 0u);
	EXPECT_EQ(f.log.reportCalls, 2) << "repeats are not reported";
	EXPECT_EQ(f.log.commandCount - commands, 10 * 8) << "four changes a round, each an exit and an entry";
}

TEST(FallThroughKey, APivotedTablesBlankCellIsTheBlank)
{
	auto doc = ReadDocument(R"(
[table lookup
  (input mode flags)
  (key label int (domain rows))
  (key msg hash fallthrough)
  (column req flags (input mode))
  (column clip string)
  (order label msg)
  (pivot req clip)]
)", "pivot.tabular");
	ASSERT_TRUE(doc.has_value()) << RefusalOf(doc);
	relation_store store(std::move(doc->tables));
	csv_rows rows;
	rows.table   = 0;
	rows.owner   = 1;
	rows.text    = "label,msg,None,A\n1,,plain,plain_a\n1,hit,hurt,hurt_a\n";
	rows.label   = "lookup.csv";
	rows.pivoted = true;
	ASSERT_EQ(RefusalOf(LoadCsv(store, rows)), "(accepted)");
	auto clip = [&](int64_t type) -> std::string
	{
		cell const keys[]   = {cell::Integer(1), cell::Integer(type)};
		cell const inputs[] = {cell::Integer(0)};
		auto picked = PickBest(store, 0, pick_query{keys, inputs});
		EXPECT_TRUE(picked.has_value() && *picked) << RefusalOf(picked);
		if (!picked || !*picked)
		{
			return "";
		}
		auto text = store.StringAt(0, **picked, 3);
		return text ? std::string(std::string_view(*text)) : std::string();
	};
	EXPECT_EQ(clip(0), "plain");
	EXPECT_EQ(clip(Hash("hit")), "hurt");
}

// ---- One evaluation, the host's error ------------------------------------------------

TEST(MachineReport, AConditionIsAskedOncePerEvaluationAndItsOwnErrorIsReported)
{
	// Each flaky condition fails the first time it is asked and holds after,
	// so asking it again to build the report would see it answer.
	effects_fixture f(R"(
[table choose
  (key mode hash (domain rows))
  (column when condition)
  (column v int)
  (order mode)]
[table bits
  (key mode hash)
  (column when condition)
  (column flags flags)
  (pick or flags)
  (order mode)]
[machine asker
  (state mode hash (members a) (initial a))
  (state count int (initial 0))]
list,from,where,given,set,emit,overrides
step,choose,mode=state.mode,,count=v,,
step,bits,mode=state.mode,,,,
)", {"mode,when,v\na,flaky,1\na,,2\n", "mode,when,flags\na,flaky_or,1\na,,2\n"});
	ASSERT_TRUE(f.binding);
	host_object host;
	instance asker = f.Create(host);
	f.Step(asker, &host, 1);
	EXPECT_EQ(host.flakyCalls, 1) << "the best clause's condition was asked once";
	EXPECT_EQ(host.flakyOrCalls, 1) << "the or clause's condition was asked once";
	ASSERT_EQ(asker.Answer(trigger::Step, 0).outcome, clause_outcome::Failed);
	ASSERT_EQ(asker.Answer(trigger::Step, 1).outcome, clause_outcome::Failed);
	ASSERT_EQ(f.log.reports.size(), 2u);
	EXPECT_TRUE(Mentions(f.log.reports[0].error, "first failure")) << f.log.reports[0].error;
	EXPECT_TRUE(Mentions(f.log.reports[0].error, "table 'choose' row 0")) << f.log.reports[0].error;
	EXPECT_TRUE(Mentions(f.log.reports[0].error, "machine 'asker' step clause 0 (table 'choose')")) << f.log.reports[0].error;
	EXPECT_TRUE(Mentions(f.log.reports[1].error, "first failure")) << f.log.reports[1].error;
	EXPECT_TRUE(Mentions(f.log.reports[1].error, "table 'bits' row 0")) << f.log.reports[1].error;
	EXPECT_TRUE(Mentions(f.log.reports[1].error, "machine 'asker' step clause 1 (table 'bits')")) << f.log.reports[1].error;

	f.Step(asker, &host, 2);
	EXPECT_EQ(host.flakyCalls, 2);
	EXPECT_EQ(host.flakyOrCalls, 2);
	EXPECT_EQ(asker.Answer(trigger::Step, 0).outcome, clause_outcome::Answered);
	EXPECT_EQ(asker.State(1).i, 1) << "the flaky row holds now";
	EXPECT_EQ(asker.Answer(trigger::Step, 1).bits, 3u);
	EXPECT_EQ(f.log.reports.size(), 2u);
}

// ---- The fall-through is the table's ---------------------------------------------------

TEST(FallThroughKey, PickBestFallsThroughToTheBlankRunToo)
{
	effects_fixture f(kGrazer, {kBrainRows, kEnterRows, kExitRows});
	ASSERT_TRUE(f.machine);
	host_object host;
	cell const inputs[] = {cell::Real(0.8), cell::Integer(0), cell::Real(5)};
	auto pick = [&](int64_t type) -> std::optional<size_t>
	{
		cell const keys[] = {cell::Integer(Hash("walk")), cell::Integer(type)};
		auto picked = PickBest(f.store, Brain, pick_query{keys, inputs, &host});
		EXPECT_TRUE(picked.has_value()) << RefusalOf(picked);
		return picked ? *picked : std::nullopt;
	};
	std::optional<size_t> const blank = pick(0);
	ASSERT_TRUE(blank.has_value());
	EXPECT_EQ(pick(Hash("sneeze")), blank) << "walk has no sneeze run; the pick answers from walk's blank run";
	std::optional<size_t> const food = pick(Hash("food_found"));
	ASSERT_TRUE(food.has_value());
	EXPECT_NE(*food, *blank) << "a value with a run of its own answers from it";
}

// ---- A fall-through key's 0 stays in fall-through keys ----------------------------------

TEST(FallThroughKey, AColumnDomainFromOneHoldsItsValuesWithoutTheBlank)
{
	auto doc = ReadDocument(R"(
[table a
  (key msg hash fallthrough)
  (column v int)
  (order msg)]
[table b
  (key msg hash (domain (from a msg)))
  (column w int)
  (order msg)]
)", "ab.tabular");
	ASSERT_TRUE(doc.has_value()) << RefusalOf(doc);
	relation_store store(std::move(doc->tables));
	auto load = [&](uint32_t table, uint64_t owner, std::string_view text)
	{
		csv_rows rows;
		rows.table = table;
		rows.owner = owner;
		rows.text  = text;
		rows.label = "rows.csv";
		return RefusalOf(LoadCsv(store, rows));
	};
	EXPECT_EQ(load(0, 1, "msg,v\n,0\n"), "(accepted)") << "a's blank run alone: b's domain has no value yet";
	EXPECT_EQ(load(1, 1, "msg,w\nwalk,1\nsneeze,2\n"), "(accepted)");
	EXPECT_EQ(load(0, 2, "msg,v\nwalk,1\nsneeze,2\n"), "(accepted)")
		<< "b is total over a's values; a's blank is the blank run's, not a value b could key";
	std::string const gap = load(0, 3, "msg,v\nhop,3\n");
	EXPECT_TRUE(Mentions(gap, "table 'b'")) << "b has no hop run: " << gap;
}

TEST(FallThroughKey, AnOrdinaryKeyMayNotReadAnEarlierClausesFallThroughKey)
{
	// mood keys on what the brain's fall-through key answered: the brain
	// answers 0 from its blank run, which no ordinary key can hold.
	std::string const tables = Replace(kGrazer, "[machine grazer", R"([table mood
  (key kind hash (domain rows))
  (column v int)
  (order kind)]
[machine grazer)");
	std::string const reader = Replace(tables, "mode=next,,\nexit,", "mode=next,,\nstep,mood,kind=clause.0.event,,,,\nexit,");
	std::string const error = RefusalOf(ReadDocument(reader, "machine.tabular"));
	EXPECT_TRUE(Mentions(error, "machine 'grazer' step clause 1 (table 'mood')")) << error;
	EXPECT_TRUE(Mentions(error, "key 'kind' reads column 'event' of clause 0, the fall-through key of table 'grazer_brain'"))
		<< error;
	EXPECT_TRUE(Mentions(error, "its blank run")) << error;

	// Into a fall-through key the blank is a value it holds: 0 looks up the
	// blank run itself.
	std::string const intoFallThrough = Replace(Replace(tables, "(key kind hash (domain rows))",
		"(key kind hash fallthrough)"), "mode=next,,\nexit,", "mode=next,,\nstep,mood,kind=clause.0.event,,,,\nexit,");
	EXPECT_EQ(RefusalOf(ReadDocument(intoFallThrough, "machine.tabular")), "(accepted)");
}

// ---- Report memory per scripted row -----------------------------------------------------

TEST(MachineReport, EachScriptedRowAmongManyIsReportedOnce)
{
	// Two scripted rows, one at each end of a table of unscripted ones: each
	// has its own report bit, and the rows between have none.
	std::string rows = "level,when,next\n0,cornered,100\n0,,0\n";
	for (int n = 1; n <= 40; ++n)
	{
		rows += std::to_string(n) + ",," + std::to_string(n) + "\n";
	}
	rows += "41,cornered,141\n41,,41\n";
	effects_fixture f(R"(
[table tally
  (key level int (domain rows))
  (column when condition)
  (column next int)
  (order level)]
[machine levels
  (state mode hash (members a) (initial a))
  (state count int (initial 0))
  (input level int)]
list,from,where,given,set,emit,overrides
step,tally,level=input.level,,count=next,,
)", {rows});
	ASSERT_TRUE(f.binding);
	host_object host;
	host.broken      = true;
	instance levels  = f.Create(host);
	f.log.reports.clear();
	for (int round = 0; round < 3; ++round)
	{
		host.level = 0;
		f.Step(levels, &host, 1);
		host.level = 20;
		f.Step(levels, &host, 2);
		EXPECT_EQ(levels.State(1).i, 20) << "an unscripted row answers";
		host.level = 41;
		f.Step(levels, &host, 3);
	}
	ASSERT_EQ(f.log.reports.size(), 2u) << "one report per scripted row, however often each fails";
	EXPECT_EQ(f.log.reports[0].row, 0u);
	EXPECT_EQ(f.log.reports[1].row, 42u);
	EXPECT_TRUE(Mentions(f.log.reports[1].error, "table 'tally' row 42")) << f.log.reports[1].error;
}

// ---- NoRun names the key as it came -----------------------------------------------------

TEST(MachineReport, ANoRunOnAFallThroughKeyNamesTheValueNotTheBlank)
{
	effects_fixture f(R"(
[table poke
  (key level int (domain rows))
  (key msg hash fallthrough)
  (column next int)
  (order level msg)]
[machine poker
  (state mode hash (members a) (initial a))
  (state count int (initial 0))
  (input level int)
  (input event hash)]
list,from,where,given,set,emit,overrides
step,poke,level=input.level msg=input.event,,count=next,,
)", {"level,msg,next\n0,,1\n0,poke,2\n"});
	ASSERT_TRUE(f.binding);
	host_object host;
	instance poker = f.Create(host);
	f.log.reports.clear();
	host.level = 5;
	f.Step(poker, &host, 1, Message("poke"));
	EXPECT_EQ(poker.Answer(trigger::Step, 0).outcome, clause_outcome::NoRun) << "(5, poke) and its blank twin (5, 0) miss";
	ASSERT_EQ(f.log.reports.size(), 1u);
	EXPECT_TRUE(Mentions(f.log.reports[0].error, "table 'poke' key (5, " + std::to_string(Hash("poke")) + ") has no run"))
		<< f.log.reports[0].error;
}

// ---- Steps that find no run allocate nothing ----------------------------------------------

TEST(MachineReport, RepeatedNoRunsAndGetterFailuresWithAReportAllocateNothing)
{
	if (!kCountsAllocations)
	{
		GTEST_SKIP() << "a sanitizer owns operator new in this build, so nothing counts allocations";
	}
	effects_fixture f(kCounter, {kTallyRows, kPhaseRows, kLookupRows});
	ASSERT_TRUE(f.binding);
	host_object host;
	instance counter = f.Create(host);
	// The first of each builds its report.
	host.level = 5;
	f.Step(counter, &host, 1);
	host.unwired = true;
	f.Step(counter, &host, 2);
	ASSERT_EQ(f.log.reportCalls, 2) << "the missing run and the getter";
	f.log.record = false;
	AllocationCount count;
	for (int i = 0; i < 40; ++i)
	{
		host.unwired = i % 2 == 0;
		host.level   = 5 + i;
		f.Step(counter, &host, uint64_t(i));
	}
	EXPECT_EQ(count.Count(), 0u);
	EXPECT_EQ(f.log.reportCalls, 2) << "repeats are not reported";
	EXPECT_EQ(counter.Answer(trigger::Step, 0).outcome, clause_outcome::NoRun);
}
