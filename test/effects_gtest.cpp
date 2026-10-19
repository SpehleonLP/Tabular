#include <gtest/gtest.h>
#include "Support/allocationtestingsupport.h"
#include "tabular/instance.h"
#include "tabular/machine.h"
#include "tabular/pick.h"
#include "tabular/relation.h"
#include "tabular/rows_csv.h"
#include "tabular/schema_yaml.h"
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
// reports once (P15), the message it is given (R3) and the exit list it runs
// when a state is left (R6). The worked example is the spec's grazer (§4)
// with an exit table that aborts what entry started.

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

// A document's tables loaded with rows (one text per table, in table order),
// its first machine prepared and bound to LoggingHost(log).
struct effects_fixture
{
	effect_log                 log;
	schema_document            document;
	relation_store             store;
	counted_ptr<Machine const> machine;
	counted_ptr<Binding const> binding;

	effects_fixture(std::string_view yaml, std::vector<std::string_view> const& rows)
		: document(Parse(yaml)), store(std::move(document.tables))
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

	static schema_document Parse(std::string_view yaml)
	{
		auto doc = DocumentFromYaml(yaml, "machine.yaml");
		EXPECT_TRUE(doc.has_value()) << RefusalOf(doc);
		return doc ? std::move(*doc) : schema_document{};
	}

	instance Create(host_object const& host, uint64_t id = 7)
	{
		instance out = CreateInstance(*binding, id, 1, &host);
		log.Record(out.id(), out.Emitted());
		return out;
	}

	// A step, its commands kept in log.
	std::span<emitted const> Step(instance& subject, void const* self, uint64_t seed, message const& msg = {})
	{
		std::span<emitted const> const results = tabular::Step(subject, self, seed, msg);
		log.Record(subject.id(), results);
		return results;
	}
};

message Message(std::string_view type, std::span<cell const> arguments = {})
{
	return message{static_cast<uint32_t>(Hash(type)), arguments};
}

// ---- The worked example ------------------------------------------------------------
//
// The spec's grazer with a message argument and an exit table: entering a
// state starts its animation (grazer_enter), leaving it aborts it
// (grazer_exit). food_found carries the food's distance: near food is eaten,
// far food is noted and the grazer keeps walking. A type no row keys on falls
// through to the blank run. A cornered check in panic raises when the host
// object is broken.

constexpr std::string_view kGrazer = R"(
tables:
  - name: grazer_brain
    read_only: true
    pick: best
    inputs:
      - {name: energy,    type: double, range: 0..1}
      - {name: food_near, type: flags}
      - {name: distance,  type: double, range: 0..10}
    columns:
      - {name: mode,    type: hash, key: true, domain: rows}
      - {name: message, type: hash, key: true, domain: message}
      - {name: energy,  type: band, input: energy}
      - {name: near,    type: flags, input: food_near}
      - {name: dist,    type: band, input: distance}
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
  - name: grazer_exit
    read_only: true
    pick: best
    columns:
      - {name: mode, type: hash, key: true, domain: rows}
    order_by: {columns: [mode]}
    suffix: command
machines:
  - name: grazer
    state:
      - {name: mode, type: hash, members: [idle, walk, eat, panic], initial: idle}
    inputs:
      - {name: energy,    type: double, range: 0..1}
      - {name: food_near, type: flags}
      - {name: message,   type: hash}
      - {name: distance,  type: double, range: 0..10}
    message: {type: message, arguments: [distance]}
    step:
      - from:  grazer_brain
        where: {mode: state.mode, message: input.message}
        given: {energy: input.energy, food_near: input.food_near, distance: input.distance}
        set:   {mode: next}
    exit:
      - from:  grazer_exit
        where: {mode: state.mode}
        emit:  command
    entry:
      - from:  grazer_enter
        where: {mode: state.mode}
        emit:  command
)";

constexpr std::string_view kBrainRows = R"(# grazer_brain
mode,  message,    energy, near, dist,  when,     next
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

	f.Step(grazer, &host, 1);   // no message: idle's blank run, energy 0.8, no food
	EXPECT_EQ(grazer.State(0).i, Hash("walk"));
	EXPECT_EQ(f.log.Take(), (std::vector<std::string>{"2:((abort stand))", "1:((anim walk))"}))
		<< "exit, keyed on the idle being left, then entry";
	EXPECT_FALSE(grazer.Absorbed());

	f.Step(grazer, &host, 2, Message("sneeze"));
	EXPECT_EQ(grazer.State(0).i, Hash("walk")) << "sneeze fell through to walk's blank run";
	EXPECT_FALSE(grazer.Absorbed()) << "the host routes sneeze to the script";
	EXPECT_TRUE(f.log.Take().empty()) << "nothing was entered, so nothing was emitted";
	EXPECT_EQ(grazer.Answer(trigger::Exit, 0).outcome, clause_outcome::NotRun);

	cell const far[] = {cell::Real(5)};
	f.Step(grazer, &host, 3, Message("food_found", far));
	EXPECT_EQ(grazer.State(0).i, Hash("walk")) << "far food: noted, still walking";
	EXPECT_TRUE(grazer.Absorbed()) << "walk has a food_found run, and it answered";
	EXPECT_TRUE(f.log.Take().empty());

	cell const near[] = {cell::Real(1)};
	f.Step(grazer, &host, 4, Message("food_found", near));
	EXPECT_EQ(grazer.State(0).i, Hash("eat"));
	EXPECT_TRUE(grazer.Absorbed());
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

// ---- Messages ----------------------------------------------------------------------

TEST(MachineMessage, AnArgumentTheMessageDoesNotCarryReadsZero)
{
	effects_fixture f(kGrazer, {kBrainRows, kEnterRows, kExitRows});
	ASSERT_TRUE(f.binding);
	host_object host;
	instance grazer = f.Create(host);
	f.Step(grazer, &host, 1);
	ASSERT_EQ(grazer.State(0).i, Hash("walk"));
	f.Step(grazer, &host, 2, Message("food_found"));
	EXPECT_EQ(grazer.State(0).i, Hash("eat")) << "distance 0 is near";
	grazer = f.Create(host);
	f.Step(grazer, &host, 1);
	cell const two[] = {cell::Real(5), cell::Real(0)};
	f.Step(grazer, &host, 2, Message("food_found", two));
	EXPECT_EQ(grazer.State(0).i, Hash("walk")) << "an argument past the declared ones is not read";
}

TEST(MachineMessage, ABlankCellIsNoMessageOnlyInAMessageKey)
{
	effects_fixture f(kGrazer, {kBrainRows, kEnterRows, kExitRows});
	ASSERT_TRUE(f.machine);
	auto blank = f.store.Value(Brain, 0, 1);
	ASSERT_TRUE(blank.has_value()) << RefusalOf(blank);
	EXPECT_EQ(blank->i, 0) << "idle's first row, message blank";
	// The same blank in mode is refused, as every other blank value cell is.
	csv_rows load;
	load.table     = Brain;
	load.owner     = 2;
	load.text      = "mode,message,energy,near,dist,when,next\n,scared,0..1,0,0..10,,idle\n";
	load.label     = "more.csv";
	load.condition = &Conditions;
	std::string const error = RefusalOf(LoadCsv(f.store, load));
	EXPECT_TRUE(Mentions(error, "column 'mode' is empty")) << error;
	EXPECT_TRUE(Mentions(error, "more.csv:2")) << error;
}

TEST(MachineMessage, ALoadDemandsTheBlankRunOfEveryTupleOfTheOtherKeys)
{
	auto doc = DocumentFromYaml(kGrazer, "machine.yaml");
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

	// A run keyed on a type is in the domain because it is there, so it is
	// checked: food_found's distance bands leave a gap.
	std::string const gap = Replace(kBrainRows, "walk,  food_found, 0..1,   0,    2..10, ,         walk\n", "");
	load.text = gap;
	std::string const gapError = RefusalOf(LoadCsv(store, load));
	EXPECT_TRUE(Mentions(gapError, "table 'grazer_brain' key (" + std::to_string(Hash("walk")) + ", "
		+ std::to_string(Hash("food_found")) + "): energy 0, distance 2 has no row")) << gapError;
}

TEST(MachineMessage, ARunKeyedOnATypeWhoseOtherKeysLeftTheDomainIsAnOrphan)
{
	// mode closes over members without sleep: sleep's runs, blank or typed,
	// are outside the domain, reported and unchecked.
	std::string const tables = Replace(kGrazer, "{name: mode,    type: hash, key: true, domain: rows}",
		"{name: mode,    type: hash, key: true, domain: {members: [idle, walk, eat, panic]}}");
	auto doc = DocumentFromYaml(tables, "machine.yaml");
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
	return RefusalOf(DocumentFromYaml(Replace(kGrazer, from, to), "machine.yaml"));
}

}

TEST(MachineMessage, TheMessageIsAHashInputAndItsArgumentsOtherInputs)
{
	std::string const unknown = BuildRefusal("message: {type: message,", "message: {type: msg,");
	EXPECT_TRUE(Mentions(unknown, "machine 'grazer'")) << unknown;
	EXPECT_TRUE(Mentions(unknown, "the message's type is input 'msg', which the machine does not declare")) << unknown;
	std::string const typed = BuildRefusal("message: {type: message,", "message: {type: energy,");
	EXPECT_TRUE(Mentions(typed, "the message's type is input 'energy', a double; a message's type is a hash")) << typed;
	std::string const missing = BuildRefusal("arguments: [distance]", "arguments: [distance, range]");
	EXPECT_TRUE(Mentions(missing, "message argument 1 is input 'range', which the machine does not declare")) << missing;
	std::string const twice = BuildRefusal("arguments: [distance]", "arguments: [distance, distance]");
	EXPECT_TRUE(Mentions(twice, "message argument 1 is input 'distance', which the message already carries as an argument"))
		<< twice;
	std::string const type = BuildRefusal("arguments: [distance]", "arguments: [message]");
	EXPECT_TRUE(Mentions(type, "message argument 0 is input 'message', which the message already carries as its type"))
		<< type;
	std::string const blank = BuildRefusal("message: {type: message,", "message: {type: \"\",");
	EXPECT_TRUE(Mentions(blank, "the message's type names no input")) << blank;
}

TEST(MachineMessage, ArgumentsWithNoTypeAreRefused)
{
	auto doc = DocumentFromYaml(kGrazer, "machine.yaml");
	ASSERT_TRUE(doc.has_value()) << RefusalOf(doc);
	create_machine           m         = doc->machines()[0];
	std::string_view const   args[]    = {"distance"};
	m.message                          = message_inputs{{}, args};
	std::string const        error     = RefusalOf(CheckMachine(m, doc->tables));
	EXPECT_TRUE(Mentions(error, "the message names arguments and no type")) << error;
}

TEST(MachineMessage, AMessageKeyAndTheMessageTypeAreBoundToEachOther)
{
	std::string const literal = BuildRefusal("message: input.message}", "message: {literal: scared}}");
	EXPECT_TRUE(Mentions(literal, "machine 'grazer' step clause 0 (table 'grazer_brain')")) << literal;
	EXPECT_TRUE(Mentions(literal, "key 'message' of table 'grazer_brain' is a message key; it reads the machine's message type"))
		<< literal;
	// The type bound to a key the table does not mark: a rows key has no blank
	// run to fall through to.
	std::string const unmarked = RefusalOf(DocumentFromYaml(Replace(kGrazer,
		"{name: message, type: hash, key: true, domain: message}", "{name: message, type: hash, key: true, domain: rows}"),
		"machine.yaml"));
	EXPECT_TRUE(Mentions(unmarked, "key 'message' reads the message type, and table 'grazer_brain' does not declare it a "
		"message key (domain: message)")) << unmarked;
	// A machine that takes no message cannot fill a message key.
	std::string const none = BuildRefusal("    message: {type: message, arguments: [distance]}\n", "");
	EXPECT_TRUE(Mentions(none, "is a message key; it reads the machine's message type")) << none;
}

TEST(MachineMessage, AMessageKeyIsOneHashKey)
{
	std::string const flags = RefusalOf(DocumentFromYaml(R"(
tables:
  - name: t
    read_only: true
    pick: best
    columns:
      - {name: m, type: int, key: true, domain: message}
      - {name: v, type: int}
    order_by: {columns: [m]}
)", "t.yaml"));
	EXPECT_TRUE(Mentions(flags, "key 'm' of table 't' is a int; a message key is a hash, the message's type")) << flags;
	std::string const two = RefusalOf(DocumentFromYaml(R"(
tables:
  - name: t
    read_only: true
    pick: best
    columns:
      - {name: a, type: hash, key: true, domain: message}
      - {name: b, type: hash, key: true, domain: message}
      - {name: v, type: int}
    order_by: {columns: [a, b]}
)", "t.yaml"));
	EXPECT_TRUE(Mentions(two, "key 'b' of table 't' is a second message key; a table keys on one message")) << two;
}

// ---- Exit ---------------------------------------------------------------------------

namespace
{

// go moves mode; leave reads the state being left and writes data (an int);
// enter reads the state entered and writes data too, after leave.
constexpr std::string_view kDoor = R"(
tables:
  - name: go
    read_only: true
    pick: best
    columns:
      - {name: mode, type: hash, key: true, domain: rows}
      - {name: next, type: hash}
    order_by: {columns: [mode]}
  - name: leave
    read_only: true
    pick: best
    columns:
      - {name: mode, type: hash, key: true, domain: rows}
      - {name: code, type: int}
    order_by: {columns: [mode]}
    suffix: say
  - name: enter
    read_only: true
    pick: best
    columns:
      - {name: mode, type: hash, key: true, domain: rows}
      - {name: code, type: int}
    order_by: {columns: [mode]}
    suffix: say
machines:
  - name: door
    state:
      - {name: mode, type: hash, members: [a, b, c], initial: a}
      - {name: left, type: int, initial: 0}
      - {name: last, type: int, initial: 0}
    step:
      - from: go
        where: {mode: state.mode}
        set: {mode: next}
    exit:
      - from: leave
        where: {mode: state.mode}
        set: {left: code, last: code}
        emit: say
    entry:
      - from: enter
        where: {mode: state.mode}
        set: {last: code}
        emit: say
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
	std::string const error = RefusalOf(DocumentFromYaml(Replace(kDoor, "set: {left: code, last: code}",
		"set: {mode: mode, left: code}"), "machine.yaml"));
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
tables:
  - name: go
    read_only: true
    pick: best
    columns:
      - {name: mode, type: hash, key: true, domain: rows}
      - {name: next, type: hash}
    order_by: {columns: [mode]}
  - name: words
    read_only: true
    pick: best
    columns:
      - {name: mode, type: hash, key: true, domain: rows}
    order_by: {columns: [mode]}
    suffix: say
machines:
  - name: talker
    state:
      - {name: mode, type: hash, members: [a, b], initial: a}
    create:
      - from: words
        where: {mode: {literal: start}}
        emit: say
    step:
      - from: go
        where: {mode: state.mode}
        set: {mode: next}
      - from: words
        where: {mode: state.mode}
        emit: say
    exit:
      - from: words
        where: {mode: {literal: leave}}
        emit: say
    entry:
      - from: words
        where: {mode: state.mode}
        emit: say
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
tables:
  - name: say
    read_only: true
    pick: best
    columns:
      - {name: mode, type: hash, key: true, domain: rows}
      - {name: when, type: condition}
    order_by: {columns: [mode]}
    suffix: words
machines:
  - name: speaker
    state:
      - {name: mode, type: hash, members: [a], initial: a}
    step:
      - from: say
        where: {mode: state.mode}
        emit: words
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
tables:
  - name: tally
    read_only: true
    pick: best
    columns:
      - {name: level, type: int, key: true, domain: rows}
      - {name: next,  type: int}
    order_by: {columns: [level]}
  - name: phase
    read_only: true
    pick: or
    output: bits
    inputs:
      - {name: t, type: double, range: 0..1}
    columns:
      - {name: mode, type: hash, key: true}
      - {name: t,    type: band, input: t}
      - {name: when, type: condition}
      - {name: bits, type: flags}
    order_by: {columns: [mode]}
  - name: lookup
    read_only: true
    pick: best
    columns:
      - {name: bits, type: flags, key: true, domain: rows}
      - {name: next, type: int}
    order_by: {columns: [bits]}
machines:
  - name: counter
    state:
      - {name: mode,  type: hash, members: [a], initial: a}
      - {name: count, type: int, initial: 0}
    inputs:
      - {name: level, type: int}
      - {name: t,     type: double, range: 0..1}
    step:
      - from: tally
        where: {level: input.level}
        set: {count: next}
      - from: phase
        where: {mode: state.mode}
        given: {t: input.t}
      - from: lookup
        where: {bits: clause.1.bits}
        set: {count: next}
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

TEST(GrazerEffects, StepsThatEmitTakeMessagesExitAndRepeatReportsAllocateNothing)
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

TEST(MachineMessage, APivotedTablesBlankMessageKeyIsNoMessage)
{
	auto doc = DocumentFromYaml(R"(
tables:
  - name: lookup
    read_only: true
    pick: best
    inputs:
      - {name: mode, type: flags}
    columns:
      - {name: label, type: int, key: true, domain: rows}
      - {name: msg,   type: hash, key: true, domain: message}
      - {name: req,   type: flags, input: mode}
      - {name: clip,  type: string}
    order_by: {columns: [label, msg]}
    pivot: {headers: req, cells: clip}
)", "pivot.yaml");
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
tables:
  - name: choose
    read_only: true
    pick: best
    columns:
      - {name: mode, type: hash, key: true, domain: rows}
      - {name: when, type: condition}
      - {name: v,    type: int}
    order_by: {columns: [mode]}
  - name: bits
    read_only: true
    pick: or
    output: flags
    columns:
      - {name: mode,  type: hash, key: true}
      - {name: when,  type: condition}
      - {name: flags, type: flags}
    order_by: {columns: [mode]}
machines:
  - name: asker
    state:
      - {name: mode,  type: hash, members: [a], initial: a}
      - {name: count, type: int, initial: 0}
    step:
      - from: choose
        where: {mode: state.mode}
        set: {count: v}
      - from: bits
        where: {mode: state.mode}
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

TEST(MachineMessage, PickBestFallsThroughToTheBlankRunToo)
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
	EXPECT_NE(*food, *blank) << "a type with a run of its own answers from it";
}

// ---- A message key's 0 stays in message keys -------------------------------------------

TEST(MachineMessage, AColumnDomainFromAMessageKeyHoldsItsTypesWithoutTheBlank)
{
	auto doc = DocumentFromYaml(R"(
tables:
  - name: a
    read_only: true
    pick: best
    columns:
      - {name: msg, type: hash, key: true, domain: message}
      - {name: v,   type: int}
    order_by: {columns: [msg]}
  - name: b
    read_only: true
    pick: best
    columns:
      - {name: msg, type: hash, key: true, domain: {table: a, column: msg}}
      - {name: w,   type: int}
    order_by: {columns: [msg]}
)", "ab.yaml");
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
	EXPECT_EQ(load(0, 1, "msg,v\n,0\n"), "(accepted)") << "a's blank run alone: b's domain has no type yet";
	EXPECT_EQ(load(1, 1, "msg,w\nwalk,1\nsneeze,2\n"), "(accepted)");
	EXPECT_EQ(load(0, 2, "msg,v\nwalk,1\nsneeze,2\n"), "(accepted)")
		<< "b is total over a's real types; a's blank is no message, not a type b could key";
	std::string const gap = load(0, 3, "msg,v\nhop,3\n");
	EXPECT_TRUE(Mentions(gap, "table 'b'")) << "b has no hop run: " << gap;
}

TEST(MachineMessage, ANonMessageKeyMayNotReadAnEarlierClausesMessageKey)
{
	// mood keys on what the brain's message key answered: the brain answers 0
	// from its blank run, which no ordinary key can hold.
	std::string const tables = Replace(kGrazer, "machines:\n", R"(  - name: mood
    read_only: true
    pick: best
    columns:
      - {name: kind, type: hash, key: true, domain: rows}
      - {name: v,    type: int}
    order_by: {columns: [kind]}
machines:
)");
	std::string const reader = Replace(tables, "        set:   {mode: next}\n", R"(        set:   {mode: next}
      - from:  mood
        where: {kind: clause.0.message}
)");
	std::string const error = RefusalOf(DocumentFromYaml(reader, "machine.yaml"));
	EXPECT_TRUE(Mentions(error, "machine 'grazer' step clause 1 (table 'mood')")) << error;
	EXPECT_TRUE(Mentions(error, "key 'kind' reads column 'message' of clause 0, the message key of table 'grazer_brain'"))
		<< error;
	EXPECT_TRUE(Mentions(error, "its blank run")) << error;

	// Into a message key the read is the message key's own rule: it reads the
	// machine's message type, and nothing else.
	std::string const intoMessage = Replace(Replace(tables, "      - {name: kind, type: hash, key: true, domain: rows}\n",
		"      - {name: kind, type: hash, key: true, domain: message}\n"), "        set:   {mode: next}\n", R"(        set:   {mode: next}
      - from:  mood
        where: {kind: clause.0.message}
)");
	std::string const messageError = RefusalOf(DocumentFromYaml(intoMessage, "machine.yaml"));
	EXPECT_TRUE(Mentions(messageError, "key 'kind' of table 'mood' is a message key; it reads the machine's message type"))
		<< messageError;
	EXPECT_FALSE(Mentions(messageError, "its blank run")) << messageError;
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
tables:
  - name: tally
    read_only: true
    pick: best
    columns:
      - {name: level, type: int, key: true, domain: rows}
      - {name: when,  type: condition}
      - {name: next,  type: int}
    order_by: {columns: [level]}
machines:
  - name: levels
    state:
      - {name: mode,  type: hash, members: [a], initial: a}
      - {name: count, type: int, initial: 0}
    inputs:
      - {name: level, type: int}
    step:
      - from: tally
        where: {level: input.level}
        set: {count: next}
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

TEST(MachineReport, ANoRunOnAMessageKeyNamesTheTypeNotTheBlank)
{
	effects_fixture f(R"(
tables:
  - name: poke
    read_only: true
    pick: best
    columns:
      - {name: level, type: int, key: true, domain: rows}
      - {name: msg,   type: hash, key: true, domain: message}
      - {name: next,  type: int}
    order_by: {columns: [level, msg]}
machines:
  - name: poker
    state:
      - {name: mode,  type: hash, members: [a], initial: a}
      - {name: count, type: int, initial: 0}
    inputs:
      - {name: level,   type: int}
      - {name: message, type: hash}
    message: {type: message}
    step:
      - from: poke
        where: {level: input.level, msg: input.message}
        set: {count: next}
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

// ---- Extra message arguments --------------------------------------------------------------

TEST(MachineMessage, ExtraArgumentsAreReportedOnceAndTheStepStillRuns)
{
	effects_fixture f(kGrazer, {kBrainRows, kEnterRows, kExitRows});
	ASSERT_TRUE(f.binding);
	host_object host;
	instance grazer = f.Create(host);
	f.Step(grazer, &host, 1);
	ASSERT_EQ(grazer.State(0).i, Hash("walk"));
	cell const two[] = {cell::Real(5), cell::Real(0)};
	f.Step(grazer, &host, 2, Message("food_found", two));
	EXPECT_EQ(grazer.State(0).i, Hash("walk")) << "the declared argument, distance 5, was read: far food";
	EXPECT_TRUE(grazer.Absorbed());
	ASSERT_EQ(f.log.reports.size(), 1u);
	EXPECT_EQ(f.log.reports[0].table, kNoTable);
	EXPECT_EQ(f.log.reports[0].row, kNoRow);
	EXPECT_TRUE(Mentions(f.log.reports[0].error, "machine 'grazer'")) << f.log.reports[0].error;
	EXPECT_TRUE(Mentions(f.log.reports[0].error, "a message carries 2 arguments, and the machine declares 1"))
		<< f.log.reports[0].error;
	cell const three[] = {cell::Real(5), cell::Real(0), cell::Real(0)};
	f.Step(grazer, &host, 3, Message("food_found", three));
	EXPECT_EQ(f.log.reports.size(), 1u) << "reported once per instance";

	// A missing argument is no failure: it reads 0, as on a step with no message.
	f.Step(grazer, &host, 4, Message("food_found"));
	EXPECT_EQ(grazer.State(0).i, Hash("eat"));
	EXPECT_EQ(f.log.reports.size(), 1u);
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
