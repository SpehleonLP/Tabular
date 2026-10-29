#ifndef TABULAR_CSV_TEXT_H
#define TABULAR_CSV_TEXT_H

// The CSV reader every Tabular front end shares (rows, pivoted, grid). Not a
// public header.
//
// Tables are authored in a spreadsheet, and a spreadsheet's CSV export quotes
// any cell holding a comma, a double quote or a line break, doubling the quotes
// inside (RFC 4180). So `(message food "eat")` is saved as
// `"(message food ""eat"")"`, and a reader without quoting would keep both sets
// of quotes. Spehleon/lib's CSV::File has no quoting and other engine code
// reads it, so Tabular reads its own.

#include "tabular/cell_rows.h"

#include <span>

namespace tabular::detail
{

// The reader behind ReadCells; cell_rows holds what it reads. Kept as its own
// name so every Tabular front end says csv_text::Read.
class csv_text : public cell_rows
{
public:
	// Refused, naming label and a line: an unterminated quote (the line it
	// opened on), a '"' inside an unquoted cell (a spreadsheet never writes
	// one), or text after a cell's closing quote. first_line is the source
	// line text starts on, so a CSV body inside a larger file reports lines
	// the author can find.
	static tl::expected<csv_text, ErrorContext> Read(std::string_view text, std::string_view label,
		uint32_t first_line = 1);
	// The same, except that record i (blank and comment rows are not records)
	// starts on record_lines[i]: text compiled from a file names the lines the
	// author wrote. Refused when the text has another number of records.
	static tl::expected<csv_text, ErrorContext> Read(std::string_view text, std::string_view label,
		std::span<uint32_t const> record_lines);
};

}

#endif // TABULAR_CSV_TEXT_H
