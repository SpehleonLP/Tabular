#ifndef TABULAR_PICK_INTERNAL_H
#define TABULAR_PICK_INTERNAL_H

// Internal to Tabular: the pieces of the interpreted picks that a machine's
// Step runs too, so the compiled step and its oracle read inputs, test
// conditions, rank flags and draw rows with one piece of code each. Not a
// public header.

#include "tabular/pick.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace tabular::detail
{

// A table input as a band compares it: a ranged input clamped in double into
// [lo, the largest float below hi], so the top value lands in the top band and
// a value past float's range never narrows; an unranged one as it is.
float ClampedInput(input_def const& in, double value);

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
// On Failed, a non-null error_out (empty on the way in) receives the host's
// error with the table and row named. A null error_out is the step's path:
// the host's error is dropped and nothing is built or added, so the test
// allocates nothing Tabular's (a host message past small-string size
// allocates on the host side). The interpreted picks pass one; slice 4's
// report will too.
row_test ScriptHolds(TableVersion const& version, size_t row, void const* this_in, ErrorContext* error_out);

// The OR of an or table's output over its rows [first, last) that hold (every
// band for its clamped input, then the script condition); 0 when none does.
// None when a script condition could not answer, error_out as ScriptHolds.
// The query's inputs must have passed FirstBadInput.
std::optional<uint32_t> OrOfRows(TableVersion const& version, size_t first, size_t last, pick_query const& query,
	ErrorContext* error_out);

// The row of the weighted table's rows [first, last) the point
// DrawUnit(Draw(seed, site)) * total falls in, the run laid end to end by
// weight in row order. The run must not be empty.
size_t DrawRow(TableVersion const& version, size_t first, size_t last, uint64_t seed, uint64_t site);

}

#endif // TABULAR_PICK_INTERNAL_H
