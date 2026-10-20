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

// Refused when command is not one list of commands, as every command column
// holds (R9): a ')' that closes nothing, a '(' never closed, a string never
// closed, or text that is not exactly one parenthesised list (`(a) (b)`, a
// bare atom). Lexically a command is parentheses, double-quoted strings (`\"`
// and `\\` escape inside one) and atoms, and nothing about what it means is
// checked.
tl::expected<void, ErrorContext> CheckCommandList(std::string_view command);

// Command cells as the one list the store keeps: the cells joined with a
// space, as a sequence of s-expressions, wrapped in parentheses. A declared
// command column is one cell, a suffix every cell from its first on. Cell
// boundaries mean nothing inside an open parenthesis, so a command may run
// across cells when it starts with '('. Outside one, a cell of several
// items that does not start with '(' is one command written without its
// parentheses, and is wrapped: `message this hard-land` reads as
// `(message this hard-land)`, while a single word stays a word. A cell that is
// an empty list (`()`, `( )`) is the explicit "do nothing" and adds nothing,
// and no cells (or only empty ones) is `()`. Refused when a cell of several
// items that does not start with '(' is not whole by itself (`message food
// (say`, `a) (b`): wrapping it would hide where its parenthesis was meant to
// go; and when a string is open at a cell's end, which in a quoted CSV is an
// unquoted comma inside it (joining would drop the comma). Refused too, as
// CheckCommandList, when the joined list is not one list.
//
// This is the CSV front end's rule. The builder and the store take a command
// column's text in the store's form, the list already written
// (`((anim chew))`), and only check its shape: they wrap nothing.
tl::expected<std::string, ErrorContext> CommandList(std::span<std::string_view const> cells);

}

#endif // TABULAR_COMMAND_TEXT_H
