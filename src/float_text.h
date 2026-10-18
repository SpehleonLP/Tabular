#ifndef TABULAR_FLOAT_TEXT_H
#define TABULAR_FLOAT_TEXT_H

// Internal to Tabular: only the sources that write numbers into messages
// include it, so <charconv> stays out of the shared row layout.

#include <charconv>
#include <string>
#include <type_traits>

namespace tabular
{

// cc refuses floats; messages spell them the shortest way that reads back at
// the value's own width, so a band's 0.1f reads 0.1, not its double expansion.
template<typename Real>
std::string FloatText(Real v)
{
	static_assert(std::is_floating_point_v<Real>);
	char buffer[32];
	auto r = std::to_chars(buffer, buffer + sizeof buffer, v);
	return std::string(buffer, r.ptr);
}

}

#endif // TABULAR_FLOAT_TEXT_H
