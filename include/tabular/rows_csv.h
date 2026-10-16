#ifndef TABULAR_ROWS_CSV_H
#define TABULAR_ROWS_CSV_H

#include "tabular/relation.h"
#include <string_view>

namespace tabular
{

// CSV text whose header row names the table's columns, in any order; every
// column but a Condition one appears exactly once. Cells are read by their
// column's type: a hash is CityHash32 of the text, flags an unsigned decimal
// or 0x hex below 2^32, an int a signed decimal, a double as from_chars reads
// it, a command verbatim. The rows replace the owner's rows in the table, all
// or none.
struct csv_rows
{
	uint32_t         table      = 0;
	uint64_t         owner      = 0;
	std::string_view text;
	std::string_view label;            // names the source in errors
	uint32_t         first_line = 1;   // the source line text starts on
};

tl::expected<void, ErrorContext> LoadCsv(relation_store& store, csv_rows const& command);

}

#endif // TABULAR_ROWS_CSV_H
