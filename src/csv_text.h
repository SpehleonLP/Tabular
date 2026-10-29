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

#include "Errors/errorcontext.h"

#include <expected/include/tl/expected.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tabular::detail
{

// A CSV text as rows of cells, quotes undone. Blank rows (nothing but
// separators and spaces, as a spreadsheet writes an empty row) and comment
// rows (first unquoted character '#') are skipped; a leading BOM is dropped.
class csv_text
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

	size_t rows() const { return _rows.size(); }
	// The cells row has, padding included.
	size_t width(size_t row) const { return _rows[row].count; }
	// The cells row has up to its last non-empty one: a spreadsheet pads every
	// row to its widest with empty cells, so trailing empties carry nothing.
	size_t used_width(size_t row) const;
	// Empty past the row's end. An unquoted cell is trimmed; a quoted one is
	// its text between the quotes, exactly.
	std::string_view cell(size_t column, size_t row) const;
	// The line a row starts on: a quoted cell may run over several.
	uint32_t line_of_row(size_t row) const { return _rows[row].line; }

private:
	struct cell_span
	{
		uint32_t offset = 0;
		uint32_t size   = 0;
	};
	struct row_span
	{
		uint32_t first = 0;
		uint32_t count = 0;
		uint32_t line  = 0;
	};

	std::string            _text;    // every cell's text, end to end
	std::vector<cell_span> _cells;
	std::vector<row_span>  _rows;
};

}

#endif // TABULAR_CSV_TEXT_H
