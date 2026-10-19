// The budget gate's driver (scripts/test-tabular-budgets.sh builds it with
// optimization): runs every benchmark at the sizes the budgets were measured
// at, prints each number beside its budget, and exits 1 when one is broken or
// measured nothing, 2 when a workload could not be built.
//
//   tabular-budgets [--scale S]
//
// --scale (0 < S <= 1) holds every measurement to S times its budget, so the
// gate can be shown failing without editing a budget; it cannot loosen one.

#include "budgets.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

int main(int argc, char** argv)
{
	double scale = 1;
	for (int a = 1; a < argc; ++a)
	{
		if (std::strcmp(argv[a], "--scale") == 0 && a + 1 < argc)
		{
			char* end = nullptr;
			scale     = std::strtod(argv[++a], &end);
			if (end == argv[a] || *end != '\0' || !(scale > 0 && scale <= 1))
			{
				std::fprintf(stderr, "--scale takes a number in (0, 1], not '%s'\n", argv[a]);
				return 2;
			}
		}
		else
		{
			std::fprintf(stderr, "usage: %s [--scale S]\n", argv[0]);
			return 2;
		}
	}

	auto measured = tabular::bench::RunBudgets(tabular::bench::budget_sizes{});
	if (!measured)
	{
		std::fprintf(stderr, "FAIL: a budget workload could not be built: %s\n", measured.error().format().c_str());
		return 2;
	}
	for (tabular::bench::measurement const& m : *measured)
	{
		std::string const unit(tabular::bench::UnitName(m.unit));
		// Tabs end the name and the budget, so a reader splits on them whatever
		// a name's length; the padding is only for the eye.
		std::printf("BUDGET %-40s\t%12.4g %-5s budget %.10g %s\t%s\n", m.name.c_str(), m.measured, unit.c_str(),
			m.budget * scale, unit.c_str(), m.how.c_str());
	}
	auto const over = tabular::bench::Exceeded(*measured, scale);
	for (std::string const& line : over)
	{
		std::printf("OVER %s\n", line.c_str());
	}
	if (!over.empty())
	{
		std::printf("FAIL: %zu of %zu budgets broken\n", over.size(), measured->size());
		return 1;
	}
	std::printf("PASS: %zu budgets met\n", measured->size());
	return 0;
}
