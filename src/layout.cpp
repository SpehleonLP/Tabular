#include "layout.h"

#include <algorithm>

namespace tabular::detail
{

void LayOut(std::span<interval const> domain, std::span<interval const> boxes, std::span<layout_row const> rows,
	run_layout& out)
{
	size_t const dims = domain.size();
	out.dims = dims;
	out.groupFlags.clear();
	out.cutStart.clear();
	out.cuts.clear();
	out.cellStart.clear();
	out.listStart.clear();
	out.lists.clear();
	out.closed.clear();
	out.listed.assign(rows.size(), 0);

	for (layout_row const& row : rows)
	{
		out.groupFlags.push_back(row.flags);
	}
	std::sort(out.groupFlags.begin(), out.groupFlags.end());
	out.groupFlags.erase(std::unique(out.groupFlags.begin(), out.groupFlags.end()), out.groupFlags.end());

	bool domainEmpty = false;
	for (interval const& axis : domain)
	{
		domainEmpty = domainEmpty || !(axis.lo < axis.hi);
	}

	std::vector<uint32_t> members;   // the current group's rows, in row order
	std::vector<size_t>   index(dims);
	std::vector<float>    corner(dims);
	out.listStart.push_back(0);
	for (uint32_t const flags : out.groupFlags)
	{
		members.clear();
		for (size_t r = 0; r < rows.size(); ++r)
		{
			if (rows[r].flags == flags)
			{
				members.push_back(static_cast<uint32_t>(r));
			}
		}
		// Cuts per axis: the domain's ends and every edge of the group's boxes
		// strictly inside. Only this group's edges: another group's rows are
		// never tried in this group's lists, so their edges split nothing.
		size_t cells = domainEmpty ? 0 : 1;
		for (size_t a = 0; a < dims; ++a)
		{
			size_t const first = out.cuts.size();
			out.cutStart.push_back(static_cast<uint32_t>(first));
			out.cuts.push_back(domain[a].lo);
			out.cuts.push_back(domain[a].hi);
			for (uint32_t r : members)
			{
				for (float edge : {boxes[r * dims + a].lo, boxes[r * dims + a].hi})
				{
					if (edge > domain[a].lo && edge < domain[a].hi)
					{
						out.cuts.push_back(edge);
					}
				}
			}
			std::sort(out.cuts.begin() + first, out.cuts.end());
			out.cuts.erase(std::unique(out.cuts.begin() + first, out.cuts.end()), out.cuts.end());
			cells *= out.cuts.size() - first - 1;
		}
		out.cellStart.push_back(static_cast<uint32_t>(out.closed.size()));

		// Odometer over the group's cells, axis 0 most significant. A cell lies
		// wholly inside or outside each box, so its low corner speaks for it.
		size_t const axisBase = out.cutStart.size() - dims;
		std::fill(index.begin(), index.end(), 0);
		for (size_t c = 0; c < cells; ++c)
		{
			for (size_t a = 0; a < dims; ++a)
			{
				corner[a] = out.cuts[out.cutStart[axisBase + a] + index[a]];
			}
			bool closed = false;
			for (uint32_t r : members)
			{
				bool inside = true;
				for (size_t a = 0; a < dims && inside; ++a)
				{
					interval const& box = boxes[r * dims + a];
					inside = box.lo <= corner[a] && corner[a] < box.hi;
				}
				if (!inside)
				{
					continue;
				}
				out.lists.push_back(r);
				out.listed[r] = 1;
				// A row with no script always holds once its bands do, so the pick
				// stops here and nothing after it in the group is ever tried.
				if (!rows[r].scripted)
				{
					closed = true;
					break;
				}
			}
			out.closed.push_back(closed ? 1 : 0);
			out.listStart.push_back(static_cast<uint32_t>(out.lists.size()));
			for (size_t a = dims; a > 0; --a)
			{
				size_t const axisCuts = out.cutStart.size() > axisBase + a
					? out.cutStart[axisBase + a] - out.cutStart[axisBase + a - 1]
					: out.cuts.size() - out.cutStart[axisBase + a - 1];
				if (++index[a - 1] + 1 < axisCuts)
				{
					break;
				}
				index[a - 1] = 0;
			}
		}
	}
	out.cutStart.push_back(static_cast<uint32_t>(out.cuts.size()));
	out.cellStart.push_back(static_cast<uint32_t>(out.closed.size()));
}

size_t FindGroup(run_layout const& layout, uint32_t flags)
{
	auto found = std::lower_bound(layout.groupFlags.begin(), layout.groupFlags.end(), flags);
	if (found == layout.groupFlags.end() || *found != flags)
	{
		return SIZE_MAX;
	}
	return static_cast<size_t>(found - layout.groupFlags.begin());
}

std::optional<std::vector<float>> FirstOpen(run_layout const& layout, std::span<interval const> domain, size_t group)
{
	size_t const dims = domain.size();
	for (interval const& axis : domain)
	{
		if (!(axis.lo < axis.hi))
		{
			return std::nullopt;   // an empty domain has no input to miss
		}
	}
	std::vector<float> point(dims);
	if (group == SIZE_MAX)
	{
		for (size_t a = 0; a < dims; ++a)
		{
			point[a] = domain[a].lo;
		}
		return point;
	}
	uint32_t const first = layout.cellStart[group];
	uint32_t const last  = layout.cellStart[group + 1];
	for (uint32_t c = first; c < last; ++c)
	{
		if (layout.closed[c])
		{
			continue;
		}
		// Back from the cell's number to its corner: the odometer's digits,
		// last axis least significant.
		size_t rest = c - first;
		for (size_t a = dims; a > 0; --a)
		{
			uint32_t const begin = layout.cutStart[group * dims + a - 1];
			uint32_t const end   = layout.cutStart[group * dims + a];
			size_t const   span  = end - begin - 1;
			point[a - 1]         = layout.cuts[begin + rest % span];
			rest /= span;
		}
		return point;
	}
	return std::nullopt;
}

}
