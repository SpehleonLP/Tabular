#include "tabular/instance.h"

#include "layout.h"
#include "machine_internal.h"
#include "pick_internal.h"
#include "relation_internal.h"

#include "cityhash/src/city.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <optional>
#include <span>
#include <utility>

namespace tabular
{

namespace
{

void ReleaseGetters(std::span<input_getter const> getters)
{
	for (input_getter const& g : getters)
	{
		if (g.release)
		{
			g.release(g.data);
		}
	}
}

// The getters resolve has returned while Bind runs: released with it on a
// refusal, or handed whole to the binding, which releases them when dropped.
// Either way each is released exactly once.
struct staged_getters
{
	std::vector<input_getter> getters;

	staged_getters()                                 = default;
	staged_getters(staged_getters const&)            = delete;
	staged_getters& operator=(staged_getters const&) = delete;
	~staged_getters() { ReleaseGetters(getters); }

	immutable_array<input_getter> Adopt()
	{
		auto adopted = immutable_array<input_getter>::FromArray(getters.data(), getters.size());
		getters.clear();
		return adopted;
	}
};

}

struct Binding::Data
{
	counted_ptr<Machine const>                         machine;
	immutable_array<input_getter>                      getters;   // per machine input
	// Per list: where its clauses' answers start in an instance's answers,
	// then one end; and per clause, its weighted draw site.
	std::array<uint32_t, kTriggerCount + 1>            answerStart{};
	std::array<immutable_array<uint64_t>, kTriggerCount> sites;
	// The widest any clause needs: keys, table inputs, layout axes.
	size_t                                             maxKeys   = 0;
	size_t                                             maxInputs = 0;
	size_t                                             maxAxes   = 0;

	Data() = default;
	Data(Data const&)            = delete;
	Data& operator=(Data const&) = delete;
	~Data() { ReleaseGetters(std::span<input_getter const>(getters.begin(), getters.size())); }
};

struct binding_access
{
	using Data = Binding::Data;

	static Data const& Of(Binding const& b) { return *b._data; }
	static counted_ptr<Binding const> Make(std::unique_ptr<Data>&& data)
	{
		return UncountedWrap(static_cast<Binding const*>(new Binding(std::move(data))));
	}
};

struct instance_access
{
	static instance Make() { return instance(); }

	static counted_ptr<Binding const>& BindingOf(instance& i) { return i._binding; }
	static uint64_t&                   Id(instance& i) { return i._id; }
	static std::vector<cell>&          Current(instance& i) { return i._current; }
	static std::vector<cell>&          Next(instance& i) { return i._next; }
	static std::vector<cell>&          Inputs(instance& i) { return i._inputs; }
	static std::vector<uint8_t>&       InputRead(instance& i) { return i._inputRead; }
	static std::vector<clause_answer>& Answers(instance& i) { return i._answers; }
	static std::vector<cell>&          Keys(instance& i) { return i._keys; }
	static std::vector<cell>&          Given(instance& i) { return i._given; }
	static std::vector<float>&         Axes(instance& i) { return i._axes; }
};

Binding::Binding(std::unique_ptr<Data>&& data) : _data(std::move(data)) {}

Binding::~Binding() = default;

void Binding::Release() const
{
	if (_refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
	{
		delete this;
	}
}

Machine const& Binding::machine() const
{
	return *_data->machine;
}

uint64_t DrawSite(std::string_view machine, trigger list, uint32_t clause)
{
	uint64_t const name = CityHash32(machine.data(), machine.size());
	return (name << 32) | (uint64_t(list) << 24) | (clause & 0xFFFFFFu);
}

tl::expected<counted_ptr<Binding const>, ErrorContext> Bind(counted_ptr<Machine const> machine, host_binding&& host)
{
	if (!machine)
	{
		return tl::make_unexpected(ErrorContext("Bind was given no machine"));
	}
	machine_access::Data const& prepared = machine_access::Of(*machine);
	detail::machine_def const&  def      = prepared.def;
	auto data     = std::make_unique<binding_access::Data>();
	data->machine = machine;
	if (!def.inputs.empty() && !host.resolve)
	{
		return tl::make_unexpected(ErrorContext("machine '", def.name,
			"' has inputs, and the host binding has no resolve to supply them"));
	}
	// Every getter resolve returns is staged at once, so a refusal below
	// releases it with the rest when staged is dropped.
	staged_getters staged;
	staged.getters.reserve(def.inputs.size());
	for (input_def const& in : def.inputs)
	{
		auto getter = host.resolve(in.name);
		if (!getter)
		{
			return tl::make_unexpected(std::move(getter.error()).with("machine '", def.name, "' input '", in.name, "'"));
		}
		staged.getters.push_back(*getter);
		if (getter->read == nullptr)
		{
			return tl::make_unexpected(ErrorContext("machine '", def.name, "' input '", in.name,
				"': the host returned a getter with no read function"));
		}
	}

	uint32_t              answers = 0;
	std::vector<uint64_t> sites;
	for (size_t list = 0; list < kTriggerCount; ++list)
	{
		data->answerStart[list] = answers;
		std::vector<detail::resolved_clause> const& clauses = def.lists[list];
		sites.clear();
		for (uint32_t i = 0; i < clauses.size(); ++i)
		{
			sites.push_back(DrawSite(def.name, static_cast<trigger>(list), i));
			data->maxKeys   = std::max(data->maxKeys, clauses[i].keys.size());
			data->maxInputs = std::max(data->maxInputs, clauses[i].inputs.size());
			data->maxAxes   = std::max(data->maxAxes, prepared.lists[list][i].runs.layout.dims);
		}
		data->sites[list] = immutable_array<uint64_t>::FromArray(sites.data(), sites.size());
		answers += static_cast<uint32_t>(clauses.size());
	}
	data->answerStart[kTriggerCount] = answers;
	data->getters                    = staged.Adopt();
	return binding_access::Make(std::move(data));
}

namespace
{

using detail::prepared_clause;
using detail::resolved_clause;
using detail::resolved_source;

// One run of a list over an instance: the buffers it reads and writes, laid
// out by CreateInstance so nothing here allocates.
struct list_run
{
	binding_access::Data const&               binding;
	machine_access::Data const&        machine;
	std::vector<cell>&                 next;
	std::vector<cell> const&           inputs;
	std::vector<uint8_t> const&        inputRead;
	std::span<clause_answer>           answers;   // this list's
	std::vector<cell>&                 keys;
	std::vector<cell>&                 given;
	std::vector<float>&                axes;
	void const*                        self;
	uint64_t                           seed;
	size_t                             list;
};

cell CellAt(TableVersion const& version, uint32_t row, uint32_t column)
{
	table_rows const& rows = version_access::Rows(version);
	return rows.cells[size_t(row) * rows.valueCount + rows.slot[column]];
}

// The value a clause's answer gives one of its value columns: an or clause's
// only answer is its OR, every other clause's is its row's cell.
cell AnswerValue(prepared_clause const& clause, clause_answer const& answer, uint32_t column)
{
	if (version_access::Def(*clause.version).pick == pick_policy::Or)
	{
		return cell::Integer(answer.bits);
	}
	return CellAt(*clause.version, answer.row, column);
}

// Reads sources into out; false when one has no value this step (an input
// whose getter failed, or an earlier clause that did not answer).
bool ReadSources(list_run const& run, std::span<resolved_source const> sources, std::vector<cell>& out)
{
	for (size_t s = 0; s < sources.size(); ++s)
	{
		resolved_source const& from = sources[s];
		switch (from.kind)
		{
		case source_kind::State:
			out[s] = run.next[from.index];
			break;
		case source_kind::Input:
			if (!run.inputRead[from.index])
			{
				return false;
			}
			out[s] = run.inputs[from.index];
			break;
		case source_kind::Clause:
		{
			clause_answer const& earlier = run.answers[from.index];
			if (earlier.outcome != clause_outcome::Answered)
			{
				return false;
			}
			out[s] = AnswerValue(run.machine.lists[run.list][from.index], earlier, from.column);
			break;
		}
		case source_kind::Literal:
			out[s] = from.literal;
			break;
		}
	}
	return true;
}

// The tuple of the clause's domain equal to keys, by binary search over its
// sorted tuples (signed int64, key 0 most significant, as Prepare sorted
// them); SIZE_MAX when the domain has none.
size_t FindTuple(prepared_clause const& clause, std::span<cell const> keys)
{
	size_t const   width = keys.size();
	int64_t const* base  = clause.tuples.begin();
	auto compare = [&](size_t t)
	{
		for (size_t k = 0; k < width; ++k)
		{
			int64_t const v = base[t * width + k];
			if (v != keys[k].i)
			{
				return v < keys[k].i ? -1 : 1;
			}
		}
		return 0;
	};
	size_t lo = 0;
	size_t hi = clause.tupleCount;
	while (lo < hi)
	{
		size_t const mid = lo + (hi - lo) / 2;
		if (compare(mid) < 0)
		{
			lo = mid + 1;
		}
		else
		{
			hi = mid;
		}
	}
	return lo < clause.tupleCount && compare(lo) == 0 ? lo : SIZE_MAX;
}

// What a best clause's laid-out pick found: Answered with its row, NoRun when
// every row tried is scripted and none holds, Failed when a script condition
// could not answer.
struct laid_pick
{
	clause_outcome outcome = clause_outcome::NoRun;
	uint32_t       row     = UINT32_MAX;
};

// The first row of group g's list for the input point that holds. The point's
// cell is found per axis by binary search over the group's cuts.
laid_pick TryGroup(TableVersion const& version, detail::run_layout const& layout, size_t g,
	std::span<float const> point, void const* self)
{
	size_t const dims = layout.dims;
	size_t       cell = 0;
	for (size_t a = 0; a < dims; ++a)
	{
		float const* begin = layout.cuts.data() + layout.cutStart[g * dims + a];
		float const* end   = layout.cuts.data() + layout.cutStart[g * dims + a + 1];
		size_t const spans = static_cast<size_t>(end - begin) - 1;
		// The clamped point lies in [first cut, last cut), so upper_bound lands
		// in 1..spans; the min is a guard that keeps a point on the last cut
		// (which the clamp should never produce) in the top span instead of
		// past the group's cells.
		size_t const at = std::min(static_cast<size_t>(std::upper_bound(begin, end, point[a]) - begin), spans) - 1;
		cell = cell * spans + at;
	}
	cell += layout.cellStart[g];
	for (uint32_t k = layout.listStart[cell]; k < layout.listStart[cell + 1]; ++k)
	{
		uint32_t const   row  = layout.lists[k];
		detail::row_test test = detail::ScriptHolds(version, row, self, nullptr);
		if (test == detail::row_test::Failed)
		{
			return laid_pick{clause_outcome::Failed};
		}
		if (test == detail::row_test::Holds)
		{
			return laid_pick{clause_outcome::Answered, row};
		}
	}
	return laid_pick{};
}

// A best clause's pick from its laid-out run: the run's flag groups ranked
// against the input flags with PickBest's own rank (detail::RankOf; a group
// matching nothing skipped; the R == 0 group last), and within a group the
// cell's list in row order. Groups of one run have distinct flags, so the
// rank alone orders them. An unranked table has one group, flags 0, so it is
// the last-tried group and nothing else.
laid_pick PickLaid(prepared_clause const& clause, size_t tuple, std::span<cell const> given, std::vector<float>& axes,
	void const* self)
{
	TableVersion const&        version = *clause.version;
	table_def const&           def     = version_access::Def(version);
	detail::run_layout const&  layout  = clause.runs.layout;
	detail::run_span const     run     = clause.runs.runs[tuple];
	for (size_t a = 0; a < layout.dims; ++a)
	{
		uint32_t const in = clause.axisInputs[a];
		axes[a]           = detail::ClampedInput(def.inputs[in], given[in].d);
	}
	std::span<float const> const point(axes.data(), layout.dims);
	uint32_t const F = clause.rankingInput == UINT32_MAX ? 0u : static_cast<uint32_t>(given[clause.rankingInput].i);

	std::optional<detail::flag_rank> below;
	while (true)
	{
		size_t            best = SIZE_MAX;
		detail::flag_rank bestRank{};
		for (uint32_t g = run.first; g < run.first + run.count; ++g)
		{
			uint32_t const R = layout.groupFlags[g];
			if (!detail::Ranked(R, F))
			{
				continue;   // the default group, or one matching nothing in the input
			}
			detail::flag_rank const k = detail::RankOf(R, F);
			if ((!below || k < *below) && (best == SIZE_MAX || bestRank < k))
			{
				best     = g;
				bestRank = k;
			}
		}
		if (best == SIZE_MAX)
		{
			break;
		}
		laid_pick const picked = TryGroup(version, layout, best, point, self);
		if (picked.outcome != clause_outcome::NoRun)
		{
			return picked;
		}
		below = bestRank;
	}
	size_t const fallback = detail::FindGroup(layout, run, 0);
	if (fallback == SIZE_MAX)
	{
		return laid_pick{};
	}
	return TryGroup(version, layout, fallback, point, self);
}

// Runs one clause: its sources, its run, its pick, then its sets into next.
// A clause that cannot answer writes nothing and says why in its answer.
clause_answer RunClause(list_run const& run, size_t index)
{
	resolved_clause const& rc = run.machine.def.lists[run.list][index];
	prepared_clause const& pc = run.machine.lists[run.list][index];
	TableVersion const&    version = *pc.version;
	table_def const&       def     = version_access::Def(version);
	std::span<cell const>  keys(run.keys.data(), rc.keys.size());
	std::span<cell const>  given(run.given.data(), rc.inputs.size());

	clause_answer answer;
	if (!ReadSources(run, rc.keys, run.keys) || !ReadSources(run, rc.inputs, run.given))
	{
		answer.outcome = clause_outcome::SourceHeld;
		return answer;
	}
	// Inputs before the run, as a pick checks its query before finding its run.
	if (detail::FirstBadInput(def, given).index != UINT32_MAX)
	{
		answer.outcome = clause_outcome::Failed;
		return answer;
	}
	size_t const tuple = FindTuple(pc, keys);
	if (tuple == SIZE_MAX)
	{
		answer.outcome = clause_outcome::NoRun;
		return answer;
	}

	switch (def.pick)
	{
	case pick_policy::Best:
	{
		laid_pick const picked = PickLaid(pc, tuple, given, run.axes, run.self);
		// NoRun is not reached for a proved run (its default group's every list
		// ends in a row with no script); a hold keeps a broken proof from writing.
		if (picked.outcome != clause_outcome::Answered)
		{
			answer.outcome = picked.outcome;
			return answer;
		}
		answer.row = picked.row;
		break;
	}
	case pick_policy::Or:
	{
		detail::row_span const rows = pc.rowRuns[tuple];
		auto bits = detail::OrOfRows(version, rows.first, size_t(rows.first) + rows.count,
			pick_query{keys, given, run.self}, nullptr);
		if (!bits)
		{
			answer.outcome = clause_outcome::Failed;
			return answer;
		}
		answer.bits = *bits;
		break;
	}
	case pick_policy::Weighted:
	{
		detail::row_span const rows = pc.rowRuns[tuple];
		answer.row = static_cast<uint32_t>(
			detail::DrawRow(version, rows.first, size_t(rows.first) + rows.count, run.seed, run.binding.sites[run.list][index]));
		break;
	}
	case pick_policy::None:
	case pick_policy::All:
		// The definition build refuses a clause on these.
		answer.outcome = clause_outcome::Failed;
		return answer;
	}

	answer.outcome = clause_outcome::Answered;
	for (auto const& [column, state] : rc.sets)
	{
		run.next[state] = AnswerValue(pc, answer, column);
	}
	return answer;
}

list_run RunOver(instance& subject, size_t list, void const* self, uint64_t seed)
{
	binding_access::Data const& binding = binding_access::Of(*instance_access::BindingOf(subject));
	std::vector<clause_answer>& answers = instance_access::Answers(subject);
	std::span<clause_answer> const own(answers.data() + binding.answerStart[list],
		binding.answerStart[list + 1] - binding.answerStart[list]);
	return list_run{binding, machine_access::Of(*binding.machine), instance_access::Next(subject),
		instance_access::Inputs(subject), instance_access::InputRead(subject), own, instance_access::Keys(subject),
		instance_access::Given(subject), instance_access::Axes(subject), self, seed, list};
}

// Runs a list in order; each clause sees what the ones before it wrote.
void RunList(instance& subject, trigger list, void const* self, uint64_t seed)
{
	list_run const run = RunOver(subject, size_t(list), self, seed);
	for (size_t i = 0; i < run.answers.size(); ++i)
	{
		run.answers[i] = RunClause(run, i);
	}
}

void MarkNotRun(instance& subject, trigger list)
{
	binding_access::Data const& binding = binding_access::Of(*instance_access::BindingOf(subject));
	std::vector<clause_answer>& answers = instance_access::Answers(subject);
	std::fill(answers.begin() + binding.answerStart[size_t(list)], answers.begin() + binding.answerStart[size_t(list) + 1],
		clause_answer{});
}

// Every input once per step, so every clause of the step reads one value of it.
void ReadInputs(instance& subject, void const* self)
{
	binding_access::Data const&  binding = binding_access::Of(*instance_access::BindingOf(subject));
	std::vector<cell>&    inputs  = instance_access::Inputs(subject);
	std::vector<uint8_t>& read    = instance_access::InputRead(subject);
	for (size_t i = 0; i < binding.getters.size(); ++i)
	{
		input_getter const& g = binding.getters[i];
		ErrorContext        error;
		inputs[i] = g.read(g.data, self, &error);
		read[i]   = error.empty() ? 1 : 0;
	}
}

// A state is entered when a hash state column of next differs from current
// at the end of the step list (so A -> B -> A enters nothing): the hash
// columns are the machine's states, an int or a double is data it keeps.
bool Entered(machine_access::Data const& machine, std::vector<cell> const& current, std::vector<cell> const& next)
{
	for (size_t s = 0; s < machine.def.state.size(); ++s)
	{
		if (machine.def.state[s].type == column_type::Hash && current[s].i != next[s].i)
		{
			return true;
		}
	}
	return false;
}

}

instance::instance(instance&&) noexcept            = default;
instance& instance::operator=(instance&&) noexcept = default;
instance::~instance()                              = default;

Binding const& instance::binding() const
{
	assert(_binding && "a moved-from instance is used: caller misuse");
	return *_binding;
}

clause_answer instance::Answer(trigger list, size_t clause) const
{
	assert(_binding && "a moved-from instance is used: caller misuse");
	binding_access::Data const& binding = binding_access::Of(*_binding);
	return _answers[binding.answerStart[size_t(list)] + clause];
}

instance CreateInstance(Binding const& binding, uint64_t id, uint64_t seed, void const* self)
{
	binding_access::Data const&        data    = binding_access::Of(binding);
	machine_access::Data const& machine = machine_access::Of(*data.machine);
	instance                    out     = instance_access::Make();
	instance_access::BindingOf(out)     = CountedWrap(&binding);
	instance_access::Id(out)            = id;
	std::vector<cell>& current          = instance_access::Current(out);
	for (detail::state_column const& column : machine.def.state)
	{
		current.push_back(column.initial);
	}
	instance_access::Next(out)      = current;
	instance_access::Inputs(out).assign(data.getters.size(), cell::Integer(0));
	instance_access::InputRead(out).assign(data.getters.size(), 0);
	instance_access::Answers(out).assign(data.answerStart[kTriggerCount], clause_answer{});
	instance_access::Keys(out).assign(data.maxKeys, cell::Integer(0));
	instance_access::Given(out).assign(data.maxInputs, cell::Integer(0));
	instance_access::Axes(out).assign(data.maxAxes, 0.f);

	ReadInputs(out, self);
	RunList(out, trigger::Create, self, seed);
	RunList(out, trigger::Entry, self, seed);
	std::swap(instance_access::Current(out), instance_access::Next(out));
	return out;
}

void Step(instance& subject, void const* self, uint64_t seed)
{
	// A moved-from instance has no binding, and stepping one is caller misuse.
	assert(instance_access::BindingOf(subject) && "a moved-from instance is stepped: caller misuse");
	machine_access::Data const& machine = machine_access::Of(*binding_access::Of(subject.binding()).machine);
	std::vector<cell>&          current = instance_access::Current(subject);
	std::vector<cell>&          next    = instance_access::Next(subject);
	ReadInputs(subject, self);
	// Unwritten columns hold because next starts as current.
	std::copy(current.begin(), current.end(), next.begin());
	RunList(subject, trigger::Step, self, seed);
	// The exit list (slice 4, R6) will run here, before entry, on the same test.
	if (Entered(machine, current, next))
	{
		RunList(subject, trigger::Entry, self, seed);
	}
	else
	{
		MarkNotRun(subject, trigger::Entry);
	}
	// Swapping two vectors moves their buffers, so nothing is allocated.
	std::swap(current, next);
}

}
