#ifndef TABULAR_CSV_CELLS_H
#define TABULAR_CSV_CELLS_H

// Cell readers shared by the CSV front ends. Not a public header.

#include "tabular/rows_csv.h"

#include <charconv>
#include <string_view>
#include <type_traits>

namespace tabular::detail
{

// The whole of text as T, or nothing: "12abc" is not 12.
template<typename T>
bool ParseWhole(std::string_view text, T& out, int base = 10)
{
	char const* end = text.data() + text.size();
	std::from_chars_result r;
	if constexpr (std::is_floating_point_v<T>)
	{
		r = std::from_chars(text.data(), end, out);
	}
	else
	{
		r = std::from_chars(text.data(), end, out, base);
	}
	return r.ec == std::errc() && r.ptr == end;
}

// A value cell of column's type; an empty cell is refused.
tl::expected<cell, ErrorContext> ReadCell(column_def const& column, std::string_view text);

tl::expected<void, ErrorContext> LoadPivotedCsv(relation_store& store, csv_rows const& command);

}

#endif // TABULAR_CSV_CELLS_H
