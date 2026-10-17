#include "coverage.h"

#include <algorithm>

namespace tabular::detail
{

std::optional<std::vector<float>> Uncovered(std::span<interval const> domain, std::span<interval const> boxes,
	size_t count)
{
	size_t const dims = domain.size();
	// Cuts per dimension: the domain's ends and every box edge strictly inside.
	std::vector<std::vector<float>> cuts(dims);
	for (size_t d = 0; d < dims; ++d)
	{
		if (!(domain[d].lo < domain[d].hi))
		{
			return std::nullopt;   // an empty domain has no point to miss
		}
		cuts[d] = {domain[d].lo, domain[d].hi};
		for (size_t b = 0; b < count; ++b)
		{
			for (float edge : {boxes[b * dims + d].lo, boxes[b * dims + d].hi})
			{
				if (edge > domain[d].lo && edge < domain[d].hi)
				{
					cuts[d].push_back(edge);
				}
			}
		}
		std::sort(cuts[d].begin(), cuts[d].end());
		cuts[d].erase(std::unique(cuts[d].begin(), cuts[d].end()), cuts[d].end());
	}
	// Odometer over cells, dimension 0 most significant, so the first miss is
	// the lowest and a refusal names the same point every time.
	std::vector<size_t> cell(dims, 0);
	std::vector<float>  point(dims);
	while (true)
	{
		for (size_t d = 0; d < dims; ++d)
		{
			point[d] = cuts[d][cell[d]];
		}
		bool covered = false;
		for (size_t b = 0; b < count && !covered; ++b)
		{
			covered = true;
			for (size_t d = 0; d < dims && covered; ++d)
			{
				interval const& in = boxes[b * dims + d];
				covered = in.lo <= point[d] && point[d] < in.hi;
			}
		}
		if (!covered)
		{
			return point;
		}
		if (dims == 0)
		{
			return std::nullopt;
		}
		size_t d = dims;
		while (d > 0)
		{
			--d;
			if (++cell[d] + 1 < cuts[d].size())
			{
				break;
			}
			cell[d] = 0;
			if (d == 0)
			{
				return std::nullopt;
			}
		}
	}
}

}
