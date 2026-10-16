#ifndef TABULAR_ROWS_CSV_H
#define TABULAR_ROWS_CSV_H

#include "tabular/relation.h"
#include <functional>
#include <string_view>

namespace tabular
{

// CSV text whose header row names the table's columns, in any order; every
// column appears exactly once. Cells are read by their
// column's type: a hash is CityHash32 of the text, flags an unsigned decimal
// or 0x hex below 2^32, an int a signed decimal, a double as from_chars reads
// it, a vec2 as `a..b`, two floats, a command verbatim, a string as its trimmed text. Cells are trimmed of
// surrounding whitespace, blank
// lines and lines starting with '#' are skipped, a leading UTF-8 BOM is
// dropped, and text is otherwise taken byte for byte, so a hash matches the
// one a script literal of the same name gets. The rows replace the owner's
// rows in the table, all or none; a refusal names the offending line.
//
// A condition cell's text is handed to condition, which returns the compiled
// closure or an error; that is the whole of Tabular's part in compiling
// conditions. A table with condition columns needs one.
struct csv_rows
{
	uint32_t         table      = 0;
	uint64_t         owner      = 0;
	std::string_view text;
	std::string_view label;            // names the source in errors
	uint32_t         first_line = 1;   // the source line text starts on
	std::function<tl::expected<closure, ErrorContext>(std::string_view)> condition;
};

tl::expected<void, ErrorContext> LoadCsv(relation_store& store, csv_rows const& command);

}

#endif // TABULAR_ROWS_CSV_H
