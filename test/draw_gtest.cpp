#include <gtest/gtest.h>
#include "tabular/draw.h"

#include <array>
#include <cstdint>
#include <set>

using namespace tabular;

TEST(Draw, IsAPureFunctionOfSeedAndSite)
{
	for (uint64_t seed : {uint64_t(0), uint64_t(1), UINT64_C(0xDEADBEEFCAFEF00D), ~uint64_t(0)})
	{
		for (uint64_t site : {uint64_t(0), uint64_t(1), UINT64_C(0x100000000), ~uint64_t(0)})
		{
			uint64_t const first = Draw(seed, site);
			// Other draws in between change nothing: there is no generator state.
			(void)Draw(seed + 1, site);
			(void)Draw(seed, site + 1);
			EXPECT_EQ(Draw(seed, site), first);
		}
	}
	static_assert(Draw(3, 4) == Draw(3, 4), "usable at compile time, so it holds no state");
}

TEST(Draw, DifferentSitesAndSeedsDiffer)
{
	std::set<uint64_t> seen;
	for (uint64_t site = 0; site < 4096; ++site)
	{
		seen.insert(Draw(7, site));
	}
	EXPECT_EQ(seen.size(), 4096u) << "no two of the first 4096 sites draw alike";

	seen.clear();
	for (uint64_t seed = 0; seed < 4096; ++seed)
	{
		seen.insert(Draw(seed, 7));
	}
	EXPECT_EQ(seen.size(), 4096u);

	// The high halves are inputs too, not discarded.
	EXPECT_NE(Draw(1, 5), Draw(1 + (uint64_t(1) << 32), 5));
	EXPECT_NE(Draw(1, 5), Draw(1, 5 + (uint64_t(1) << 32)));
	// Seed and site are not interchangeable.
	EXPECT_NE(Draw(3, 9), Draw(9, 3));
	EXPECT_NE(Draw(0, 0), 0u);
}

// A fixed seed set and exact bounds, so the check is a deterministic fact, not
// a statistic that can flake. 64,000 draws into 16 buckets expect 4,000 each
// with a standard deviation near 61; the bounds allow +-400, over six of them.
TEST(Draw, FillsBucketsEvenlyFromTheTopAndTheBottom)
{
	constexpr int kDraws = 64000;
	for (uint64_t seed : {uint64_t(1), UINT64_C(0x9E3779B97F4A7C15), UINT64_C(0xABCDEF0123456789)})
	{
		std::array<int, 16> top{};
		std::array<int, 16> bottom{};
		for (int site = 0; site < kDraws; ++site)
		{
			uint64_t const d = Draw(seed, static_cast<uint64_t>(site));
			++top[d >> 60];
			++bottom[d & 15];
		}
		for (int b = 0; b < 16; ++b)
		{
			EXPECT_GE(top[b], 3600) << "seed " << seed << " top bucket " << b;
			EXPECT_LE(top[b], 4400) << "seed " << seed << " top bucket " << b;
			EXPECT_GE(bottom[b], 3600) << "seed " << seed << " bottom bucket " << b;
			EXPECT_LE(bottom[b], 4400) << "seed " << seed << " bottom bucket " << b;
		}
	}
}

TEST(Draw, SequentialSitesAreNotCorrelatedWithTheirNeighbours)
{
	// A counter-based hash must not hand consecutive sites consecutive numbers.
	int rising = 0;
	for (uint64_t site = 0; site < 10000; ++site)
	{
		rising += Draw(5, site + 1) > Draw(5, site) ? 1 : 0;
	}
	EXPECT_GE(rising, 4700);
	EXPECT_LE(rising, 5300);
}

TEST(DrawUnit, StaysInsideZeroToOneAndKeepsTheTopBits)
{
	EXPECT_EQ(DrawUnit(0), 0.0);
	EXPECT_LT(DrawUnit(~uint64_t(0)), 1.0);
	EXPECT_EQ(DrawUnit(UINT64_C(1) << 63), 0.5);
	EXPECT_EQ(DrawUnit(UINT64_C(1) << 62), 0.25);
	// The bottom 11 bits are below a double's precision here and are dropped.
	EXPECT_EQ(DrawUnit(UINT64_C(0x7FF)), 0.0);
}

// Strict avalanche over a fixed set of inputs: flipping any one of the 128 input
// bits must flip every output bit about half the time. A site bit that reached
// only one hash round fails this for the output bits that round leaves
// unmixed. The input pairs come from a fixed splitmix64 stream, so the check is
// a deterministic fact; the counts have a standard deviation near 16 of 1024
// pairs, and the 40-60% bounds are over twelve of them.
TEST(Draw, EveryInputBitFlipsEveryOutputBitAboutHalfTheTime)
{
	constexpr int kPairs = 1024;
	uint64_t state = UINT64_C(0x123456789ABCDEF0);
	auto next = [&state]()
	{
		state += UINT64_C(0x9E3779B97F4A7C15);
		uint64_t z = state;
		z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
		z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
		return z ^ (z >> 31);
	};
	std::array<std::array<uint64_t, 2>, kPairs> inputs{};
	for (auto& in : inputs)
	{
		in[0] = next();
		in[1] = next();
	}

	for (int bit = 0; bit < 128; ++bit)
	{
		std::array<int, 64> flips{};
		for (auto const& in : inputs)
		{
			uint64_t seed = in[0];
			uint64_t site = in[1];
			uint64_t const base = Draw(seed, site);
			uint64_t const mask = uint64_t(1) << (bit & 63);
			(bit < 64 ? seed : site) ^= mask;
			uint64_t const diff = base ^ Draw(seed, site);
			for (int out = 0; out < 64; ++out)
			{
				flips[out] += int((diff >> out) & 1);
			}
		}
		for (int out = 0; out < 64; ++out)
		{
			EXPECT_GE(flips[out], kPairs * 40 / 100) << (bit < 64 ? "seed" : "site") << " bit " << (bit & 63) << " -> output bit " << out;
			EXPECT_LE(flips[out], kPairs * 60 / 100) << (bit < 64 ? "seed" : "site") << " bit " << (bit & 63) << " -> output bit " << out;
		}
	}
}
