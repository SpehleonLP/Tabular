#ifndef TABULAR_DRAW_H
#define TABULAR_DRAW_H

#include "Support/sample_hash.h"

#include <cstdint>

namespace tabular
{

// Randomness with no generator state (definition.md, promise 4): a pure hash of
// the host's seed and the draw site, so the same (seed, site) is the same draw
// on any thread in any order, and a step's draws do not depend on how many
// came before. A site salts one draw point (a table, a row) so two points in
// one step with one seed do not share a number. Built on Support's pcg3d, the
// stateless hash the cook's sampling already uses, rather than a second recipe.
constexpr uint64_t Draw(uint64_t seed, uint64_t site)
{
	// pcg3d mixes three 32-bit words, and its own rounds leave each input word
	// poorly spread across the outputs. The site's high word enters the second
	// call, so a third call on that result gives every input bit at least two
	// calls' mixing after it enters; with only two, high site bits never reached
	// some low output bits. The avalanche test pins this.
	Support::hash3 const first  = Support::Pcg3d(uint32_t(seed), uint32_t(seed >> 32), uint32_t(site));
	Support::hash3 const second = Support::Pcg3d(first.x, first.y ^ uint32_t(site >> 32), first.z);
	Support::hash3 const third  = Support::Pcg3d(second.x, second.y, second.z);
	return (uint64_t(third.x) << 32) | third.y;
}

// [0, 1) from a draw's top 53 bits: exact in a double, and 1 is never returned.
constexpr double DrawUnit(uint64_t draw)
{
	return double(draw >> 11) * (1.0 / 9007199254740992.0);
}

}

#endif // TABULAR_DRAW_H
