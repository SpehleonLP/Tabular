#ifndef TABULAR_ROWS_CSV_H
#define TABULAR_ROWS_CSV_H

#include "tabular/relation.h"
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace tabular
{

// CSV text whose header row names the table's columns, in any order; every
// column appears exactly once. Cells are read by their
// column's type: a hash is CityHash32 of the text, and within one load two
// texts with one hash, or a text hashing to 0, are refused, naming both lines,
// flags an unsigned decimal
// or 0x hex below 2^32, an int a signed decimal, a double as from_chars reads
// it, a vec2 as `a..b`, two floats, a command as one list of commands (below),
// a string as its text. Cells are quoted as RFC 4180 has it, so a
// quoted cell may hold commas, doubled quotes and line breaks; cells are
// trimmed of whitespace outside the quotes, trailing empty cells past the
// header say nothing, blank lines (a row of nothing but separators included)
// and lines whose first unquoted character is '#' are skipped, a leading UTF-8
// BOM is dropped, and text is otherwise taken byte for byte, so a hash matches
// the one a script literal of the same name gets. The rows replace the owner's
// rows in the table, all or none; a refusal names the offending line.
//
// A condition cell's text is handed to condition, which returns the compiled
// closure or an error; that is the whole of Tabular's part in compiling
// conditions. It is needed only when a non-blank condition cell appears. A
// blank condition cell is no condition: it is not handed to condition. A
// blank fall-through key is the blank, 0, and a blank edge cell no edge.
// Every other blank cell is refused, a declared command cell's included
// (`()` is the explicit "do nothing").
//
// A command cell is one list of commands: a cell of several items that does
// not start with '(' is one command and is wrapped, so `anim chew` reads as
// `((anim chew))`, while a single word stays a word. A table with a suffix
// (table_def::suffix) ends its header with the suffix column's name, and every
// cell of a row from the suffix column on is joined, with a space, into that
// one list, so a command may run across cells; no cells, or only `()`, is
// `()`. Refused, naming the line, when the cells do not join into exactly one
// list (see the commands-and-messages spec, Part 2).
//
// A pivoted load reads the table's pivot: the header row names the key
// columns, in key order, then the flag headers, whose order gives the flag
// domain (a best table's first is None, the 0 value; each simple header after
// takes the next bit; `A|B` ORs headers to its left; headers are distinct flag
// sets, and None anywhere but a best table's first header is refused). Each non-empty cell is
// one row, its header's flags in the headers column and the cell in the cells
// column; blank cells give no row.
struct csv_rows
{
	uint32_t         table      = 0;
	uint64_t         owner      = 0;
	std::string_view text;
	std::string_view label;            // names the source in errors
	uint32_t         first_line = 1;   // the source line text starts on
	std::function<tl::expected<closure, ErrorContext>(std::string_view)> condition;
	bool                      pivoted    = false;     // the table's pivot: keys, then flag headers
	std::vector<std::string>* flag_names = nullptr;   // filled by a pivoted load: index = bit
	std::vector<run_note>*    report     = nullptr;   // the load's report (see relation_store); empty on a refusal
};

tl::expected<void, ErrorContext> LoadCsv(relation_store& store, csv_rows const& command);

// A grid in CSV: the header row's cells after the first are the column keys,
// each line's first cell its row key, and every other cell one grid cell.
// Keys read as key cells (a hash is CityHash32 of the text (collisions refused
// as in LoadCsv), an int decimal).
// Every cell is filled: a blank one is refused, naming its line and column
// header. Trimming, '#' lines, blank lines and the BOM are as in LoadCsv.
struct csv_grid
{
	uint32_t         table      = 0;
	uint64_t         owner      = 0;
	std::string_view text;
	std::string_view label;
	uint32_t         first_line = 1;
};

tl::expected<void, ErrorContext> LoadGridCsv(relation_store& store, csv_grid const& command);

}

#endif // TABULAR_ROWS_CSV_H
