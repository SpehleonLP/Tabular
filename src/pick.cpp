#include "tabular/pick.h"

#include "pick_internal.h"
#include "relation_internal.h"
#include "tabular/draw.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <utility>
#include <vector>

namespace tabular
{

namespace
{

char const* PolicyName(pick_policy p)
{
	switch (p)
	{
	case pick_policy::None: return "none";
	case pick_policy::Best: return "best";
	case pick_policy::All:  return "all";
	case pick_policy::Or:   return "or";
	case pick_policy::Weighted: return "weighted";
	}
	return "unknown";
}

// The key run the query selects, once the query matches the table's shape.
tl::expected<row_range, ErrorContext> RunOf(TableVersion const& version, pick_query const& query, pick_policy wanted)
{
	table_def const& def = version_access::Def(version);
	if (def.pick != wanted)
	{
		return tl::make_unexpected(ErrorContext("table '", def.name, "' is a ", PolicyName(def.pick),
			" table, not picked with ", PolicyName(wanted)));
	}
	if (query.keys.size() != def.key_count)
	{
		return tl::make_unexpected(ErrorContext("a pick of '", def.name, "' with ", query.keys.size(),
			" keys; the table has ", def.key_count));
	}
	if (query.inputs.size() != def.inputs.size())
	{
		return tl::make_unexpected(ErrorContext("a pick of '", def.name, "' with ", query.inputs.size(),
			" inputs; the table declares ", def.inputs.size()));
	}
	detail::bad_input const bad = detail::FirstBadInput(def, query.inputs);
	if (bad.index != UINT32_MAX)
	{
		return tl::make_unexpected(detail::BadInputError(def, bad, query.inputs));
	}
	if (def.key_count == 0)
	{
		return row_range{0, version.RowCount()};
	}
	uint32_t const messageKey = detail::MessageKeyOf(def);
	// The retry's keys, on the stack for any table of up to kInlineKeys keys:
	// a pick allocates nothing for the fall-through, as a step does not.
	constexpr size_t           kInlineKeys = 16;
	std::array<cell, kInlineKeys> inlineKeys;
	std::vector<cell>          wideKeys;
	std::span<cell>            scratch(inlineKeys.data(), std::min(query.keys.size(), kInlineKeys));
	if (messageKey != UINT32_MAX && query.keys.size() > kInlineKeys)
	{
		wideKeys.resize(query.keys.size());
		scratch = wideKeys;
	}
	return detail::FindFallingThrough(query.keys, scratch, messageKey,
		[&](std::span<cell const> keys) { return version.FindOrdered(keys); },
		[](tl::expected<row_range, ErrorContext> const& run) { return run && run->first == run->last; });
}

// Bands first, so a script runs only for a row whose native conditions hold.
// Floats, as animation's phase bands compare. error as ScriptHolds.
detail::row_test RowHolds(TableVersion const& version, size_t row, pick_query const& query, ErrorContext& error)
{
	table_def const&  def  = version_access::Def(version);
	table_rows const& rows = version_access::Rows(version);
	for (uint32_t c = 0; c < def.columns.size(); ++c)
	{
		if (def.columns[c].type != column_type::Band)
		{
			continue;
		}
		cell const  band = rows.cells[row * rows.valueCount + rows.slot[c]];
		float const x    = detail::ClampedInput(def.inputs[def.columns[c].input], query.inputs[def.columns[c].input].d);
		if (!(x >= band.v[0] && x < band.v[1]))
		{
			return detail::row_test::Misses;
		}
	}
	return detail::ScriptHolds(version, row, query.this_in, error);
}

// A row's failure, as the interpreted picks refuse with it: the host's error,
// naming the table and row.
ErrorContext RowError(TableVersion const& version, size_t row, ErrorContext&& error)
{
	return std::move(error).with("table '", version_access::Def(version).name, "' row ", row);
}

// The interpreted picks' test of a row: a failure is an error naming the
// table and row.
tl::expected<bool, ErrorContext> CheckRow(TableVersion const& version, size_t row, pick_query const& query)
{
	ErrorContext           error;
	detail::row_test const test = RowHolds(version, row, query, error);
	if (test == detail::row_test::Failed)
	{
		return tl::make_unexpected(RowError(version, row, std::move(error)));
	}
	return test == detail::row_test::Holds;
}

}

namespace detail
{

float ClampedInput(input_def const& in, double value)
{
	if (!in.ranged)
	{
		return static_cast<float>(value);
	}
	double const top = std::nextafter(in.hi, -INFINITY);
	return static_cast<float>(std::clamp(value, double(in.lo), top));
}

bad_input FirstBadInput(table_def const& def, std::span<cell const> inputs)
{
	for (uint32_t i = 0; i < def.inputs.size(); ++i)
	{
		if (def.inputs[i].type == column_type::Flags && !Fits32(inputs[i].i))
		{
			return bad_input{i, false};
		}
		// A clamp has no place for NaN, in any policy: the range declaration
		// is what refuses it, not the pick.
		if (def.inputs[i].ranged && std::isnan(inputs[i].d))
		{
			return bad_input{i, true};
		}
	}
	return {};
}

ErrorContext BadInputError(table_def const& def, bad_input bad, std::span<cell const> inputs)
{
	if (bad.nan)
	{
		return ErrorContext("input '", def.inputs[bad.index].name, "' of '", def.name,
			"' is NaN; a ranged input is clamped into its range, which has no place for it");
	}
	return ErrorContext("input '", def.inputs[bad.index].name, "' of '", def.name,
		"' is a flag set, a 32-bit value widened without sign; got ", inputs[bad.index].i);
}

row_test ScriptHolds(TableVersion const& version, size_t row, void const* this_in, ErrorContext& error)
{
	table_def const&  def  = version_access::Def(version);
	table_rows const& rows = version_access::Rows(version);
	for (uint32_t c = 0; c < def.columns.size(); ++c)
	{
		if (def.columns[c].type != column_type::Condition)
		{
			continue;
		}
		bool const holds = CallCondition(rows, row, c, this_in, error);
		// An error may arrive as context alone, from a closure wrapping another's.
		if (!error.empty())
		{
			return row_test::Failed;
		}
		if (!holds)
		{
			return row_test::Misses;
		}
	}
	return row_test::Holds;
}

std::optional<uint32_t> OrOfRows(TableVersion const& version, size_t first, size_t last, pick_query const& query,
	ErrorContext& error, size_t& failed_row)
{
	table_def const&  def  = version_access::Def(version);
	table_rows const& rows = version_access::Rows(version);
	uint32_t const    slot = rows.slot[def.output];
	uint32_t          bits = 0;
	for (size_t r = first; r < last; ++r)
	{
		row_test const test = RowHolds(version, r, query, error);
		if (test == row_test::Failed)
		{
			failed_row = r;
			return std::nullopt;
		}
		if (test == row_test::Holds)
		{
			bits |= static_cast<uint32_t>(rows.cells[r * rows.valueCount + slot].i);
		}
	}
	return bits;
}

uint32_t MessageKeyOf(table_def const& def)
{
	// Domains are one per key column, in column order, as a picked table's
	// keys are.
	for (uint32_t k = 0; k < def.domains.size(); ++k)
	{
		if (def.domains[k].kind == domain_kind::Message)
		{
			return k;
		}
	}
	return UINT32_MAX;
}

size_t DrawRow(TableVersion const& version, size_t first, size_t last, uint64_t seed, uint64_t site)
{
	table_def const&  def  = version_access::Def(version);
	table_rows const& rows = version_access::Rows(version);
	uint32_t const    slot = rows.slot[def.weight];
	auto weightOf = [&](size_t r) { return rows.cells[r * rows.valueCount + slot].d; };

	// Summed in row order, as Load summed it to prove the total finite.
	double total = 0;
	for (size_t r = first; r < last; ++r)
	{
		total += weightOf(r);
	}
	double const target = DrawUnit(Draw(seed, site)) * total;
	double       edge   = 0;
	for (size_t r = first; r < last; ++r)
	{
		edge += weightOf(r);
		if (target < edge)
		{
			return r;
		}
	}
	// Not reached: Load admits only normal positive weights, so total is a normal
	// double, and the unit is at most 1 - 2^-53, which puts the exact product at
	// least half an ulp below total and rounds it below total. The loop above
	// sums in the order that built total, so its last edge is total and some
	// row takes the target. A subnormal total could round the product up to
	// total, which is why Load refuses subnormal weights. The return keeps a
	// corrupt version from running off the run.
	return last - 1;
}

}

tl::expected<std::optional<size_t>, ErrorContext> PickBest(TableVersion const& version, pick_query const& query)
{
	auto run = RunOf(version, query, pick_policy::Best);
	if (!run)
	{
		return tl::make_unexpected(std::move(run.error()));
	}
	table_def const&  def  = version_access::Def(version);
	table_rows const& rows = version_access::Rows(version);

	// Each candidate tried is a script call at most; nothing here allocates,
	// since picks run per frame on a tight budget.
	auto tryRow = [&](size_t r) { return CheckRow(version, r, query); };

	if (def.ranking == UINT32_MAX)
	{
		for (size_t r = run->first; r < run->last; ++r)
		{
			auto holds = tryRow(r);
			if (!holds)
			{
				return tl::make_unexpected(std::move(holds.error()));
			}
			if (*holds)
			{
				return std::optional<size_t>(r);
			}
		}
		return std::optional<size_t>();
	}

	uint32_t const F        = static_cast<uint32_t>(query.inputs[def.columns[def.ranking].input].i);
	uint32_t const rankSlot = rows.slot[def.ranking];
	// Larger is better: the shared flag rank, then the earlier row. The last
	// makes it a total order, so rows are tried best first by scanning for the
	// best below the last one tried; the first try usually holds.
	using rank = std::pair<detail::flag_rank, size_t>;
	auto rankOf = [&](size_t r) -> std::optional<rank>
	{
		uint32_t const R = static_cast<uint32_t>(rows.cells[r * rows.valueCount + rankSlot].i);
		if (!detail::Ranked(R, F))
		{
			return std::nullopt;   // the default, or a row matching nothing in the input
		}
		return rank{detail::RankOf(R, F), SIZE_MAX - r};
	};
	std::optional<rank> below;
	while (true)
	{
		std::optional<size_t> best;
		std::optional<rank>   bestRank;
		for (size_t r = run->first; r < run->last; ++r)
		{
			auto k = rankOf(r);
			if (k && (!below || *k < *below) && (!bestRank || *bestRank < *k))
			{
				best     = r;
				bestRank = k;
			}
		}
		if (!best)
		{
			break;
		}
		auto holds = tryRow(*best);
		if (!holds)
		{
			return tl::make_unexpected(std::move(holds.error()));
		}
		if (*holds)
		{
			return best;
		}
		below = bestRank;
	}
	// The R == 0 rows, in row order.
	for (size_t r = run->first; r < run->last; ++r)
	{
		if (rows.cells[r * rows.valueCount + rankSlot].i != 0)
		{
			continue;
		}
		auto holds = tryRow(r);
		if (!holds)
		{
			return tl::make_unexpected(std::move(holds.error()));
		}
		if (*holds)
		{
			return std::optional<size_t>(r);
		}
	}
	return std::optional<size_t>();
}

tl::expected<std::vector<size_t>, ErrorContext> PickAll(TableVersion const& version, pick_query const& query)
{
	auto run = RunOf(version, query, pick_policy::All);
	if (!run)
	{
		return tl::make_unexpected(std::move(run.error()));
	}
	std::vector<size_t> out;
	for (size_t r = run->first; r < run->last; ++r)
	{
		auto holds = CheckRow(version, r, query);
		if (!holds)
		{
			return tl::make_unexpected(std::move(holds.error()));
		}
		if (*holds)
		{
			out.push_back(r);
		}
	}
	return out;
}

tl::expected<uint32_t, ErrorContext> PickOr(TableVersion const& version, pick_query const& query)
{
	auto run = RunOf(version, query, pick_policy::Or);
	if (!run)
	{
		return tl::make_unexpected(std::move(run.error()));
	}
	ErrorContext error;
	size_t       failed = SIZE_MAX;
	auto         bits   = detail::OrOfRows(version, run->first, run->last, query, error, failed);
	if (!bits)
	{
		return tl::make_unexpected(RowError(version, failed, std::move(error)));
	}
	return *bits;
}

tl::expected<std::optional<size_t>, ErrorContext> PickWeighted(TableVersion const& version, pick_query const& query,
	uint64_t seed, uint64_t site)
{
	auto run = RunOf(version, query, pick_policy::Weighted);
	if (!run)
	{
		return tl::make_unexpected(std::move(run.error()));
	}
	if (run->first == run->last)
	{
		return std::optional<size_t>();
	}
	return std::optional<size_t>(detail::DrawRow(version, run->first, run->last, seed, site));
}

tl::expected<std::optional<size_t>, ErrorContext> PickWeighted(relation_store const& store, uint32_t table,
	pick_query const& query, uint64_t seed, uint64_t site)
{
	auto version = store.Version(table);
	if (!version)
	{
		return tl::make_unexpected(std::move(version.error()));
	}
	return PickWeighted(**version, query, seed, site);
}

tl::expected<std::optional<size_t>, ErrorContext> PickBest(relation_store const& store, uint32_t table, pick_query const& query)
{
	auto version = store.Version(table);
	if (!version)
	{
		return tl::make_unexpected(std::move(version.error()));
	}
	return PickBest(**version, query);
}

tl::expected<std::vector<size_t>, ErrorContext> PickAll(relation_store const& store, uint32_t table, pick_query const& query)
{
	auto version = store.Version(table);
	if (!version)
	{
		return tl::make_unexpected(std::move(version.error()));
	}
	return PickAll(**version, query);
}

tl::expected<uint32_t, ErrorContext> PickOr(relation_store const& store, uint32_t table, pick_query const& query)
{
	auto version = store.Version(table);
	if (!version)
	{
		return tl::make_unexpected(std::move(version.error()));
	}
	return PickOr(**version, query);
}

}
