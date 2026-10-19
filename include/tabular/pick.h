#ifndef TABULAR_PICK_H
#define TABULAR_PICK_H

#include "tabular/relation.h"

#include <optional>
#include <span>
#include <vector>

namespace tabular
{

// What a pick is given. Keys select the key run (the table's leading order
// columns); inputs are the table's declared inputs in order, which the caller
// resolved, since Tabular resolves no names; this_in goes to the row's script
// condition.
struct pick_query
{
	std::span<cell const> keys;
	std::span<cell const> inputs;
	void const*           this_in = nullptr;
	// The inputs' previous values, parallel to inputs, which an edge band
	// compares against: required (and checked as inputs are) for a table with
	// an edge, and otherwise empty or ignored.
	std::span<cell const> previous = {};
};

// A row holds when every band holds for its input and its script condition,
// if any, holds; bands are tested first. A script condition that cannot answer
// fails the pick with its error: it is never read as "does not hold".
//
// The best row of the key run: with a ranking column, ranked as animation's
// LookupBest ranks a lookup row (a row whose flags are all in the input first,
// then the larger R & F, then the larger R; ties to row order), rows matching
// nothing skipped, and the R == 0 row tried last; without one, in row order.
// Empty when no row holds.
tl::expected<std::optional<size_t>, ErrorContext> PickBest(TableVersion const& version, pick_query const& query);
tl::expected<std::optional<size_t>, ErrorContext> PickBest(relation_store const& store, uint32_t table, pick_query const& query);

// Every row of the key run that holds, in row order; ranking is not used.
tl::expected<std::vector<size_t>, ErrorContext> PickAll(TableVersion const& version, pick_query const& query);
tl::expected<std::vector<size_t>, ErrorContext> PickAll(relation_store const& store, uint32_t table, pick_query const& query);

// The OR of the output column over every row of the key run that holds; 0 when
// none does, so an or table needs no default. This is animation's
// PipelineClosure::eval_phase_bits.
tl::expected<uint32_t, ErrorContext> PickOr(TableVersion const& version, pick_query const& query);
tl::expected<uint32_t, ErrorContext> PickOr(relation_store const& store, uint32_t table, pick_query const& query);

// One row of the key run, drawn in proportion to its weight: the run's rows in
// row order are laid end to end by weight, and Draw(seed, site) picks a point on
// that line. Nothing is tested first (a weighted table has no conditions), so
// the answer depends only on the rows, the seed and the site, and is empty only
// when the key has no run. The weights are normal and positive and a run's total
// is finite, which Load enforces, so a nonempty run always draws a row.
tl::expected<std::optional<size_t>, ErrorContext> PickWeighted(TableVersion const& version, pick_query const& query,
	uint64_t seed, uint64_t site);
tl::expected<std::optional<size_t>, ErrorContext> PickWeighted(relation_store const& store, uint32_t table,
	pick_query const& query, uint64_t seed, uint64_t site);

}

#endif // TABULAR_PICK_H
