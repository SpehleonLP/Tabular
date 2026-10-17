#ifndef TABULAR_CSV_CELLS_H
#define TABULAR_CSV_CELLS_H

// Cell readers shared by the CSV front ends. Not a public header.

#include "tabular/rows_csv.h"

#include <charconv>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>

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

// Every text one CSV load hashed, in any column, so two names with one hash,
// or a name hashing to 0, are refused: a script looking either name up would
// get the other, or a "no hash".
class hash_names
{
public:
	tl::expected<void, ErrorContext> Note(std::string_view text, int64_t hash, uint32_t line);

private:
	std::unordered_map<int64_t, std::pair<std::string, uint32_t>> _seen;
};

// ReadCell, recording a hash cell's text in names.
tl::expected<cell, ErrorContext> ReadCell(column_def const& column, std::string_view text, hash_names& names,
	uint32_t line);

tl::expected<void, ErrorContext> LoadPivotedCsv(relation_store& store, csv_rows const& command);

}

#endif // TABULAR_CSV_CELLS_H
