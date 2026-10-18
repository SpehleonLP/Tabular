#include <gtest/gtest.h>
#include "../src/layout.h"

#include <optional>
#include <span>
#include <vector>

using tabular::detail::FindGroup;
using tabular::detail::FirstOpen;
using tabular::detail::interval;
using tabular::detail::Clear;
using tabular::detail::LayOut;
using tabular::detail::layout_row;
using tabular::detail::layout_scratch;
using tabular::detail::run_layout;
using tabular::detail::run_span;

namespace
{

// The lowest input the rows with no script and no flags leave open: the
// totality question, asked of the layout.
std::optional<std::vector<float>> Uncovered(std::span<interval const> domain, std::span<interval const> boxes,
	size_t count)
{
	std::vector<layout_row> rows(count);
	run_layout              layout;
	layout_scratch          scratch;
	run_span const          run = LayOut(domain, boxes, rows, 0, layout, scratch);
	return FirstOpen(layout, domain, FindGroup(layout, run, 0));
}

std::vector<uint32_t> ListAt(run_layout const& layout, size_t group, size_t cell)
{
	uint32_t const c = layout.cellStart[group] + static_cast<uint32_t>(cell);
	return std::vector<uint32_t>(layout.lists.begin() + layout.listStart[c], layout.lists.begin() + layout.listStart[c + 1]);
}

}

TEST(Coverage, OneDimensionFindsTheGap)
{
	interval const domain[] = {{0.0f, 1.0f}};
	interval const boxes[]  = {{0.0f, 0.2f}, {0.4f, 1.0f}};
	auto hole = Uncovered(domain, boxes, 2);
	ASSERT_TRUE(hole.has_value());
	EXPECT_EQ(*hole, (std::vector<float>{0.2f}));
}

TEST(Coverage, TouchingBandsLeaveNoSeam)
{
	interval const domain[] = {{0.0f, 1.0f}};
	interval const boxes[]  = {{0.5f, 1.0f}, {0.0f, 0.5f}};
	EXPECT_FALSE(Uncovered(domain, boxes, 2).has_value());
}

TEST(Coverage, BoxesReachingPastTheDomainCoverIt)
{
	interval const domain[] = {{0.25f, 0.75f}};
	interval const boxes[]  = {{0.0f, 1.0f}};
	EXPECT_FALSE(Uncovered(domain, boxes, 1).has_value());
}

TEST(Coverage, TwoDimensionsFindTheCornerOfAnLShapedHole)
{
	interval const domain[] = {{0.0f, 1.0f}, {0.0f, 1.0f}};
	// Everything but [0.5, 1) x [0.5, 1).
	interval const boxes[] = {{0.0f, 0.5f}, {0.0f, 1.0f},
	                          {0.5f, 1.0f}, {0.0f, 0.5f}};
	auto hole = Uncovered(domain, boxes, 2);
	ASSERT_TRUE(hole.has_value());
	EXPECT_EQ(*hole, (std::vector<float>{0.5f, 0.5f}));
}

TEST(Coverage, NoDimensionsIsCoveredByAnyBox)
{
	EXPECT_TRUE(Uncovered({}, {}, 0).has_value());
	EXPECT_FALSE(Uncovered({}, {}, 1).has_value());
}

TEST(Coverage, AnEmptyDomainIsCovered)
{
	interval const domain[] = {{0.5f, 0.5f}};
	EXPECT_FALSE(Uncovered(domain, {}, 0).has_value());
}

TEST(Coverage, DimensionZeroIsMostSignificant)
{
	interval const domain[] = {{0.0f, 1.0f}, {0.0f, 1.0f}};
	// Holes at (0, 0.5) and (0.5, 0): the lowest in dimension 0 first.
	interval const boxes[] = {{0.0f, 0.5f}, {0.0f, 0.5f},
	                          {0.5f, 1.0f}, {0.5f, 1.0f}};
	auto hole = Uncovered(domain, boxes, 2);
	ASSERT_TRUE(hole.has_value());
	EXPECT_EQ(*hole, (std::vector<float>{0.0f, 0.5f}));
}

TEST(Coverage, AListHoldsTheRowsTriedAndStopsAtTheFirstWithNoScript)
{
	interval const domain[] = {{0.0f, 1.0f}};
	// Row 0 scripted over the whole range, row 1 plain over [0, 0.5), row 2
	// plain over the whole range: on [0, 0.5) the pick tries 0 then 1 and never
	// reaches 2; on [0.5, 1) it tries 0 then 2.
	interval const   boxes[] = {{0.0f, 1.0f}, {0.0f, 0.5f}, {0.0f, 1.0f}};
	layout_row const rows[]  = {{0, true}, {0, false}, {0, false}};
	run_layout       layout;
	layout_scratch   scratch;
	LayOut(domain, boxes, rows, 0, layout, scratch);
	ASSERT_EQ(layout.groupFlags, (std::vector<uint32_t>{0}));
	ASSERT_EQ(layout.cellStart[1] - layout.cellStart[0], 2u);
	EXPECT_EQ(ListAt(layout, 0, 0), (std::vector<uint32_t>{0, 1}));
	EXPECT_EQ(ListAt(layout, 0, 1), (std::vector<uint32_t>{0, 2}));
	EXPECT_EQ(layout.closed, (std::vector<uint8_t>{1, 1}));
	EXPECT_EQ(layout.listed, (std::vector<uint8_t>{1, 1, 1}));
}

TEST(Coverage, ARowEveryEarlierPlainRowCoversIsInNoList)
{
	interval const   domain[] = {{0.0f, 1.0f}};
	interval const   boxes[]  = {{0.0f, 1.0f}, {0.25f, 0.5f}};
	layout_row const rows[]   = {{0, false}, {0, false}};
	run_layout       layout;
	layout_scratch   scratch;
	LayOut(domain, boxes, rows, 0, layout, scratch);
	EXPECT_EQ(layout.listed, (std::vector<uint8_t>{1, 0}));
}

TEST(Coverage, GroupsAreByFlagSetAndCutOnlyAtTheirOwnEdges)
{
	interval const domain[] = {{0.0f, 1.0f}};
	// The flagged row would shadow the default if groups mixed; they do not, and
	// its edge at 0.3 does not split the default group's one cell.
	interval const   boxes[] = {{0.0f, 0.3f}, {0.0f, 1.0f}};
	layout_row const rows[]  = {{4, false}, {0, false}};
	run_layout       layout;
	layout_scratch   scratch;
	run_span const   run = LayOut(domain, boxes, rows, 0, layout, scratch);
	ASSERT_EQ(layout.groupFlags, (std::vector<uint32_t>{0, 4}));
	EXPECT_EQ(layout.cellStart[1] - layout.cellStart[0], 1u) << "the default group has one cell";
	EXPECT_EQ(layout.cellStart[2] - layout.cellStart[1], 2u) << "the flagged group is cut at 0.3";
	EXPECT_EQ(layout.listed, (std::vector<uint8_t>{1, 1}));
	EXPECT_FALSE(FirstOpen(layout, domain, FindGroup(layout, run, 0)).has_value());
	auto open = FirstOpen(layout, domain, FindGroup(layout, run, 4));
	ASSERT_TRUE(open.has_value());
	EXPECT_EQ(*open, (std::vector<float>{0.3f}));
	EXPECT_EQ(FindGroup(layout, run, 2), SIZE_MAX);
}

TEST(Coverage, RunsAppendWithTheirOwnGroupsAndTheRowNumbersGiven)
{
	interval const domain[] = {{0.0f, 1.0f}};
	// The first run (rows 10, 11) is total; the second (rows 20, 21) leaves
	// [0.5, 1) to a scripted row and has a flagged group of its own.
	interval const   firstBoxes[]  = {{0.0f, 0.5f}, {0.5f, 1.0f}};
	layout_row const firstRows[]   = {{0, false}, {0, false}};
	interval const   secondBoxes[] = {{0.0f, 0.5f}, {0.5f, 1.0f}, {0.0f, 1.0f}};
	layout_row const secondRows[]  = {{0, false}, {0, true}, {2, false}};
	run_layout       layout;
	layout_scratch   scratch;
	run_span const   first  = LayOut(domain, firstBoxes, firstRows, 10, layout, scratch);
	run_span const   second = LayOut(domain, secondBoxes, secondRows, 20, layout, scratch);
	EXPECT_EQ(first.first, 0u);
	EXPECT_EQ(first.count, 1u);
	EXPECT_EQ(second.first, 1u);
	EXPECT_EQ(second.count, 2u);
	EXPECT_EQ(layout.groupFlags, (std::vector<uint32_t>{0, 0, 2}));
	EXPECT_EQ(ListAt(layout, 0, 0), (std::vector<uint32_t>{10}));
	EXPECT_EQ(ListAt(layout, 0, 1), (std::vector<uint32_t>{11}));
	EXPECT_EQ(ListAt(layout, 1, 1), (std::vector<uint32_t>{21})) << "version row numbers, not run-relative";
	EXPECT_EQ(ListAt(layout, 2, 0), (std::vector<uint32_t>{22}));
	EXPECT_FALSE(FirstOpen(layout, domain, FindGroup(layout, first, 0)).has_value()) << "the first run is untouched";
	auto open = FirstOpen(layout, domain, FindGroup(layout, second, 0));
	ASSERT_TRUE(open.has_value()) << "FirstOpen reads the second run at its own base";
	EXPECT_EQ(*open, (std::vector<float>{0.5f}));
	EXPECT_EQ(FindGroup(layout, first, 2), SIZE_MAX) << "a group of another run is not this run's";
	EXPECT_EQ(FindGroup(layout, second, 2), 2u);
}

TEST(Coverage, ClearEmptiesALayoutForTheNextRun)
{
	interval const   domain[] = {{0.0f, 1.0f}};
	interval const   boxes[]  = {{0.0f, 1.0f}};
	layout_row const rows[]   = {{0, false}};
	run_layout       layout;
	layout_scratch   scratch;
	LayOut(domain, boxes, rows, 5, layout, scratch);
	Clear(layout);
	run_span const run = LayOut(domain, boxes, rows, 7, layout, scratch);
	EXPECT_EQ(run.first, 0u);
	EXPECT_EQ(layout.groupFlags.size(), 1u);
	EXPECT_EQ(layout.lists, (std::vector<uint32_t>{7}));
	EXPECT_EQ(layout.cutStart, (std::vector<uint32_t>{0, 2}));
}
