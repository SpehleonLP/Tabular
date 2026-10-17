#ifndef TABULAR_COVERAGE_H
#define TABULAR_COVERAGE_H

// Internal to Tabular: the load checks' one geometric question. Not a public header.

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace tabular::detail
{

struct interval
{
	float lo = 0;
	float hi = 0;   // half-open: lo <= x < hi
};

// The lowest point of domain (one interval per dimension, dimension 0 most
// significant) that none of count boxes contains, or none. boxes holds count
// boxes of domain.size() intervals each, one after another. Exact: every
// dimension is cut at every box edge inside the domain, so a cell lies wholly
// inside or outside each box, and its low corner speaks for it. With no
// dimensions the domain is one point, covered by any box.
std::optional<std::vector<float>> Uncovered(std::span<interval const> domain, std::span<interval const> boxes,
	size_t count);

}

#endif // TABULAR_COVERAGE_H
