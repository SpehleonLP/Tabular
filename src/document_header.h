#ifndef TABULAR_DOCUMENT_HEADER_H
#define TABULAR_DOCUMENT_HEADER_H

// One cell of a [table]'s header row, the row that declares its columns. Not a
// public header.

#include "tabular/relation.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace tabular::detail
{

// `[>|^] name [?] [: type] [= default]`, whitespace allowed around ':' and '='.
// '>' marks a key and '>name?' a fall-through key; '^' marks an edge band.
struct header_cell
{
	enum class mark : uint8_t { None, Key, Edge };
	std::string_view                text;                  // the cell, trimmed, for refusals
	mark                            what        = mark::None;
	bool                            fallthrough = false;   // `>name?`
	std::string_view                name;
	std::optional<column_type>      type;                  // `: type`
	std::optional<std::string_view> fallback;              // `= value`, trimmed, non-empty
};

// Refuses, quoting the cell, anything that is not that grammar: an empty type
// or default, an unknown type, a name that is not a bare word, '?' on anything
// but a key, and a default on a key.
tl::expected<header_cell, ErrorContext> ParseHeaderCell(std::string_view cell);

// The type a cell declares: its annotation, else when -> Condition, weight ->
// Double, command -> Command, an edge -> Band, else Hash. Refuses an annotation
// that contradicts one of the three names or an edge.
tl::expected<column_type, ErrorContext> TypeOfCell(header_cell const& cell);

// A type's name as a file writes it, or nothing.
std::optional<column_type> TypeNamed(std::string_view name);

}

#endif // TABULAR_DOCUMENT_HEADER_H
