#include "tabular/instance.h"

#include "layout.h"
#include "machine_internal.h"
#include "pick_internal.h"
#include "relation_internal.h"

#include "Support/strings.h"
#include "cityhash/src/city.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <limits>
#include <optional>
#include <span>
#include <string>
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

// Whether any condition cell of row holds a script: only such a row can fail
// to answer, so only it has a report bit.
bool RowIsScripted(TableVersion const& version, uint32_t row)
{
	table_def const&  def  = version_access::Def(version);
	table_rows const& rows = version_access::Rows(version);
	for (uint32_t c = 0; c < def.columns.size(); ++c)
	{
		if (def.columns[c].type == column_type::Condition
			&& rows.conditions[size_t(row) * rows.conditionCount + rows.slot[c]]->get().function != nullptr)
		{
			return true;
		}
	}
	return false;
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
	// Per machine input; an input the message carries has none (no read).
	immutable_array<input_getter>                      getters;
	std::function<void(uint64_t, uint32_t, size_t, ErrorContext const&)>  report;
	// Per list: where its clauses' answers start in an instance's answers,
	// then one end; and per clause, its weighted draw site.
	std::array<uint32_t, kTriggerCount + 1>            answerStart{};
	std::array<immutable_array<uint64_t>, kTriggerCount> sites;
	// An instance's report-once bits: one per machine input (its getter
	// failed), one for a message carrying more arguments than the machine
	// declares (the bit right after the inputs'), then per clause, from its
	// start: no run, one per table input (refused), one per version row that
	// has a script condition (it could not answer). Only a scripted row can
	// fail, so a clause costs 1 + its table inputs + its scripted rows: a
	// 1000-row table with 3 scripted rows and 2 inputs takes 6 bits, not 1003;
	// a weighted table has no conditions and takes none for rows.
	std::array<immutable_array<uint32_t>, kTriggerCount> reportStart;
	// Per list, per clause, where its rows' ranks start in rowRank; rowRank is
	// each clause's version rows in order, a scripted row's rank among the
	// clause's scripted rows, UINT32_MAX for a row with no script condition.
	std::array<immutable_array<uint32_t>, kTriggerCount> rankStart;
	std::array<immutable_array<uint32_t>, kTriggerCount> rowRank;
	size_t                                             reportBits = 0;
	// The clauses that can emit, across every list: the most commands one run
	// returns, so an instance's result buffer is sized once.
	size_t                                             emitters   = 0;
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
	static std::vector<cell>&          Previous(instance& i) { return i._previous; }
	static std::vector<cell>&          GivenBefore(instance& i) { return i._givenBefore; }
	static std::vector<clause_answer>& Answers(instance& i) { return i._answers; }
	static std::vector<uint64_t>&      Reported(instance& i) { return i._reported; }
	static std::vector<cell>&          Keys(instance& i) { return i._keys; }
	static std::vector<cell>&          Given(instance& i) { return i._given; }
	static std::vector<float>&         Axes(instance& i) { return i._axes; }
	static bool&                       Absorbed(instance& i) { return i._absorbed; }
	static std::vector<emitted>&       Emitted(instance& i) { return i._emitted; }
	static size_t&                     EmittedCount(instance& i) { return i._emittedCount.n; }
	static counted_ptr<Binding const>& Left(instance& i) { return i._left; }
#if TABULAR_PROVENANCE
	static std::vector<provenance_entry>& Ring(instance& i) { return i._provenance; }
	static size_t&                        RingNext(instance& i) { return i._provenanceNext; }
	static size_t&                        RingCount(instance& i) { return i._provenanceCount; }
	static uint64_t&                      Pass(instance& i) { return i._pass; }
	static provenance_run&                Run(instance& i) { return i._run; }
#endif
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
	bool const needsResolve = std::find(def.carried.begin(), def.carried.end(), detail::kFromGetter) != def.carried.end();
	if (needsResolve && !host.resolve)
	{
		return tl::make_unexpected(ErrorContext("machine '", def.name,
			"' has inputs, and the host binding has no resolve to supply them"));
	}
	// Every getter resolve returns is staged at once, so a refusal below
	// releases it with the rest when staged is dropped.
	staged_getters staged;
	staged.getters.reserve(def.inputs.size());
	for (size_t i = 0; i < def.inputs.size(); ++i)
	{
		input_def const& in = def.inputs[i];
		if (def.carried[i] != detail::kFromGetter)
		{
			staged.getters.push_back(input_getter{});
			continue;
		}
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
	size_t                bits    = def.inputs.size() + 1;   // the getters, then extra arguments
	std::vector<uint64_t> sites;
	std::vector<uint32_t> starts;
	std::vector<uint32_t> rankStarts;
	std::vector<uint32_t> ranks;
	for (size_t list = 0; list < kTriggerCount; ++list)
	{
		data->answerStart[list] = answers;
		std::vector<detail::resolved_clause> const& clauses = def.lists[list];
		sites.clear();
		starts.clear();
		rankStarts.clear();
		ranks.clear();
		for (uint32_t i = 0; i < clauses.size(); ++i)
		{
			detail::prepared_clause const& pc = prepared.lists[list][i];
			sites.push_back(DrawSite(def.name, static_cast<trigger>(list), i));
			starts.push_back(static_cast<uint32_t>(bits));
			rankStarts.push_back(static_cast<uint32_t>(ranks.size()));
			uint32_t scripted = 0;
			for (uint32_t r = 0; r < pc.version->RowCount(); ++r)
			{
				ranks.push_back(RowIsScripted(*pc.version, r) ? scripted++ : UINT32_MAX);
			}
			bits += 1 + clauses[i].inputs.size() + scripted;
			data->emitters += clauses[i].emit != UINT32_MAX ? 1 : 0;
			data->maxKeys   = std::max(data->maxKeys, clauses[i].keys.size());
			data->maxInputs = std::max(data->maxInputs, clauses[i].inputs.size());
			data->maxAxes   = std::max(data->maxAxes, pc.runs.layout.dims);
		}
		data->sites[list]       = immutable_array<uint64_t>::FromArray(sites.data(), sites.size());
		data->reportStart[list] = immutable_array<uint32_t>::FromArray(starts.data(), starts.size());
		data->rankStart[list]   = immutable_array<uint32_t>::FromArray(rankStarts.data(), rankStarts.size());
		data->rowRank[list]     = immutable_array<uint32_t>::FromArray(ranks.data(), ranks.size());
		answers += static_cast<uint32_t>(clauses.size());
	}
	data->answerStart[kTriggerCount] = answers;
	data->reportBits                 = bits;
	data->report                     = std::move(host.report);
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
	binding_access::Data const&        binding;
	machine_access::Data const&        machine;
	std::vector<cell>&                 next;
	std::vector<cell> const&           state;     // what a State source reads: next, or current for exit
	std::vector<cell> const&           inputs;
	std::vector<uint8_t> const&        inputRead;
	std::vector<cell> const&           previous;   // per machine input, for edges
	std::span<clause_answer>           answers;   // this list's
	std::vector<uint64_t>&             reported;
	std::vector<cell>&                 keys;
	std::vector<cell>&                 given;
	std::vector<cell>&                 givenBefore;
	std::vector<float>&                axes;
	bool&                              absorbed;
	uint64_t                           id;
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
			out[s] = run.state[from.index];
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
// every row tried is conditional and none holds, Failed with the row whose script
// condition could not answer.
struct laid_pick
{
	clause_outcome outcome = clause_outcome::NoRun;
	uint32_t       row     = UINT32_MAX;
};

// The first row of group g's list for the input point that holds. The point's
// cell is found per axis by binary search over the group's cuts. The cell
// lies inside every plain band of its rows; an edge row's crossing is tested
// here, from the table inputs given and their previous values, before its
// script, as a pick tests bands before scripts.
laid_pick TryGroup(TableVersion const& version, detail::run_layout const& layout, size_t g,
	std::span<float const> point, std::span<cell const> given, std::span<cell const> before, void const* self,
	ErrorContext& error)
{
	bool const edges = version_access::Def(version).edges;
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
		uint32_t const row = layout.lists[k];
		if (edges && !detail::EdgesHold(version, row, given, before))
		{
			continue;
		}
		detail::row_test test = detail::ScriptHolds(version, row, self, error);
		if (test == detail::row_test::Failed)
		{
			return laid_pick{clause_outcome::Failed, row};
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
laid_pick PickLaid(prepared_clause const& clause, size_t tuple, std::span<cell const> given,
	std::span<cell const> before, std::vector<float>& axes, void const* self, ErrorContext& error)
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
		laid_pick const picked = TryGroup(version, layout, best, point, given, before, self, error);
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
	return TryGroup(version, layout, fallback, point, given, before, self, error);
}

// The report-once memory: whether failure bit has been reported by this
// instance, marking it if not. Only the first occurrence reaches the host, so
// only it builds an error.
bool FirstReport(std::vector<uint64_t>& reported, size_t bit)
{
	uint64_t&      word = reported[bit / 64];
	uint64_t const mask = uint64_t(1) << (bit % 64);
	if ((word & mask) != 0)
	{
		return false;
	}
	word |= mask;
	return true;
}

// The one place a clause's hold reaches the host: once per bit, the error
// built only then, naming the clause after what failed.
template<typename Build>
void ReportHold(list_run const& run, size_t index, size_t bit, size_t row, Build const& build)
{
	if (!FirstReport(run.reported, bit) || !run.binding.report)
	{
		return;
	}
	resolved_clause const& rc    = run.machine.def.lists[run.list][index];
	std::string_view const table = version_access::Def(*run.machine.lists[run.list][index].version).name;
	run.binding.report(run.id, rc.table, row,
		build().with(detail::ClausePlace(run.machine.def.name, run.list, index, table)));
}

std::string KeyText(std::span<cell const> keys)
{
	std::string text;
	for (cell const& k : keys)
	{
		text = cc(text, text.empty() ? "" : ", ", k.i);
	}
	return text;
}

// The report of a row whose script condition could not answer: the host's
// error from the step's one asking, naming the table and row. Built only the
// first time the row is reported.
ErrorContext ConditionError(TableVersion const& version, size_t row, ErrorContext&& error)
{
	return std::move(error).with("table '", version_access::Def(version).name, "' row ", row);
}

// Runs one clause: its sources, its run, its pick, then its sets into next.
// A clause that cannot answer writes nothing, says why in its answer, and
// reports it once (a source held by an earlier hold or a failed getter was
// reported where that hold began).
clause_answer RunClause(list_run const& run, size_t index)
{
	resolved_clause const& rc = run.machine.def.lists[run.list][index];
	prepared_clause const& pc = run.machine.lists[run.list][index];
	TableVersion const&    version = *pc.version;
	table_def const&       def     = version_access::Def(version);
	std::span<cell const>  keys(run.keys.data(), rc.keys.size());
	std::span<cell const>  given(run.given.data(), rc.inputs.size());
	size_t const           bits = run.binding.reportStart[run.list][index];
	// A scripted row's report bit, past the no-run and table-input bits.
	auto rowBit = [&](size_t row)
	{
		uint32_t const rank = run.binding.rowRank[run.list][run.binding.rankStart[run.list][index] + row];
		assert(rank != UINT32_MAX && "only a scripted row fails to answer");
		return bits + 1 + rc.inputs.size() + rank;
	};
	// The host's error, from the one asking of the failing row's condition.
	ErrorContext           error;

	clause_answer answer;
	if (!ReadSources(run, rc.keys, run.keys) || !ReadSources(run, rc.inputs, run.given))
	{
		answer.outcome = clause_outcome::SourceHeld;
		return answer;
	}
	// Inputs before the run, as a pick checks its query before finding its run.
	detail::bad_input const bad = detail::FirstBadInput(def, given);
	if (bad.index != UINT32_MAX)
	{
		answer.outcome = clause_outcome::Failed;
		answer.input   = bad.index;
		ReportHold(run, index, bits + 1 + bad.index, kNoRow, [&]() { return detail::BadInputError(def, bad, given); });
		return answer;
	}
	// What each edge crosses from: its machine input's previous value (an edge
	// reads only a getter's input). Other table inputs keep this step's value,
	// which no edge reads.
	std::span<cell const> before;
	if (def.edges)
	{
		std::copy(given.begin(), given.end(), run.givenBefore.begin());
		for (column_def const& col : def.columns)
		{
			if (col.edge)
			{
				run.givenBefore[col.input] = run.previous[rc.inputs[col.input].index];
			}
		}
		before = std::span<cell const>(run.givenBefore.data(), rc.inputs.size());
	}
	// A message type with no run falls through to the blank run, as every
	// lookup of the table does; the retry writes run.keys itself.
	size_t const tuple = detail::FindFallingThrough(keys, std::span<cell>(run.keys.data(), rc.keys.size()), rc.messageKey,
		[&](std::span<cell const> k) { return FindTuple(pc, k); }, [](size_t t) { return t == SIZE_MAX; });
	if (tuple == SIZE_MAX)
	{
		answer.outcome = clause_outcome::NoRun;
		ReportHold(run, index, bits, kNoRow, [&]()
		{
			return ErrorContext("table '", def.name, "' key (", KeyText(keys),
				") has no run; the value is outside the domain Prepare proved the clause over");
		});
		return answer;
	}

	switch (def.pick)
	{
	case pick_policy::Best:
	{
		laid_pick const picked = PickLaid(pc, tuple, given, before, run.axes, run.self, error);
		if (picked.outcome == clause_outcome::Failed)
		{
			answer.outcome = clause_outcome::Failed;
			answer.row     = picked.row;
			ReportHold(run, index, rowBit(picked.row), picked.row,
				[&]() { return ConditionError(version, picked.row, std::move(error)); });
			return answer;
		}
		// Not reached for a proved run (its default group's every list ends in
		// a row with no script); a hold keeps a broken proof from writing.
		if (picked.outcome != clause_outcome::Answered)
		{
			answer.outcome = picked.outcome;
			ReportHold(run, index, bits, kNoRow, [&]()
			{
				return ErrorContext("table '", def.name, "' key (", KeyText(keys),
					") picked no row, which its proof should have ruled out");
			});
			return answer;
		}
		answer.row = picked.row;
		break;
	}
	case pick_policy::Or:
	{
		detail::row_span const rows = pc.rowRuns[tuple];
		size_t                 failed = SIZE_MAX;
		auto ored = detail::OrOfRows(version, rows.first, size_t(rows.first) + rows.count,
			pick_query{keys, given, run.self, before}, error, failed);
		if (!ored)
		{
			answer.outcome = clause_outcome::Failed;
			answer.row     = static_cast<uint32_t>(failed);
			ReportHold(run, index, rowBit(failed), failed,
				[&]() { return ConditionError(version, failed, std::move(error)); });
			return answer;
		}
		answer.bits = *ored;
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
	// A clause that fell through left its message key blank, so only a run
	// keyed on the type itself absorbs the message.
	if (rc.messageKey != UINT32_MAX && run.keys[rc.messageKey].i != 0)
	{
		run.absorbed = true;
	}
	return answer;
}

list_run RunOver(instance& subject, size_t list, std::vector<cell> const& state, void const* self, uint64_t seed)
{
	binding_access::Data const& binding = binding_access::Of(*instance_access::BindingOf(subject));
	std::vector<clause_answer>& answers = instance_access::Answers(subject);
	std::span<clause_answer> const own(answers.data() + binding.answerStart[list],
		binding.answerStart[list + 1] - binding.answerStart[list]);
	return list_run{binding, machine_access::Of(*binding.machine), instance_access::Next(subject), state,
		instance_access::Inputs(subject), instance_access::InputRead(subject), instance_access::Previous(subject), own,
		instance_access::Reported(subject), instance_access::Keys(subject), instance_access::Given(subject),
		instance_access::GivenBefore(subject), instance_access::Axes(subject),
		instance_access::Absorbed(subject), subject.id(), self, seed, list};
}

#if TABULAR_PROVENANCE
// Writes what clause index of run read and answered into the next slot of the
// ring, overwriting the oldest once it is full. Reads the scratch buffers the
// clause left: its keys (with the message type put back where it fell
// through to the blank run), its inputs and, with edges, their previous
// values. Copies into the ring sized at creation, so it allocates nothing.
void Record(instance& subject, list_run const& run, size_t index, clause_answer const& answer)
{
	resolved_clause const& rc  = run.machine.def.lists[run.list][index];
	table_def const&       def = version_access::Def(*run.machine.lists[run.list][index].version);
	std::vector<provenance_entry>& ring = instance_access::Ring(subject);
	size_t& next = instance_access::RingNext(subject);
	provenance_entry& entry = ring[next];
	next = (next + 1) % ring.size();
	instance_access::RingCount(subject) = std::min(instance_access::RingCount(subject) + 1, ring.size());

	entry        = provenance_entry{};
	entry.pass   = instance_access::Pass(subject);
	entry.run    = instance_access::Run(subject);
	entry.list   = static_cast<trigger>(run.list);
	entry.clause = static_cast<uint32_t>(index);
	entry.table  = rc.table;
	entry.answer = answer;
	if (answer.outcome == clause_outcome::SourceHeld)
	{
		return;
	}
	size_t stored = 0;
	auto keep = [&](cell value)
	{
		if (stored < kProvenanceValues)
		{
			entry.values[stored] = value;
		}
		++stored;
	};
	for (size_t k = 0; k < rc.keys.size(); ++k)
	{
		cell value = run.keys[k];
		// A clause keyed on the message always reads the type from the
		// message input, and only the fall-through leaves 0 where it was not.
		if (k == rc.messageKey && value.i == 0 && rc.keys[k].kind == source_kind::Input
			&& run.inputs[rc.keys[k].index].i != 0)
		{
			value             = run.inputs[rc.keys[k].index];
			entry.fellThrough = true;
		}
		keep(value);
	}
	for (size_t g = 0; g < rc.inputs.size(); ++g)
	{
		keep(run.given[g]);
	}
	entry.keyCount   = static_cast<uint8_t>(rc.keys.size());
	entry.inputCount = static_cast<uint8_t>(rc.inputs.size());
	// A clause refused on an input stopped before taking previous values.
	if (def.edges && answer.input == UINT32_MAX)
	{
		for (size_t g = 0; g < rc.inputs.size(); ++g)
		{
			keep(run.givenBefore[g]);
		}
		entry.previousCount = static_cast<uint8_t>(rc.inputs.size());
	}
}
#endif

// Runs a list in order; each clause sees what the ones before it wrote. state
// is the vector its State sources read.
void RunList(instance& subject, trigger list, std::vector<cell> const& state, void const* self, uint64_t seed)
{
	list_run const run = RunOver(subject, size_t(list), state, self, seed);
	for (size_t i = 0; i < run.answers.size(); ++i)
	{
		run.answers[i] = RunClause(run, i);
#if TABULAR_PROVENANCE
		Record(subject, run, i, run.answers[i]);
#endif
	}
}

void MarkNotRun(instance& subject, trigger list)
{
	binding_access::Data const& binding = binding_access::Of(*instance_access::BindingOf(subject));
	std::vector<clause_answer>& answers = instance_access::Answers(subject);
	std::fill(answers.begin() + binding.answerStart[size_t(list)], answers.begin() + binding.answerStart[size_t(list) + 1],
		clause_answer{});
}

// Every input once per step, so every clause of the step reads one value of
// it: a getter's, or the message's. A getter that fails is reported once; the
// clauses reading it then hold without reporting again.
void ReadInputs(instance& subject, void const* self, message const& msg)
{
	binding_access::Data const&  binding = binding_access::Of(*instance_access::BindingOf(subject));
	detail::machine_def const&   def     = machine_access::Of(*binding.machine).def;
	std::vector<cell>&    inputs  = instance_access::Inputs(subject);
	std::vector<uint8_t>& read    = instance_access::InputRead(subject);
	// Arguments are positional slots every message type shares, and a type's
	// own arity is not declared: a missing one reads 0, as on a step with no
	// message, and is no failure. More than the machine declares is a sender
	// that disagrees with the declaration: the step still runs (it cannot be
	// refused), and the instance says so once.
	if (msg.arguments.size() > def.messageArguments && FirstReport(instance_access::Reported(subject), def.inputs.size())
		&& binding.report)
	{
		binding.report(subject.id(), kNoTable, kNoRow,
			ErrorContext("a message carries ", msg.arguments.size(), " arguments, and the machine declares ",
				def.messageArguments, "; the extra ones are not read").with("machine '", def.name, "' message"));
	}
	for (size_t i = 0; i < binding.getters.size(); ++i)
	{
		uint32_t const carried = def.carried[i];
		if (carried == detail::kMessageType)
		{
			inputs[i] = cell::Integer(msg.type);
			read[i]   = 1;
			continue;
		}
		if (carried != detail::kFromGetter)
		{
			inputs[i] = carried < msg.arguments.size() ? msg.arguments[carried] : cell::Integer(0);
			read[i]   = 1;
			continue;
		}
		input_getter const& g = binding.getters[i];
		ErrorContext        error;
		inputs[i] = g.read(g.data, self, &error);
		read[i]   = error.empty() ? 1 : 0;
		if (!read[i] && FirstReport(instance_access::Reported(subject), i) && binding.report)
		{
			binding.report(subject.id(), kNoTable, kNoRow,
				std::move(error).with("machine '", def.name, "' input '", def.inputs[i].name, "'"));
		}
	}
}

// The values this step read become the previous values the next step's edges
// cross from. A getter that failed, or a NaN, is no value read: the previous
// value stays the last one that was, so a crossing is not lost across it.
void AdvancePrevious(instance& subject)
{
	std::vector<cell> const&    inputs   = instance_access::Inputs(subject);
	std::vector<uint8_t> const& read     = instance_access::InputRead(subject);
	std::vector<cell>&          previous = instance_access::Previous(subject);
	detail::machine_def const&  def      = machine_access::Of(*binding_access::Of(subject.binding()).machine).def;
	for (size_t i = 0; i < inputs.size(); ++i)
	{
		bool const nan = def.inputs[i].type == column_type::Double && std::isnan(inputs[i].d);
		if (read[i] && !nan)
		{
			previous[i] = inputs[i];
		}
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

// Adds to the run's results the command of each clause of list that answered
// and emits, in clause order: a view into the version the binding holds, so
// nothing is copied, into room SizeFor made.
void Emit(instance& subject, trigger list)
{
	binding_access::Data const& binding = binding_access::Of(*instance_access::BindingOf(subject));
	std::vector<emitted>&       results = instance_access::Emitted(subject);
	size_t&                     count   = instance_access::EmittedCount(subject);
	machine_access::Data const&       machine = machine_access::Of(*binding.machine);
	std::vector<clause_answer> const& answers = instance_access::Answers(subject);
	std::vector<resolved_clause> const& clauses = machine.def.lists[size_t(list)];
	for (size_t i = 0; i < clauses.size(); ++i)
	{
		clause_answer const& answer = answers[binding.answerStart[size_t(list)] + i];
		if (clauses[i].emit == UINT32_MAX || answer.outcome != clause_outcome::Answered)
		{
			continue;
		}
		table_rows const& rows = version_access::Rows(*machine.lists[size_t(list)][i].version);
		std::string const& command = rows.commands[size_t(answer.row) * rows.commandCount + rows.slot[clauses[i].emit]];
		assert(count < results.size() && "more clauses emitted than Bind counted");
		results[count++] = emitted{list, clauses[i].table, command};
	}
}

// A run's results start empty. The binding a rebind left is let go here, at
// the next run, since its exit's commands were views into its versions.
void BeginResults(instance& subject)
{
	instance_access::EmittedCount(subject) = 0;
	instance_access::Left(subject)         = nullptr;
}

// Every buffer a step uses, sized for binding, so a step allocates nothing;
// the report-once memory cleared and every clause NotRun. The state vectors are
// the caller's: creation fills them with initial values, a rebind migrates.
void SizeFor(instance& subject, binding_access::Data const& data)
{
	instance_access::Inputs(subject).assign(data.getters.size(), cell::Integer(0));
	instance_access::InputRead(subject).assign(data.getters.size(), 0);
	instance_access::Answers(subject).assign(data.answerStart[kTriggerCount], clause_answer{});
	instance_access::Reported(subject).assign((data.reportBits + 63) / 64, 0);
	instance_access::Keys(subject).assign(data.maxKeys, cell::Integer(0));
	instance_access::Given(subject).assign(data.maxInputs, cell::Integer(0));
	instance_access::GivenBefore(subject).assign(data.maxInputs, cell::Integer(0));
	instance_access::Previous(subject).assign(data.getters.size(), cell::Real(std::numeric_limits<double>::quiet_NaN()));
	instance_access::Axes(subject).assign(data.maxAxes, 0.f);
	// Results already returned by this run (a rebind's exit) are kept.
	instance_access::Emitted(subject).resize(instance_access::EmittedCount(subject) + data.emitters);
	instance_access::Absorbed(subject) = false;
}

// Reads every input once (no message) and makes the values read the
// previous ones, so the lists run next cross no edge, as at creation. An
// input whose read failed, or a NaN, has no previous value.
void ReadStill(instance& subject, void const* self)
{
	ReadInputs(subject, self, message{});
	std::vector<cell>& previous = instance_access::Previous(subject);
	std::fill(previous.begin(), previous.end(), cell::Real(std::numeric_limits<double>::quiet_NaN()));
	AdvancePrevious(subject);
}

// The value a new state column takes from the old columns: the old column of
// its name, while that has the same type and, for a hash, its value is still
// a member; otherwise its initial value. Converting an int to a double, or the
// reverse, would be arithmetic the machine never wrote, so a type change
// resets.
cell Migrated(detail::state_column const& to, std::span<detail::state_column const> from, std::span<cell const> values)
{
	for (size_t s = 0; s < from.size(); ++s)
	{
		if (from[s].name != to.name)
		{
			continue;
		}
		if (from[s].type != to.type)
		{
			return to.initial;
		}
		if (to.type == column_type::Hash && !std::binary_search(to.members.begin(), to.members.end(), values[s].i))
		{
			return to.initial;
		}
		return values[s];
	}
	return to.initial;
}

// The previous value the old input of the new input's name and type had
// (raw: a changed range is the clamp's business where it is used); none for an
// input the old machine did not have, or a double it never read a value for
// (NaN). Only an input both machines read from a getter has a previous value
// that means the same thing: a message argument's is another message's, so an
// input that was one, or becomes one, is new. Only a double's previous value is
// ever read, since an edge reads only a band's input.
std::optional<cell> CarriedPrevious(detail::machine_def const& to, size_t input, detail::machine_def const& from, std::span<cell const> previous)
{
	if (to.carried[input] != detail::kFromGetter)
	{
		return std::nullopt;
	}
	input_def const& wanted = to.inputs[input];
	for (size_t i = 0; i < from.inputs.size(); ++i)
	{
		if (from.inputs[i].name != wanted.name || from.inputs[i].type != wanted.type || from.carried[i] != detail::kFromGetter)
		{
			continue;
		}
		if (wanted.type == column_type::Double && std::isnan(previous[i].d))
		{
			return std::nullopt;
		}
		return previous[i];
	}
	return std::nullopt;
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

std::span<emitted const> instance::Emitted() const
{
	assert(_binding && "a moved-from instance is used: caller misuse");
	return std::span<emitted const>(_emitted.data(), _emittedCount.n);
}

#if TABULAR_PROVENANCE
provenance_view instance::Provenance() const
{
	assert(_binding && "a moved-from instance is used: caller misuse");
	size_t const first = _provenance.empty() ? 0 : (_provenanceNext + _provenance.size() - _provenanceCount) % _provenance.size();
	return provenance_view(std::span<provenance_entry const>(_provenance.data(), _provenance.size()), first, _provenanceCount);
}
#endif

clause_answer instance::Answer(trigger list, size_t clause) const
{
	assert(_binding && "a moved-from instance is used: caller misuse");
	binding_access::Data const& binding = binding_access::Of(*_binding);
	return _answers[binding.answerStart[size_t(list)] + clause];
}

instance CreateInstance(Binding const& binding, uint64_t id, uint64_t seed, void const* self)
{
	binding_access::Data const& data    = binding_access::Of(binding);
	machine_access::Data const& machine = machine_access::Of(*data.machine);
	instance                    out     = instance_access::Make();
	instance_access::BindingOf(out)     = CountedWrap(&binding);
	instance_access::Id(out)            = id;
	std::vector<cell>& current          = instance_access::Current(out);
	for (detail::state_column const& column : machine.def.state)
	{
		current.push_back(column.initial);
	}
	instance_access::Next(out) = current;
	SizeFor(out, data);
#if TABULAR_PROVENANCE
	// Sized here only, so a rebind keeps the history it would otherwise lose.
	instance_access::Ring(out).assign(kProvenanceDepth, provenance_entry{});
#endif

	// Creation enters the initial state; nothing is left, so no exit runs. The
	// values read are their own previous values, so creation crosses no edge.
	ReadStill(out, self);
	std::vector<cell> const& next = instance_access::Next(out);
	RunList(out, trigger::Create, next, self, seed);
	RunList(out, trigger::Entry, next, self, seed);
	std::swap(instance_access::Current(out), instance_access::Next(out));
	Emit(out, trigger::Create);
	Emit(out, trigger::Entry);
	return out;
}

std::span<emitted const> Step(instance& subject, void const* self, uint64_t seed, message const& msg)
{
	// A moved-from instance has no binding, and stepping one is caller misuse.
	assert(instance_access::BindingOf(subject) && "a moved-from instance is stepped: caller misuse");
	machine_access::Data const& machine = machine_access::Of(*binding_access::Of(subject.binding()).machine);
	std::vector<cell>&          current = instance_access::Current(subject);
	std::vector<cell>&          next    = instance_access::Next(subject);
	instance_access::Absorbed(subject)  = false;
	BeginResults(subject);
#if TABULAR_PROVENANCE
	++instance_access::Pass(subject);
	instance_access::Run(subject) = provenance_run::Step;
#endif
	ReadInputs(subject, self, msg);
	// Unwritten columns hold because next starts as current.
	std::copy(current.begin(), current.end(), next.begin());
	RunList(subject, trigger::Step, next, self, seed);
	// Exit and entry run on one test; exit reads the state being left (current),
	// entry the state entered (next), and entry's writes land last.
	if (Entered(machine, current, next))
	{
		RunList(subject, trigger::Exit, current, self, seed);
		RunList(subject, trigger::Entry, next, self, seed);
	}
	else
	{
		MarkNotRun(subject, trigger::Exit);
		MarkNotRun(subject, trigger::Entry);
	}
	// Whatever the clauses did with them: the inputs were read.
	AdvancePrevious(subject);
	// Swapping two vectors moves their buffers, so nothing is allocated.
	std::swap(current, next);
	// In the order the lists ran.
	Emit(subject, trigger::Step);
	Emit(subject, trigger::Exit);
	Emit(subject, trigger::Entry);
	return subject.Emitted();
}

std::span<emitted const> Rebind(instance& subject, Binding const& binding, void const* self, uint64_t seed)
{
	assert(instance_access::BindingOf(subject) && "a moved-from instance is rebound: caller misuse");
	// Both bindings live to the end: holding fresh keeps a rebind onto the
	// binding subject already holds safe when the assignment below drops its
	// reference; old is what the exit and migration read until then.
	counted_ptr<Binding const> const old   = instance_access::BindingOf(subject);
	counted_ptr<Binding const> const fresh = CountedWrap(&binding);
	detail::machine_def const&       from  = machine_access::Of(old->machine()).def;
	detail::machine_def const&       to    = machine_access::Of(fresh->machine()).def;
	std::vector<cell>&               current = instance_access::Current(subject);
	std::vector<cell>&               next    = instance_access::Next(subject);
	BeginResults(subject);
#if TABULAR_PROVENANCE
	++instance_access::Pass(subject);
	instance_access::Run(subject) = provenance_run::Rebind;
#endif

	// Leaving, under the old binding, as a step leaves a state: the abort comes
	// from the code that started what the state runs (R6). The edge history is
	// kept aside, since a reload neither crosses an edge nor spends one.
	std::vector<cell> const history = instance_access::Previous(subject);
	ReadStill(subject, self);
	std::copy(current.begin(), current.end(), next.begin());
	RunList(subject, trigger::Exit, current, self, seed);
	std::swap(current, next);
	Emit(subject, trigger::Exit);

	// Migration by name, from the state exit left.
	std::vector<cell> migrated;
	migrated.reserve(to.state.size());
	for (detail::state_column const& column : to.state)
	{
		migrated.push_back(Migrated(column, from.state, current));
	}
	std::vector<std::optional<cell>> carried;
	carried.reserve(to.inputs.size());
	for (size_t i = 0; i < to.inputs.size(); ++i)
	{
		carried.push_back(CarriedPrevious(to, i, from, history));
	}
	// Exit's commands view the old binding's versions, so it is kept until the
	// next run, when the caller is done with them.
	instance_access::Left(subject)      = old;
	instance_access::BindingOf(subject) = fresh;
	SizeFor(subject, binding_access::Of(binding));
	current = migrated;
	next    = std::move(migrated);

	// Entering again, under the new binding.
	ReadStill(subject, self);
	RunList(subject, trigger::Entry, next, self, seed);
	std::swap(current, next);
	std::vector<cell>& previous = instance_access::Previous(subject);
	for (size_t i = 0; i < carried.size(); ++i)
	{
		if (carried[i])
		{
			previous[i] = *carried[i];
		}
	}
	Emit(subject, trigger::Entry);
	return subject.Emitted();
}

}
