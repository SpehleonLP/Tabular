#ifndef TABULAR_TEST_PROBE_CLOSURE_H
#define TABULAR_TEST_PROBE_CLOSURE_H

#include "tabular/relation.h"

#include <cstdint>

namespace tabular_test
{

// A condition's captured state: the threshold it compares with, and a count
// of releases, so a test can see each closure released exactly once.
struct Probe
{
	int64_t threshold = 0;
	int*    released  = nullptr;
};

// this_in is an int64_t; the row counts when it reaches the threshold. A
// negative this_in is an error the closure reports.
inline bool AtLeast(void* data, void const* this_in, ErrorContext* error_out)
{
	auto const* p     = static_cast<Probe const*>(data);
	int64_t     value = *static_cast<int64_t const*>(this_in);
	if (value < 0)
	{
		*error_out = ErrorContext("negative input ", value);
		return false;
	}
	return value >= p->threshold;
}

inline void ReleaseProbe(void* data)
{
	auto* p = static_cast<Probe*>(data);
	++*p->released;
	delete p;
}

inline tabular::closure MakeProbe(int64_t threshold, int* released)
{
	return tabular::closure{&AtLeast, new Probe{threshold, released}, &ReleaseProbe};
}

}

#endif // TABULAR_TEST_PROBE_CLOSURE_H
