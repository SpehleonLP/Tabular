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

// The two floats of `a..b`, the whole text. Three dots in a row would let
// "1...2" read as 1 and .2; which pair separates is ambiguous, so it is
// refused rather than guessed.
bool ParseRange(std::string_view text, float& start, float& end);

// The one rule for an empty cell, whatever the column's type: it is refused.
// Exceptions are the callers' own: a condition column's blank cell is "no
// condition", a pivoted table's blank cell is no row, and a message key's
// blank cell is no message (see the table_def ReadCell); all are accepted
// before asking. A command or string column holds text and no cell value, so
// its caller asks this instead of ReadCell and keeps the text.
tl::expected<void, ErrorContext> RefuseBlank(column_def const& column, std::string_view text);

// A cell of column's type, refusing a blank one through RefuseBlank. A command
// or string column holds no value, so it is refused here too.
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

// A cell of def's column as ReadCell reads it, except that a blank cell in the
// message key is "no message", hash 0: the one blank a value column takes and
// the one place a hash may be 0. Any text there is a message type, and hashing
// to 0 is refused for it as for every name.
tl::expected<cell, ErrorContext> ReadCell(table_def const& def, uint32_t column, std::string_view text,
	hash_names& names, uint32_t line);

// A refused Load, told in the file's terms: the lines of the rows it names,
// as lineOf(row) gives them, and the label alone when it names none. The
// rows and pivoted front ends both map Load's row indexes back this way.
template<typename LineOf>
ErrorContext LocateRefusal(ErrorContext error, load_refusal const& refusal, size_t count, std::string_view label,
	LineOf const& lineOf)
{
	if (refusal.earlier < count)
	{
		error = std::move(error).with("same key as line ", lineOf(refusal.earlier));
	}
	if (refusal.covered_by < count)
	{
		error = std::move(error).with("covered by line ", lineOf(refusal.covered_by));
	}
	if (refusal.row < count)
	{
		return std::move(error).with(cc(std::string(), label, ":", lineOf(refusal.row)));
	}
	return std::move(error).with(std::string(label));
}

tl::expected<void, ErrorContext> LoadPivotedCsv(relation_store& store, csv_rows const& command);

}

#endif // TABULAR_CSV_CELLS_H
