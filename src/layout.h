#ifndef TABULAR_LAYOUT_H
#define TABULAR_LAYOUT_H

// Internal to Tabular: a key run laid out as the cells a pick can tell apart,
// each listing the rows the pick would try. Load proves totality and dead rows
// by reading it, and a machine's Prepare keeps it as the compiled pick. Not a
// public header.

#include "Support/shared_array.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
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

// One row of a run as the layout reads it. The row's box is given beside it.
struct layout_row
{
	uint32_t flags       = 0;      // the ranking column's value; 0 in a table that does not rank
	// It may not hold where its bands do, so a pick may fall past it: it
	// carries a script condition or an edge. Totality and dead rows read only
	// this, so the two are one kind of row to the proof.
	bool     conditional = false;
};

// A key run's rows grouped by flag set, ascending, since only equal flags
// shadow each other: which group a pick tries first depends on the input
// flags, but within a group the rows are tried in row order. Each group's
// input space is cut at every band edge of the group's rows and at the
// domain's ends, so a cell lies wholly inside or outside every row's box and
// one list answers for every input in it. A cell's list holds the group's rows
// whose boxes contain it, in row order, ending at the first that is not
// conditional (no script, no edge).
//
// One layout holds any number of runs laid one after another, each a span of
// groups (run_span), so a machine's clause keeps every run of its domain in
// one set of arrays and Load reuses one for run after run. Every run in one
// layout has the same axes. Flat, so a compiled pick reads it without chasing
// pointers: offsets into shared arrays, never a vector per cell.
struct run_layout
{
	size_t                dims = 0;
	std::vector<uint32_t> groupFlags;   // per group
	std::vector<uint32_t> cutStart;     // per (group, axis), then one end: offsets into cuts
	std::vector<float>    cuts;         // per (group, axis): ascending, domain ends included
	std::vector<uint32_t> cellStart;    // per group, then one end: the group's first cell
	std::vector<uint32_t> listStart;    // per cell, then one end: offsets into lists
	std::vector<uint32_t> lists;        // row numbers, as LayOut was given them
	// Proof only: a pick never reads these, so a kept layout may drop them.
	std::vector<uint8_t>  closed;       // per cell: its list ends in a row that is not conditional
	std::vector<uint8_t>  listed;       // per row of the run laid last: it appears in some list of its group
};

// Where one run's groups sit in a layout: [first, first + count).
struct run_span
{
	uint32_t first = 0;
	uint32_t count = 0;
};

// Where one run's rows sit in a version: rows [first, first + count). A table
// picked without a layout (or, weighted) is kept as these.
struct row_span
{
	uint32_t first = 0;
	uint32_t count = 0;
};

// What LayOut needs while it works and does not keep. A caller laying run
// after run passes the same one, so the work allocates nothing once warm.
struct layout_scratch
{
	std::vector<uint32_t> members;   // the current group's rows, in run order
	std::vector<size_t>   index;     // odometer digits, one per axis
	std::vector<float>    corner;    // the current cell's low corner
};

// The runs a machine's clause keeps: one layout and, per run laid into it in
// domain order, where its groups are.
struct kept_runs
{
	run_layout            layout;
	std::vector<run_span> runs;
};

// What a prepared clause keeps of kept_runs for its life: the same arrays,
// exact-size and immutable, without the proof-only ones. Steps read it through
// the same member names, so FindGroup and the pick take either.
struct frozen_layout
{
	size_t                    dims = 0;
	immutable_array<uint32_t> groupFlags;
	immutable_array<uint32_t> cutStart;
	immutable_array<float>    cuts;
	immutable_array<uint32_t> cellStart;
	immutable_array<uint32_t> listStart;
	immutable_array<uint32_t> lists;
};

struct frozen_runs
{
	frozen_layout             layout;
	immutable_array<run_span> runs;
};

// Copies kept's arrays at their exact sizes; a Prepare-time cost, paid once
// per clause.
frozen_runs Freeze(kept_runs const& kept);

// Empties layout, keeping its allocations: no runs, the "one end" entries at 0.
void Clear(run_layout& layout);

// Appends one run of count rows whose boxes (domain.size() intervals each, one
// row after another, already cut down to the domain) and flags are given in
// run order; its lists name row r as firstRow + r, so a version's row numbers
// go in unchanged. Cells run in odometer order with axis 0 most significant,
// so the first unclosed cell of a group holds the lowest input that group
// misses. A layout default-constructed or Cleared takes its axes from the
// first run.
run_span LayOut(std::span<interval const> domain, std::span<interval const> boxes, std::span<layout_row const> rows,
	uint32_t firstRow, run_layout& out, layout_scratch& scratch);

// The group of run whose rows have flags, as an index into the layout, or
// SIZE_MAX when no row of the run has them.
template<typename Layout>
size_t FindGroup(Layout const& layout, run_span run, uint32_t flags)
{
	auto const begin = layout.groupFlags.begin() + run.first;
	auto const end   = begin + run.count;
	auto       found = std::lower_bound(begin, end, flags);
	if (found == end || *found != flags)
	{
		return SIZE_MAX;
	}
	return static_cast<size_t>(found - layout.groupFlags.begin());
}

// The lowest input of domain that group's lists leave open (no list there ends
// in a row that is not conditional), or none. A missing group (SIZE_MAX) closes
// nothing, so its answer is the domain's low corner. An empty domain has no
// input to leave open; with no axes the domain is one point.
std::optional<std::vector<float>> FirstOpen(run_layout const& layout, std::span<interval const> domain, size_t group);

}

#endif // TABULAR_LAYOUT_H
