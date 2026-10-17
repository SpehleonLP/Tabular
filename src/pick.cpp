#include "tabular/pick.h"

#include "relation_internal.h"

#include <algorithm>

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
		if (def.inputs[i].type == column_type::Flags && (v < 0 || v > INT64_C(0xFFFFFFFF)))
		{
			return tl::make_unexpected(ErrorContext("input '", def.inputs[i].name, "' of '", def.name,
				"' is a flag set, a 32-bit value widened without sign; got ", v));
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
		float const x    = static_cast<float>(query.inputs[def.columns[c].input].d);
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

	auto firstHolding = [&](auto const& candidates) -> tl::expected<std::optional<size_t>, ErrorContext>
	{
		for (size_t r : candidates)
		{
			auto holds = RowHolds(version, r, query);
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
	};

	std::vector<size_t> inOrder;
	if (def.ranking == UINT32_MAX)
	{
		for (size_t r = run->first; r < run->last; ++r)
		{
			inOrder.push_back(r);
		}
		return firstHolding(inOrder);
	}

	uint32_t const F        = static_cast<uint32_t>(query.inputs[def.columns[def.ranking].input].i);
	uint32_t const rankSlot = rows.slot[def.ranking];
	struct ranked
	{
		size_t   row;
		bool     full;
		uint32_t hit;
		uint32_t required;
		uint16_t pivot;
	};
	std::vector<ranked> candidates;
	std::vector<size_t> defaults;
	for (size_t r = run->first; r < run->last; ++r)
	{
		uint32_t const R = static_cast<uint32_t>(rows.cells[r * rows.valueCount + rankSlot].i);
		if (R == 0)
		{
			defaults.push_back(r);
			continue;
		}
		uint32_t const hit = R & F;
		if (hit == 0)
		{
			continue;   // matches nothing in the input
		}
		candidates.push_back({r, hit == R, hit, R, rows.pivots[r]});
	}
	// Stable, so rows equal in every rank keep row order.
	std::stable_sort(candidates.begin(), candidates.end(), [](ranked const& a, ranked const& b)
	{
		if (a.full != b.full)         { return a.full; }
		if (a.hit != b.hit)           { return a.hit > b.hit; }
		if (a.required != b.required) { return a.required > b.required; }
		return a.pivot < b.pivot;
	});
	for (ranked const& c : candidates)
	{
		inOrder.push_back(c.row);
	}
	inOrder.insert(inOrder.end(), defaults.begin(), defaults.end());
	return firstHolding(inOrder);
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

}
