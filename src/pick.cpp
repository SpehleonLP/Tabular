#include "tabular/pick.h"

#include "relation_internal.h"
#include "tabular/draw.h"

#include <algorithm>
#include <cmath>
#include <tuple>

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
	for (uint32_t i = 0; i < def.inputs.size(); ++i)
	{
		int64_t const v = query.inputs[i].i;
		if (def.inputs[i].type == column_type::Flags && !Fits32(v))
		{
			return tl::make_unexpected(ErrorContext("input '", def.inputs[i].name, "' of '", def.name,
				"' is a flag set, a 32-bit value widened without sign; got ", v));
		}
		// A clamp has no place for NaN, in any policy: the range declaration
		// is what refuses it, not the pick.
		if (def.inputs[i].ranged && std::isnan(query.inputs[i].d))
		{
			return tl::make_unexpected(ErrorContext("input '", def.inputs[i].name, "' of '", def.name,
				"' is NaN; a ranged input is clamped into its range, which has no place for it"));
		}
	}
	if (def.key_count == 0)
	{
		return row_range{0, version.RowCount()};
	}
	return version.FindOrdered(query.keys);
}

// Bands first, so a script runs only for a row whose native conditions hold.
// Floats, as animation's phase bands compare.
tl::expected<bool, ErrorContext> RowHolds(TableVersion const& version, size_t row, pick_query const& query)
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
		input_def const& in = def.inputs[def.columns[c].input];
		double const     d  = query.inputs[def.columns[c].input].d;
		float            x  = 0;
		// Clamped in double, so a value past float's range never narrows;
		// hi is excluded, so the top value lands in the top band. NaN was
		// refused with the query.
		if (in.ranged)
		{
			double const top = std::nextafter(in.hi, -INFINITY);
			x = static_cast<float>(std::clamp(d, double(in.lo), top));
		}
		else
		{
			x = static_cast<float>(d);
		}
		if (!(x >= band.v[0] && x < band.v[1]))
		{
			return false;
		}
	}
	for (uint32_t c = 0; c < def.columns.size(); ++c)
	{
		if (def.columns[c].type != column_type::Condition)
		{
			continue;
		}
		auto holds = version.Holds(row, c, query.this_in);
		if (!holds)
		{
			return tl::make_unexpected(std::move(holds.error()).with("table '", def.name, "' row ", row));
		}
		if (!*holds)
		{
			return false;
		}
	}
	return true;
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
	auto tryRow = [&](size_t r) -> tl::expected<bool, ErrorContext> { return RowHolds(version, r, query); };

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
	// Larger is better: a full match, then R & F, then R, then the earlier
	// row. The last makes it a total order, so rows are tried best first by
	// scanning for the best below the last one tried; the first try usually
	// holds.
	using rank = std::tuple<bool, uint32_t, uint32_t, size_t>;
	auto rankOf = [&](size_t r) -> std::optional<rank>
	{
		uint32_t const R   = static_cast<uint32_t>(rows.cells[r * rows.valueCount + rankSlot].i);
		uint32_t const hit = R & F;
		if (hit == 0)
		{
			return std::nullopt;   // the default, or a row matching nothing in the input
		}
		return rank{hit == R, hit, R, SIZE_MAX - r};
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
		auto holds = RowHolds(version, r, query);
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
	table_def const&  def  = version_access::Def(version);
	table_rows const& rows = version_access::Rows(version);
	uint32_t const    slot = rows.slot[def.output];
	uint32_t          bits = 0;
	for (size_t r = run->first; r < run->last; ++r)
	{
		auto holds = RowHolds(version, r, query);
		if (!holds)
		{
			return tl::make_unexpected(std::move(holds.error()));
		}
		if (*holds)
		{
			bits |= static_cast<uint32_t>(rows.cells[r * rows.valueCount + slot].i);
		}
	}
	return bits;
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
	table_def const&  def  = version_access::Def(version);
	table_rows const& rows = version_access::Rows(version);
	uint32_t const    slot = rows.slot[def.weight];
	auto weightOf = [&](size_t r) { return rows.cells[r * rows.valueCount + slot].d; };

	// Summed in row order, as Load summed it to prove the total finite.
	double total = 0;
	for (size_t r = run->first; r < run->last; ++r)
	{
		total += weightOf(r);
	}
	double const target = DrawUnit(Draw(seed, site)) * total;
	double       edge   = 0;
	for (size_t r = run->first; r < run->last; ++r)
	{
		edge += weightOf(r);
		if (target < edge)
		{
			return std::optional<size_t>(r);
		}
	}
	// Not reached: Load admits only normal positive weights, so total is a normal
	// double, and the unit is at most 1 - 2^-53, which puts the exact product at
	// least half an ulp below total and rounds it below total. The loop above
	// sums in the order that built total, so its last edge is total and some
	// row takes the target. A subnormal total could round the product up to
	// total, which is why Load refuses subnormal weights. The return keeps a
	// corrupt version from running off the run.
	return std::optional<size_t>(run->last - 1);
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
