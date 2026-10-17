#include <gtest/gtest.h>
#include "../src/coverage.h"

#include <vector>

using tabular::detail::interval;
using tabular::detail::Uncovered;

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
