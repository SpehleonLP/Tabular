#include <gtest/gtest.h>
#include "Support/allocationtestingsupport.h"
#include "tabular/document.h"
#include "tabular/draw.h"
#include "tabular/instance.h"
#include "tabular/machine.h"
#include "tabular/pick.h"
#include "tabular/relation.h"
#include "tabular/rows_csv.h"
#include "cityhash/src/city.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace tabular;

// Slice 7's evidence. Provenance (P16): each instance's ring of its last picks,
// in debug builds. Parity across the lists: every list a machine runs (create,
// step, exit, entry, and Rebind's exit and entry) answers what the interpreted
// picks answer on the version it holds, so the laid-out step is held to the
// prior version everywhere it runs, not only in the step list.

namespace
{

template<typename T>
std::string RefusalOf(tl::expected<T, ErrorContext> const& e)
{
	return e ? std::string("(accepted)") : e.error().format();
}

int64_t Hash(std::string_view text)
{
	return static_cast<int64_t>(CityHash32(text.data(), text.size()));
}

double const kNaN = std::numeric_limits<double>::quiet_NaN();

struct host_object
{
	double  energy    = 0.8;
	double  t         = 0.5;
	int64_t food_near = 0;
	bool    cornered  = false;
	bool    broken    = false;
	bool    unwired   = false;   // the energy getter cannot answer
	int64_t event     = 0;       // filled by the host for one step: the event it dispatches
};

host_object const& Host(void const* self)
{
	return *static_cast<host_object const*>(self);
}

cell ReadEnergy(void*, void const* self, ErrorContext* error_out)
{
	if (Host(self).unwired)
	{
		*error_out = ErrorContext("no wire");
		return cell::Real(0);
	}
	return cell::Real(Host(self).energy);
}

cell ReadT(void*, void const* self, ErrorContext*)
{
	return cell::Real(Host(self).t);
}

cell ReadEvent(void*, void const* self, ErrorContext*)
{
	return cell::Integer(Host(self).event);
}

cell ReadFoodNear(void*, void const* self, ErrorContext*)
{
	return cell::Integer(Host(self).food_near);
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

bool Panicky(void*, void const* self, ErrorContext* error_out)
{
	if (Host(self).broken)
	{
		*error_out = ErrorContext("dead target");
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

// A store, the document whose tables it holds, and the commands every run of
// its bindings returned (prefixed with the label of the binding whose clause
// emitted it), in order.
struct World
{
	schema_document          document;
	relation_store           store;
	std::vector<std::string> logged;
	bool                     quiet  = false;   // commands are counted instead of logged, so nothing allocates
	size_t                   counted = 0;

	explicit World(std::string_view source) : document(Parse(source)), store(std::move(document.tables)) {}

	static schema_document Parse(std::string_view source)
	{
		auto doc = ReadDocument(source, "evidence.tabular");
		EXPECT_TRUE(doc.has_value()) << RefusalOf(doc);
		return doc ? std::move(*doc) : schema_document{};
	}

	void Load(uint32_t table, std::string_view rows)
	{
		csv_rows load;
		load.table     = table;
		load.owner     = 1;
		load.text      = rows;
		load.label     = "rows.csv";
		load.condition = &Conditions;
		EXPECT_EQ(RefusalOf(LoadCsv(store, load)), "(accepted)") << "table " << table;
	}

	// The document's first machine prepared against the store as it is now,
	// bound to getters for energy, t and food_near, its commands logged as
	// "label:command".
	counted_ptr<Binding const> Bound(std::string const& label)
	{
		if (document.machines().empty())
		{
			return {};
		}
		auto prepared = Prepare(document.machines()[0], store);
		EXPECT_TRUE(prepared.has_value()) << RefusalOf(prepared);
		if (!prepared)
		{
			return {};
		}
		host_binding host;
		host.resolve = [](std::string_view name) -> tl::expected<input_getter, ErrorContext>
		{
			static std::map<std::string_view, cell (*)(void*, void const*, ErrorContext*)> const reads = {
				{"energy", &ReadEnergy}, {"t", &ReadT}, {"food_near", &ReadFoodNear}, {"message", &ReadEvent}};
			auto found = reads.find(name);
			if (found == reads.end())
			{
				return tl::make_unexpected(ErrorContext("no wire named '", name, "'"));
			}
			return input_getter{found->second, nullptr, nullptr};
		};
		auto bound = Bind(*prepared, std::move(host));
		EXPECT_TRUE(bound.has_value()) << RefusalOf(bound);
		if (!bound)
		{
			return {};
		}
		Targets()[bound->get()] = log_target{this, label};
		return std::move(*bound);
	}

	void Log(std::string const& label, emitted const& result)
	{
		if (quiet)
		{
			++counted;
			return;
		}
		logged.push_back(label + ":" + std::string(result.command));
	}

	struct log_target
	{
		World*      world = nullptr;
		std::string label;
	};

	// The world and label each binding's commands go to, set when it binds.
	static std::unordered_map<Binding const*, log_target>& Targets()
	{
		static std::unordered_map<Binding const*, log_target> targets;
		return targets;
	}

	std::vector<std::string> Take()
	{
		std::vector<std::string> out;
		out.swap(logged);
		return out;
	}
};

void Log(Binding const& binding, emitted const& result)
{
	auto const found = World::Targets().find(&binding);
	ASSERT_NE(found, World::Targets().end()) << "a binding no world made";
	found->second.world->Log(found->second.label, result);
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

// An event the host dispatches, by its type's hash (0 for none).
struct posted
{
	int64_t type = 0;
};

posted Message(int64_t type)
{
	return posted{type};
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

std::string CommandOf(TableVersion const& version, size_t row)
{
	uint32_t const column = uint32_t(version.definition().columns.size() - 1);
	auto command = version.CommandAt(row, column);
	EXPECT_TRUE(command.has_value()) << RefusalOf(command);
	return command ? std::string(*command) : std::string();
}


// ---- The life machine: every list, every clause kind ---------------------------------
//
// create draws a mood (weighted). step moves mode with the spec grazer's brain
// (best: a fall-through key, a ranked flags input, a band, two
// script conditions). exit, reading the state being left, picks a best row
// with a band and a script and emits it. entry chains an or (arrive) into a
// best keyed by its answer (settle, which has no run for 4, so it holds there)
// and draws the mood again.

constexpr std::string_view kLife = R"(
[table brain
  (input energy double 0..1)
  (input near flags)
  (order mode message)]
>mode, >message?, energy: band, near: flags, when, next
[table leave
  (input energy double 0..1)
  (order mode)]
>mode, energy: band, when, left: int, command
[table arrive
  (input energy double 0..1)
  (pick or bits)
  (order mode)]
>mode, energy: band, when, bits: flags
[table settle
  (order bits)]
>bits: flags, tone: int
[table mood
  (pick weighted w)
  (order mode)]
>mode, w: double, mood: int
[machine life
  (state mode hash (members idle walk eat panic) (initial idle))
  (state left int (initial 0))
  (state tone int (initial 0))
  (state mood int (initial 0))
  (input energy double 0..1)
  (input food_near flags)
  (input message hash)]
list,from,where,given,set,emit,overrides
create,mood,mode=state.mode,,mood=mood,,
entry,arrive,mode=state.mode,energy=input.energy,,,
entry,settle,bits=clause.0.bits,,tone=tone,,
entry,mood,mode=state.mode,,mood=mood,,
step,brain,mode=state.mode message=input.message,energy=input.energy near=input.food_near,mode=next,,
exit,leave,mode=state.mode,energy=input.energy,left=left,command,
)";

enum life_table : uint32_t { Brain, Leave, Arrive, Settle, Mood };
enum life_state : size_t { Mode, Left, Tone, MoodState };

constexpr std::string_view kBrainRows = R"(mode,  message, energy,   near, when,     next
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

constexpr std::string_view kLeaveRows = R"(mode, energy, when, left, command
idle,  0..0.5, cornered, 11, (leave idle cornered)
idle,  0..1,   ,         10, (leave idle)
walk,  0..1,   ,         20, (leave walk)
eat,   0.5..1, panicky,  31, (leave eat panicky)
eat,   0..1,   ,         30, (leave eat)
panic, 0..1,   ,         40, (leave panic)
)";

// The reload's rows: other cuts and other values, so a pick on the wrong
// version answers something else.
constexpr std::string_view kLeaveRowsV2 = R"(mode, energy, when, left, command
idle,  0.4..1, panicky,  13, (leave2 idle panicky)
idle,  0..1,   ,         12, (leave2 idle)
walk,  0..0.6, cornered, 22, (leave2 walk cornered)
walk,  0..1,   ,         21, (leave2 walk)
eat,   0..1,   ,         32, (leave2 eat)
panic, 0..1,   ,         42, (leave2 panic)
)";

constexpr std::string_view kArriveRows = R"(mode, energy, when, bits
idle, 0..0.5,  ,         1
idle, 0.3..1,  ,         2
walk, 0..1,    cornered, 4
eat,  0..1,    ,         1
eat,  0.7..1,  ,         8
)";

constexpr std::string_view kArriveRowsV2 = R"(mode, energy, when, bits
idle, 0..0.6,  ,         2
walk, 0..1,    ,         1
walk, 0.5..1,  panicky,  8
eat,  0..1,    cornered, 4
panic, 0..1,   ,         1
)";

constexpr std::string_view kSettleRows = "bits,tone\n0,100\n1,101\n2,102\n3,103\n8,108\n9,109\n";
constexpr std::string_view kMoodRows   = "mode,w,mood\nidle,1,1\nidle,3,2\nwalk,1,3\neat,2,4\neat,1,5\npanic,1,6\n";

std::unique_ptr<World> Life()
{
	auto w = std::make_unique<World>(kLife);
	w->Load(Brain, kBrainRows);
	w->Load(Leave, kLeaveRows);
	w->Load(Arrive, kArriveRows);
	w->Load(Settle, kSettleRows);
	w->Load(Mood, kMoodRows);
	return w;
}

// The version a binding's clause was proved against.
counted_ptr<TableVersion const> Current(World const& w, uint32_t table)
{
	auto version = w.store.Version(table);
	EXPECT_TRUE(version.has_value()) << RefusalOf(version);
	return version ? *version : counted_ptr<TableVersion const>{};
}

// The oracle's answer, read as the outcome a clause must have.
template<typename Where>
void ExpectSameAnswer(tl::expected<std::optional<size_t>, ErrorContext> const& oracle, clause_answer got,
	Where const& where)
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

double Probe(std::mt19937_64& rng)
{
	static double const edges[] = {-1.0, 0.0, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 1.0, 2.0,
		double(std::nextafter(0.5f, 0.f)), double(std::nextafter(0.3f, 1.f)), kNaN};
	std::uniform_int_distribution<size_t> pick(0, std::size(edges) + 3);
	size_t const i = pick(rng);
	if (i < std::size(edges))
	{
		return edges[i];
	}
	return std::uniform_real_distribution<double>(-0.2, 1.2)(rng);
}

// What the oracle expects an entry list (arrive -> settle -> mood) to do on
// entering mode, against the versions given: the answers, and the state it
// leaves. Returns the oracle's states for tone and mood (unchanged where a
// clause held).
struct entry_oracle
{
	tl::expected<uint32_t, ErrorContext>               bits = 0u;
	std::optional<tl::expected<std::optional<size_t>, ErrorContext>> settle;
	tl::expected<std::optional<size_t>, ErrorContext>  mood = std::optional<size_t>{};
	int64_t                                            tone = 0;
	int64_t                                            moodValue = 0;
};

entry_oracle EntryOracle(TableVersion const& arrive, TableVersion const& settle, TableVersion const& mood, cell mode,
	host_object const& host, uint64_t seed, int64_t tone, int64_t moodValue)
{
	entry_oracle out;
	std::vector<cell> const keys   = {mode};
	std::vector<cell> const inputs = {cell::Real(host.energy)};
	out.bits = PickOr(arrive, pick_query{keys, inputs, &host});
	out.tone = tone;
	if (out.bits)
	{
		std::vector<cell> const settleKeys = {cell::Integer(*out.bits)};
		out.settle = PickBest(settle, pick_query{settleKeys, {}, &host});
		if (*out.settle && **out.settle)
		{
			out.tone = ValueOf(settle, ***out.settle, "tone").i;
		}
	}
	out.mood      = PickWeighted(mood, pick_query{keys, {}, &host}, seed, DrawSite("life", trigger::Entry, 2));
	out.moodValue = out.mood && *out.mood ? ValueOf(mood, **out.mood, "mood").i : moodValue;
	return out;
}

template<typename Where>
void ExpectEntry(entry_oracle const& oracle, instance const& subject, Where const& where)
{
	clause_answer const arrive = subject.Answer(trigger::Entry, 0);
	if (!oracle.bits)
	{
		EXPECT_EQ(arrive.outcome, clause_outcome::Failed) << where();
		EXPECT_EQ(subject.Answer(trigger::Entry, 1).outcome, clause_outcome::SourceHeld) << where();
	}
	else
	{
		EXPECT_EQ(arrive.outcome, clause_outcome::Answered) << where();
		EXPECT_EQ(arrive.bits, *oracle.bits) << where();
		ExpectSameAnswer(*oracle.settle, subject.Answer(trigger::Entry, 1), where);
	}
	ExpectSameAnswer(oracle.mood, subject.Answer(trigger::Entry, 2), where);
	EXPECT_EQ(subject.State(Tone).i, oracle.tone) << where();
	EXPECT_EQ(subject.State(MoodState).i, oracle.moodValue) << where();
}

}

// ---- Parity across every list -------------------------------------------------------

TEST(ListParity, CreationsCreateAndEntryListsMatchPickWeightedPickOrAndPickBest)
{
	auto w = Life();
	counted_ptr<Binding const> bound = w->Bound("v1");
	ASSERT_TRUE(bound);
	auto mood   = Current(*w, Mood);
	auto arrive = Current(*w, Arrive);
	auto settle = Current(*w, Settle);
	std::mt19937_64 rng(0x11fe);
	int calm = 0;
	for (int i = 0; i < 2000; ++i)
	{
		host_object host;
		host.energy   = Probe(rng);
		host.cornered = rng() % 2 == 0;
		host.broken   = rng() % 20 == 0;
		uint64_t const seed = rng();
		cell const idle     = cell::Integer(Hash("idle"));
		std::vector<cell> const keys = {idle};
		auto const drawn = PickWeighted(*mood, pick_query{keys, {}, &host}, seed, DrawSite("life", trigger::Create, 0));
		instance subject = Created(*bound, uint64_t(i), seed, &host);
		auto where = [&]() { return "creation " + std::to_string(i) + " "; };
		ExpectSameAnswer(drawn, subject.Answer(trigger::Create, 0), where);
		int64_t const created = drawn && *drawn ? ValueOf(*mood, **drawn, "mood").i : 0;
		// Entry runs after create on the next vector, so its draw overwrites
		// the mood create drew; both are checked by their answers.
		entry_oracle const entry = EntryOracle(*arrive, *settle, *mood, idle, host, seed, 0, created);
		ExpectEntry(entry, subject, where);
		EXPECT_EQ(subject.Answer(trigger::Step, 0).outcome, clause_outcome::NotRun) << where();
		EXPECT_EQ(subject.Answer(trigger::Exit, 0).outcome, clause_outcome::NotRun) << where() << "creation leaves nothing";
		calm += created == 1 ? 1 : 0;
		if (::testing::Test::HasFailure())
		{
			break;
		}
	}
	EXPECT_GT(calm, 300) << "idle's weight 1 of 4 is drawn";
	EXPECT_LT(calm, 700);
	EXPECT_TRUE(w->Take().empty()) << "creation runs no exit, and nothing else here emits";
}

TEST(ListParity, StepExitAndEntryListsMatchTheInterpretedPicksStepByStep)
{
	auto w = Life();
	counted_ptr<Binding const> bound = w->Bound("v1");
	ASSERT_TRUE(bound);
	auto brain  = Current(*w, Brain);
	auto leave  = Current(*w, Leave);
	auto arrive = Current(*w, Arrive);
	auto settle = Current(*w, Settle);
	auto mood   = Current(*w, Mood);
	std::mt19937_64 rng(0x5717);
	int64_t const messages[] = {0, Hash("scared"), Hash("calm"), Hash("sneeze"), 0};
	int64_t const flags[]    = {0, 1, 2, 3, 5, 6, 9, 0xFFFFFFFF, int64_t(1) << 32};
	host_object host;
	instance subject = Created(*bound, 1, 1, &host);
	w->Take();
	int entered = 0, exitHeld = 0, settleHeld = 0, fell = 0;
	for (int i = 0; i < 6000; ++i)
	{
		host.energy    = Probe(rng);
		host.food_near = flags[rng() % std::size(flags)];
		host.cornered  = rng() % 2 == 0;
		host.broken    = rng() % 25 == 0;
		int64_t const type = messages[rng() % std::size(messages)];
		uint64_t const seed = rng();
		cell const before[] = {subject.State(Mode), subject.State(Left), subject.State(Tone), subject.State(MoodState)};

		std::vector<cell> const brainKeys   = {before[Mode], cell::Integer(type)};
		std::vector<cell> const brainInputs = {cell::Real(host.energy), cell::Integer(host.food_near)};
		auto const moved = PickBest(*brain, pick_query{brainKeys, brainInputs, &host});
		Stepped(subject, &host, seed, Message(type));
		auto where = [&]() { return "step " + std::to_string(i) + " "; };
		ExpectSameAnswer(moved, subject.Answer(trigger::Step, 0), where);
		cell const next = moved && *moved ? ValueOf(*brain, **moved, "next") : before[Mode];
		EXPECT_EQ(subject.State(Mode).i, next.i) << where();
		fell += type == Hash("sneeze") && moved && *moved ? 1 : 0;

		std::vector<std::string> const logged = w->Take();
		if (next.i == before[Mode].i)
		{
			EXPECT_EQ(subject.Answer(trigger::Exit, 0).outcome, clause_outcome::NotRun) << where();
			EXPECT_EQ(subject.Answer(trigger::Entry, 0).outcome, clause_outcome::NotRun) << where();
			EXPECT_TRUE(logged.empty()) << where();
			EXPECT_EQ(subject.State(Left).i, before[Left].i) << where();
		}
		else
		{
			++entered;
			// Exit reads the state being left.
			std::vector<cell> const leaveKeys   = {before[Mode]};
			std::vector<cell> const leaveInputs = {cell::Real(host.energy)};
			auto const left = PickBest(*leave, pick_query{leaveKeys, leaveInputs, &host});
			ExpectSameAnswer(left, subject.Answer(trigger::Exit, 0), where);
			if (left && *left)
			{
				EXPECT_EQ(subject.State(Left).i, ValueOf(*leave, **left, "left").i) << where();
				EXPECT_EQ(logged, std::vector<std::string>{"v1:" + CommandOf(*leave, **left)}) << where();
			}
			else
			{
				++exitHeld;
				EXPECT_EQ(subject.State(Left).i, before[Left].i) << where();
				EXPECT_TRUE(logged.empty()) << where() << "a held exit emits nothing";
			}
			entry_oracle const entry = EntryOracle(*arrive, *settle, *mood, next, host, seed, before[Tone].i,
				before[MoodState].i);
			ExpectEntry(entry, subject, where);
			settleHeld += entry.bits && entry.settle && !(entry.settle->has_value() && **entry.settle) ? 1 : 0;
		}
		if (::testing::Test::HasFailure())
		{
			break;
		}
	}
	EXPECT_GT(entered, 1000);
	EXPECT_GT(exitHeld, 10) << "the exit's scripted rows raised";
	EXPECT_GT(settleHeld, 20) << "walk's cornered 4 has no settle run";
	EXPECT_GT(fell, 300) << "sneeze fell through to the blank run";
}

TEST(ListParity, RebindsOldExitAndNewEntryMatchThePicksOnTheirOwnVersions)
{
	auto w = Life();
	counted_ptr<Binding const> current = w->Bound("v0");
	ASSERT_TRUE(current);
	std::mt19937_64 rng(0x4eb1);
	int64_t const messages[] = {0, Hash("scared"), Hash("calm")};
	std::string currentLabel = "v0";
	host_object host;
	instance subject = Created(*current, 1, 1, &host);
	int rebinds = 0, exitHeld = 0;
	for (int i = 0; i < 3000; ++i)
	{
		host.energy    = Probe(rng);
		host.food_near = int64_t(rng() % 10);
		host.cornered  = rng() % 2 == 0;
		host.broken    = rng() % 15 == 0;
		if (i % 7 != 6)
		{
			Stepped(subject, &host, rng(), Message(messages[rng() % std::size(messages)]));
			continue;
		}
		// A reload: the exit and arrive rows change, every few times back and
		// forth, and the instance moves to the new binding.
		w->Take();
		auto oldLeave = Current(*w, Leave);
		bool const second = (i / 7) % 2 == 0;
		w->Load(Leave, second ? kLeaveRowsV2 : kLeaveRows);
		w->Load(Arrive, second ? kArriveRowsV2 : kArriveRows);
		std::string const label = "v" + std::to_string(i);
		counted_ptr<Binding const> next = w->Bound(label);
		ASSERT_TRUE(next);
		auto newArrive = Current(*w, Arrive);
		auto settle    = Current(*w, Settle);
		auto mood      = Current(*w, Mood);
		cell const before[] = {subject.State(Mode), subject.State(Left), subject.State(Tone), subject.State(MoodState)};
		uint64_t const seed = rng();

		std::vector<cell> const leaveKeys   = {before[Mode]};
		std::vector<cell> const leaveInputs = {cell::Real(host.energy)};
		auto const left = PickBest(*oldLeave, pick_query{leaveKeys, leaveInputs, &host});
		Rebound(subject, *next, &host, seed);
		auto where = [&]() { return "rebind at " + std::to_string(i) + " "; };
		std::vector<std::string> const logged = w->Take();
		// The old exit, against the old version, tagged with the old binding.
		std::vector<std::string> expected;
		if (left && *left)
		{
			expected.push_back(currentLabel + ":" + CommandOf(*oldLeave, **left));
			EXPECT_EQ(subject.State(Left).i, ValueOf(*oldLeave, **left, "left").i) << where() << "the old exit's write, migrated";
		}
		else
		{
			++exitHeld;
			EXPECT_EQ(subject.State(Left).i, before[Left].i) << where();
		}
		// The life machine's entry emits nothing, so the old binding's abort is all.
		EXPECT_EQ(logged, expected) << where();
		// The new entry, against the new versions, on the migrated state.
		EXPECT_EQ(subject.State(Mode).i, before[Mode].i) << where() << "every member survives";
		entry_oracle const entry = EntryOracle(*newArrive, *settle, *mood, before[Mode], host, seed, before[Tone].i,
			before[MoodState].i);
		ExpectEntry(entry, subject, where);
		EXPECT_EQ(subject.Answer(trigger::Step, 0).outcome, clause_outcome::NotRun) << where();
		current      = next;
		currentLabel = label;
		++rebinds;
		if (::testing::Test::HasFailure())
		{
			break;
		}
	}
	EXPECT_GT(rebinds, 400);
	EXPECT_GT(exitHeld, 10) << "the old exit's scripted rows raised";
}

// ---- Provenance ---------------------------------------------------------------------

#if TABULAR_PROVENANCE

namespace
{

// One clause the ring should hold: which run, list and clause.
struct expected_pick
{
	uint64_t pass;
	trigger  list;
	uint32_t clause;
};

}

TEST(Provenance, TheRingKeepsTheLastPicksOldestFirstAcrossCreationStepsAndRebinds)
{
	auto w = Life();
	counted_ptr<Binding const> v1 = w->Bound("v1");
	ASSERT_TRUE(v1);
	host_object host;
	host.energy = 0.9;
	instance subject = Created(*v1, 1, 1, &host);
	// Creation: create's one clause, then entry's three.
	std::vector<expected_pick> expected = {{0, trigger::Create, 0}, {0, trigger::Entry, 0}, {0, trigger::Entry, 1},
		{0, trigger::Entry, 2}};
	ASSERT_EQ(subject.Provenance().size(), expected.size());
	std::mt19937_64 rng(0x9a55);
	for (uint64_t pass = 1; pass <= 40; ++pass)
	{
		host.energy    = Probe(rng);
		host.food_near = int64_t(rng() % 10);
		cell const before = subject.State(Mode);
		if (pass % 9 == 0)
		{
			Rebound(subject, *v1, &host, pass);
			expected.push_back({pass, trigger::Exit, 0});
			for (uint32_t c = 0; c < 3; ++c)
			{
				expected.push_back({pass, trigger::Entry, c});
			}
			continue;
		}
		Stepped(subject, &host, pass, Message(pass % 4 == 0 ? Hash("scared") : 0));
		expected.push_back({pass, trigger::Step, 0});
		if (subject.State(Mode).i != before.i)
		{
			expected.push_back({pass, trigger::Exit, 0});
			for (uint32_t c = 0; c < 3; ++c)
			{
				expected.push_back({pass, trigger::Entry, c});
			}
		}
	}
	provenance_view const ring = subject.Provenance();
	ASSERT_EQ(ring.size(), kProvenanceDepth) << "the ring is full and has wrapped";
	ASSERT_GT(expected.size(), 2 * kProvenanceDepth);
	size_t const first = expected.size() - kProvenanceDepth;
	size_t       k     = 0;
	for (provenance_entry const& entry : ring)
	{
		expected_pick const& want = expected[first + k];
		EXPECT_EQ(entry.pass, want.pass) << "entry " << k;
		EXPECT_EQ(entry.list, want.list) << "entry " << k;
		EXPECT_EQ(entry.clause, want.clause) << "entry " << k;
		EXPECT_EQ(entry.run, want.pass % 9 == 0 ? provenance_run::Rebind : provenance_run::Step) << "entry " << k;
		++k;
	}
	EXPECT_EQ(k, kProvenanceDepth);
}

TEST(Provenance, AnEntryHoldsTheTableRowAndTheValuesTheStepRead)
{
	auto w = Life();
	counted_ptr<Binding const> v1 = w->Bound("v1");
	ASSERT_TRUE(v1);
	host_object host;
	host.energy    = 0.2;
	host.food_near = 1;
	instance subject = Created(*v1, 1, 1, &host);
	ASSERT_EQ(subject.State(Mode).i, Hash("idle"));
	Stepped(subject, &host, 5);
	ASSERT_EQ(subject.State(Mode).i, Hash("eat")) << "idle, energy 0.2 and food near: eat";
	provenance_view const ring = subject.Provenance();
	// The step's brain pick, then exit (leave idle) and entry's three.
	ASSERT_EQ(ring.size(), 4u + 5u);
	provenance_entry const& brain = ring[4];
	EXPECT_EQ(brain.pass, 1u);
	EXPECT_EQ(brain.run, provenance_run::Step);
	EXPECT_EQ(brain.list, trigger::Step);
	EXPECT_EQ(brain.table, uint32_t(Brain));
	EXPECT_EQ(brain.answer.outcome, clause_outcome::Answered);
	EXPECT_EQ(brain.answer.row, subject.Answer(trigger::Step, 0).row);
	EXPECT_FALSE(brain.fellThrough);
	ASSERT_EQ(brain.Keys().size(), 2u);
	EXPECT_EQ(brain.Keys()[0].i, Hash("idle")) << "the state it read";
	EXPECT_EQ(brain.Keys()[1].i, 0) << "no event";
	ASSERT_EQ(brain.Inputs().size(), 2u);
	EXPECT_EQ(brain.Inputs()[0].d, 0.2) << "as read, before the clamp";
	EXPECT_EQ(brain.Inputs()[1].i, 1);
	EXPECT_TRUE(brain.Previous().empty()) << "the brain has no edge";
	EXPECT_FALSE(brain.Truncated());

	provenance_entry const& leave = ring[5];
	EXPECT_EQ(leave.list, trigger::Exit);
	EXPECT_EQ(leave.table, uint32_t(Leave));
	ASSERT_EQ(leave.Keys().size(), 1u);
	EXPECT_EQ(leave.Keys()[0].i, Hash("idle")) << "exit reads the state being left";
	provenance_entry const& settle = ring[7];
	EXPECT_EQ(settle.list, trigger::Entry);
	EXPECT_EQ(settle.clause, 1u);
	ASSERT_EQ(settle.Keys().size(), 1u);
	EXPECT_EQ(settle.Keys()[0].i, int64_t(subject.Answer(trigger::Entry, 0).bits)) << "the or clause's answer, as its key";
	EXPECT_TRUE(settle.Inputs().empty());

	// Out of the input's 0..1 range: the pick clamps it, the entry keeps it.
	host.energy    = 1.5;
	host.food_near = 0;
	Stepped(subject, &host, 6);
	provenance_entry const* outside = nullptr;
	for (provenance_entry const& entry : subject.Provenance())
	{
		if (entry.list == trigger::Step && entry.pass == 2)
		{
			outside = &entry;
		}
	}
	ASSERT_NE(outside, nullptr);
	EXPECT_EQ(outside->answer.outcome, clause_outcome::Answered) << "the clamped point picks a row";
	ASSERT_EQ(outside->Inputs().size(), 2u);
	EXPECT_EQ(outside->Inputs()[0].d, 1.5) << "as read, not the clamped 1";
}

TEST(Provenance, AFallThroughIsMarkedAndKeepsTheTypeAsRead)
{
	auto w = Life();
	counted_ptr<Binding const> v1 = w->Bound("v1");
	ASSERT_TRUE(v1);
	host_object host;
	host.energy = 0.9;
	instance subject = Created(*v1, 1, 1, &host);
	Stepped(subject, &host, 1, Message(Hash("sneeze")));
	provenance_entry const* brain = nullptr;
	for (provenance_entry const& entry : subject.Provenance())
	{
		if (entry.list == trigger::Step)
		{
			brain = &entry;
		}
	}
	ASSERT_NE(brain, nullptr);
	EXPECT_TRUE(brain->fellThrough);
	EXPECT_EQ(brain->answer.outcome, clause_outcome::Answered);
	EXPECT_EQ(brain->Keys()[1].i, Hash("sneeze")) << "the type read, not the blank it fell to";

	Stepped(subject, &host, 2, Message(Hash("scared")));
	provenance_view const ring = subject.Provenance();
	provenance_entry const* scared = nullptr;
	for (provenance_entry const& entry : ring)
	{
		if (entry.list == trigger::Step && entry.pass == 2)
		{
			scared = &entry;
		}
	}
	ASSERT_NE(scared, nullptr);
	EXPECT_FALSE(scared->fellThrough) << "scared has a run of its own";
	EXPECT_EQ(scared->Keys()[1].i, Hash("scared"));
}

TEST(Provenance, HoldsAreRecordedWithWhatWasRead)
{
	auto w = Life();
	counted_ptr<Binding const> v1 = w->Bound("v1");
	ASSERT_TRUE(v1);
	host_object host;
	host.energy = 0.9;
	instance subject = Created(*v1, 1, 1, &host);
	auto lastStep = [&]() -> provenance_entry
	{
		provenance_view const ring = subject.Provenance();
		for (size_t i = ring.size(); i-- > 0;)
		{
			if (ring[i].list == trigger::Step)
			{
				return ring[i];
			}
		}
		ADD_FAILURE() << "no step pick";
		return provenance_entry{};
	};

	// A script that cannot answer: idle's cornered row with food near 2.
	host.food_near = 2;
	host.broken    = true;
	Stepped(subject, &host, 1);
	provenance_entry const raised = lastStep();
	EXPECT_EQ(raised.answer.outcome, clause_outcome::Failed);
	EXPECT_EQ(raised.answer.row, subject.Answer(trigger::Step, 0).row);
	EXPECT_NE(raised.answer.row, UINT32_MAX);
	EXPECT_EQ(raised.Inputs().size(), 2u);

	// A NaN: refused on the input, which is kept as read.
	host.broken = false;
	host.energy = kNaN;
	Stepped(subject, &host, 2);
	provenance_entry const nan = lastStep();
	EXPECT_EQ(nan.answer.outcome, clause_outcome::Failed);
	EXPECT_EQ(nan.answer.input, 0u);
	ASSERT_EQ(nan.Inputs().size(), 2u);
	EXPECT_TRUE(std::isnan(nan.Inputs()[0].d));
	EXPECT_EQ(nan.pass, 2u);

	// A getter that failed: nothing whole was read.
	host.energy  = 0.9;
	host.unwired = true;
	Stepped(subject, &host, 3);
	provenance_entry const held = lastStep();
	EXPECT_EQ(held.answer.outcome, clause_outcome::SourceHeld);
	EXPECT_TRUE(held.Keys().empty());
	EXPECT_TRUE(held.Inputs().empty());
	EXPECT_EQ(held.pass, 3u);
}

namespace
{

// An edge (P13) over t: its entry keeps the previous value it crossed from.
constexpr std::string_view kZone = R"(
[table zone
  (input zone double 0..1)
  (order label)]
>label: int, ^zone, clip: int
[machine zoner
  (state label int (initial 1))
  (state clip int (initial 0))
  (input t double 0..1)]
list,from,where,given,set,emit,overrides
step,zone,label=state.label,zone=input.t,clip=clip,,
)";

}

TEST(Provenance, AnEdgeTablesEntryKeepsThePreviousValueItCrossedFrom)
{
	World w(kZone);
	w.Load(0, "label, zone, clip\n1, 0.4..0.5, 7\n1, , 0\n");
	counted_ptr<Binding const> bound = w.Bound("z");
	ASSERT_TRUE(bound);
	host_object host;
	host.t = 0.2;
	instance subject = Created(*bound, 1, 1, &host);
	host.t = 0.9;
	Stepped(subject, &host, 1);
	EXPECT_EQ(subject.State(1).i, 7) << "0.2 -> 0.9 passed through 0.4..0.5";
	provenance_view const ring = subject.Provenance();
	ASSERT_EQ(ring.size(), 1u);
	ASSERT_EQ(ring[0].Previous().size(), 1u);
	EXPECT_EQ(ring[0].Previous()[0].d, 0.2);
	EXPECT_EQ(ring[0].Inputs()[0].d, 0.9);
	EXPECT_EQ(ring[0].Keys()[0].i, 1);
}

TEST(Provenance, RecordingAllocatesNothing)
{
	if (!kCountsAllocations)
	{
		GTEST_SKIP() << "a sanitizer owns operator new in this build, so nothing counts allocations";
	}
	auto w = Life();
	counted_ptr<Binding const> v1 = w->Bound("v1");
	ASSERT_TRUE(v1);
	host_object host;
	instance subject = Created(*v1, 1, 1, &host);
	w->quiet = true;
	size_t allocated = 0;
	{
		AllocationCount count;
		for (int i = 0; i < 200; ++i)
		{
			host.energy    = (i % 11) / 10.0;
			host.food_near = i % 10;
			host.cornered  = i % 3 == 0;
			Stepped(subject, &host, uint64_t(i), Message(i % 5 == 0 ? Hash("scared") : (i % 5 == 1 ? Hash("calm") : 0)));
		}
		allocated = count.Count();
	}
	EXPECT_EQ(allocated, 0u);
	EXPECT_GT(w->counted, 20u) << "states were entered and left, so every list was recorded";
	EXPECT_EQ(subject.Provenance().size(), kProvenanceDepth);
}

// Reading a moved-from instance's ring is caller misuse, as every accessor's
// is: it aborts (asserts are live) instead of answering with an empty ring.
TEST(Provenance, AMovedFromInstancesRingAborts)
{
	auto w = Life();
	counted_ptr<Binding const> v1 = w->Bound("v1");
	ASSERT_TRUE(v1);
	host_object host;
	instance subject = Created(*v1, 1, 1, &host);
	instance moved   = std::move(subject);
	EXPECT_EQ(moved.Provenance().size(), 4u);
	EXPECT_DEATH((void)subject.Provenance(), "moved-from");
}

#else

TEST(Provenance, IsADebugBuildAid)
{
	GTEST_SKIP() << "TABULAR_PROVENANCE is 0 in this build: release instances keep no ring and have no Provenance()";
}

#endif
