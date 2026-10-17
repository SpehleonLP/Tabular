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
};

// A row holds when every band holds for its input and its script condition,
// if any, holds; bands are tested first. A script condition that cannot answer
// fails the pick with its error: it is never read as "does not hold".
//
// The best row of the key run: with a ranking column, ranked as animation's
// LookupBest ranks a lookup row (a row whose flags are all in the input first,
// then the larger R & F, then the larger R; ties to the lower pivot column,
// then row order), rows matching nothing skipped, and the R == 0 row tried
// last; without one, in row order. Empty when no row holds.
tl::expected<std::optional<size_t>, ErrorContext> PickBest(TableVersion const& version, pick_query const& query);
tl::expected<std::optional<size_t>, ErrorContext> PickBest(relation_store const& store, uint32_t table, pick_query const& query);

}

#endif // TABULAR_PICK_H
