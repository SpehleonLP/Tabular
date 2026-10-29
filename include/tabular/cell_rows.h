#ifndef TABULAR_CELL_ROWS_H
#define TABULAR_CELL_ROWS_H

#include "Errors/errorcontext.h"

#include <expected/include/tl/expected.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tabular
{

// A CSV text as rows of cells, quotes undone, as Tabular reads every body. It is
// public so a host reading the body of a section it claimed (ReadDocument's
// host_heads) reads the same dialect as the tables beside it, not a second one.
// Blank rows (nothing but separators and spaces, as a spreadsheet writes an
// empty row) and comment rows (first unquoted character '#') are skipped.
class cell_rows
{
public:
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

protected:
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

// Refused, naming label and a line: an unterminated quote (the line it opened
// on), a '"' inside an unquoted cell (a spreadsheet never writes one), or text
// after a cell's closing quote. first_line is the file line text starts on, so a
// body inside a larger file reports lines the author can find. A leading BOM is
// dropped.
tl::expected<cell_rows, ErrorContext> ReadCells(std::string_view text, std::string_view label, uint32_t first_line);

}

#endif // TABULAR_CELL_ROWS_H
