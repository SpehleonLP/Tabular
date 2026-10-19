#ifndef TABULAR_BENCH_BUDGETS_H
#define TABULAR_BENCH_BUDGETS_H

// Tabular's budget benchmarks (P17): one measurement per row of
// definition.md's Budgets table, each with the budget it is held to.
// scripts/test-tabular-budgets.sh builds them standalone with optimization and
// fails when a number is broken; the Debug test build runs them small
// (TabularBudget), only to keep them compiling and measuring, since a -O0
// build's timings say nothing about the step that ships.

#include "Errors/errorcontext.h"
#include <expected/include/tl/expected.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tabular::bench
{

// How big the measured workloads are. The defaults are the sizes the budgets
// in definition.md were measured at; a smaller run measures the same code.
struct budget_sizes
{
	size_t clauses      = 30;     // the budget brain: clauses (each over its own table)
	size_t modes        = 30;     // its state's members; each table has modes x 10 rows
	size_t instances    = 1000;   // instances stepped per tick
	size_t steps        = 50;     // ticks per timed repeat
	size_t repeats      = 7;      // a timing is the fastest repeat
	size_t mutableRows  = 10000;  // the plain mutable table (spawn points)
	size_t displays     = 50;     // the mutable best table (display owners): displays x 4 owners
};

enum class budget_unit : uint8_t
{
	Nanoseconds,
	Microseconds,
	Bytes,
};

struct measurement
{
	std::string name;       // as definition.md's Budgets table names it
	double      measured = 0;
	double      budget   = 0;
	budget_unit unit     = budget_unit::Nanoseconds;
	std::string how;        // what one unit of work is, and the size it was measured at
};

// Runs every benchmark. A workload the library refuses (a definition that does
// not load or prove) is an error, not a measurement: a benchmark that measured
// nothing must not pass.
tl::expected<std::vector<measurement>, ErrorContext> RunBudgets(budget_sizes const& sizes);

// The measurements over their budget scaled by scale (1 for the real gate; a
// smaller one shows the gate failing), one line each. A measurement that is
// not a positive finite number is listed too: it measured nothing.
std::vector<std::string> Exceeded(std::span<measurement const> measured, double scale);

std::string_view UnitName(budget_unit unit);

}

#endif // TABULAR_BENCH_BUDGETS_H
