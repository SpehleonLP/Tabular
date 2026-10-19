#ifndef TABULAR_COMMAND_TEXT_H
#define TABULAR_COMMAND_TEXT_H

// The shape of a command, which is all Tabular knows of one: a command is
// s-expression text the host interprets (R9). Not a public header.

#include "Errors/errorcontext.h"

#include <expected/include/tl/expected.hpp>

#include <span>
#include <string>
#include <string_view>

namespace tabular::detail
{

// Refused when command is not well formed: a ')' that closes nothing, a '('
// never closed, or a string never closed. Lexically a command is parentheses,
// double-quoted strings (`\"` and `\\` escape inside one) and atoms, and
// nothing about what it means is checked.
tl::expected<void, ErrorContext> CheckCommandShape(std::string_view command);

// A row's suffix cells as the one list the store keeps: the cells joined with
// a space, as a sequence of s-expressions, wrapped in parentheses. Cell
// boundaries mean nothing inside an open parenthesis, so a command may run
// across cells. Outside one, a cell of several items that does not start with
// '(' is one command written without its parentheses, and is wrapped:
// `message this hard-land` reads as `(message this hard-land)`, while a single
// word stays a word. No cells (or only empty ones) is `()`. The shape is not
// checked here; the store refuses a list that is not well formed.
std::string CommandList(std::span<std::string_view const> cells);

}

#endif // TABULAR_COMMAND_TEXT_H
