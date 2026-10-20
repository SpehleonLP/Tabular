#ifndef TABULAR_PICK_INTERNAL_H
#define TABULAR_PICK_INTERNAL_H

// Internal to Tabular: the pieces of the interpreted picks that a machine's
// Step runs too, so the compiled step and its oracle read inputs, test
// conditions, rank flags and draw rows with one piece of code each. Not a
// public header.

#include "tabular/pick.h"
#include "table_layout.h"

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace tabular::detail
{

// A table input as a band compares it: a ranged input clamped in double into
// [lo, the largest float below hi], so the top value lands in the top band and
// a value past float's range never narrows; a cyclic one wrapped into
// [lo, hi), and NaN when it is not finite, since a period has no place for
// it; an unranged one as it is.
float ClampedInput(input_def const& in, double value);

// Crossed for a cyclic input, whose range is a period: the move is taken the
// shorter way around, exactly half a period upward, and it crossed when it
// reached the end of the band it travelled toward (lo going up, hi going
// down) on the way. So it entered, or passed through, and a move that leaves
// the band and comes back into it the short way round has entered it too.
bool CrossedOnPeriod(input_def const& in, cell band, float previous, float now);

// P13's crossing, the one test of an edge: whether an input that moved from
// previous to now (both as ClampedInput gives them) crossed into the band
// [lo, hi). previous lay outside it, and now is at or past the end it came
// from: inside the band, or through it and out the other side, so a short
// band is never skipped by a fast input. Leaving from inside crosses nothing,
// and neither does reaching hi from above, since hi is not in the band; a
// previous value on hi is outside, so moving down from it enters. NaN fails
// every comparison and crosses nothing. A cyclic input moves around its
// period instead of along a line (CrossedOnPeriod).
inline bool Crossed(input_def const& in, cell band, float previous, float now)
{
	if (in.cyclic)
	{
		return CrossedOnPeriod(in, band, previous, now);
	}
	if (previous < band.v[0])
	{
		return now >= band.v[0];
	}
	if (previous >= band.v[1])
	{
		return now < band.v[1];
	}
	return false;
}

// Whether every edge of row holds for its input moving from previous to
// inputs (parallel spans, the table's inputs); a NoEdge cell holds. Plain
// bands are not tested: a pick tests them itself, and a step's cell already
// lies inside them. The interpreted picks and a step both test edges here.
bool EdgesHold(TableVersion const& version, size_t row, std::span<cell const> inputs, std::span<cell const> previous);

// The first input a pick refuses, by index into def.inputs (UINT32_MAX for
// none): a flag set wider than 32 bits, or NaN for a ranged input (a clamp has
// no place for it). nan says which. Allocates nothing, so a step can ask every
// time.
struct bad_input
{
	uint32_t index = UINT32_MAX;
	bool     nan   = false;
};
bad_input FirstBadInput(table_def const& def, std::span<cell const> inputs);

// FirstBadInput for the one input i, for a check that reads only some inputs.
bad_input BadInputAt(table_def const& def, std::span<cell const> inputs, uint32_t i);

// What a pick says of the input bad names (bad.index is an input), in the
// interpreted picks' refusal and a machine step's report alike.
ErrorContext BadInputError(table_def const& def, bad_input bad, std::span<cell const> inputs);

// How well the flags R of a row or flag group match the input flags F, as a
// best pick ranks them; larger is better: a full match (every flag of R in F),
// then the larger R & F, then the larger R. Only R & F != 0 is ranked: the
// default (R == 0) is tried after every ranked candidate, and flags matching
// nothing in the input are never tried. PickBest and a best clause's Step
// both rank with this, so they cannot disagree on an order.
struct flag_rank
{
	uint32_t full = 0;
	uint32_t hit  = 0;
	uint32_t R    = 0;

	friend auto operator<=>(flag_rank const&, flag_rank const&) = default;
};

inline bool Ranked(uint32_t R, uint32_t F)
{
	return (R & F) != 0;
}

inline flag_rank RankOf(uint32_t R, uint32_t F)
{
	uint32_t const hit = R & F;
	return flag_rank{hit == R ? 1u : 0u, hit, R};
}

// What a row's conditions said.
enum class row_test : uint8_t
{
	Misses,   // a condition does not hold
	Holds,    // every condition holds (or the row has none)
	Failed,   // a script condition could not answer
};

// Whether the row's script conditions hold for this_in; a row with none holds.
// On Failed, error (empty on the way in) holds the host's error as the host
// wrote it: nothing is added, so the test allocates nothing Tabular's (a host
// message past small-string size allocates on the host side). Each condition
// is asked once; whoever reports the failure names the table and row (the
// interpreted picks always, a step the first time it reports that row).
row_test ScriptHolds(TableVersion const& version, size_t row, void const* this_in, ErrorContext& error);

// The OR of an or table's output over its rows [first, last) that hold (every
// band for its clamped input, then the script condition); 0 when none does.
// None when a script condition could not answer: error as ScriptHolds, and
// failed_row set to that row. The query's inputs must have passed
// FirstBadInput.
std::optional<uint32_t> OrOfRows(TableVersion const& version, size_t first, size_t last, pick_query const& query,
	ErrorContext& error, size_t& failed_row);

// The fall-through, a property of the table and so of every lookup of its
// runs, interpreted or prepared: find(keys); when that misses (missed says
// so), retry with blanks (0) in the fall-through keys that hold a value, in
// counting order over those keys as bits, the first fall-through key in key
// order the lowest: with keys (a, b) both falling through, (a, b), then
// (0, b), then (a, 0), then (0, 0). A combination that blanks a key already 0
// is the same lookup as one tried before, and is skipped. The first hit
// answers; only a miss of all is a miss. The retries are written to scratch,
// which may be keys' own storage, and every fall-through key is put back as
// it came before returning, so a report names the keys as read. blanked, if
// given, receives the combination that hit (0 for the keys as read, or for a
// miss). scratch is as wide as keys.
template<typename Find, typename Missed>
auto FindFallingThrough(std::span<cell const> keys, std::span<cell> scratch, fall_through_keys const& fall,
	Find const& find, Missed const& missed, uint32_t* blanked = nullptr)
{
	if (blanked)
	{
		*blanked = 0;
	}
	auto found = find(keys);
	if (fall.count == 0 || !missed(found))
	{
		return found;
	}
	std::array<cell, kMaxFallThroughKeys> asRead{};
	uint32_t                              alreadyBlank = 0;
	for (uint32_t i = 0; i < fall.count; ++i)
	{
		asRead[i] = keys[fall.key[i]];
		alreadyBlank |= asRead[i].i == 0 ? uint32_t(1) << i : 0;
	}
	if (scratch.data() != keys.data())
	{
		std::copy(keys.begin(), keys.end(), scratch.begin());
	}
	uint32_t const all = (uint32_t(1) << fall.count) - 1;
	for (uint32_t mask = 1; mask <= all; ++mask)
	{
		if ((mask & alreadyBlank) != 0)
		{
			continue;
		}
		for (uint32_t i = 0; i < fall.count; ++i)
		{
			scratch[fall.key[i]] = ((mask >> i) & 1) != 0 ? cell::Integer(0) : asRead[i];
		}
		found = find(std::span<cell const>(scratch));
		if (!missed(found))
		{
			if (blanked)
			{
				*blanked = mask;
			}
			break;
		}
	}
	for (uint32_t i = 0; i < fall.count; ++i)
	{
		scratch[fall.key[i]] = asRead[i];
	}
	return found;
}

// The row of the weighted table's rows [first, last) the point
// DrawUnit(Draw(seed, site)) * total falls in, the run laid end to end by
// weight in row order. The run must not be empty.
size_t DrawRow(TableVersion const& version, size_t first, size_t last, uint64_t seed, uint64_t site);

}

#endif // TABULAR_PICK_INTERNAL_H
