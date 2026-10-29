#ifndef TABULAR_SEXPR_H
#define TABULAR_SEXPR_H

// The tree form of a bracket line's s-expression. Not a public header.
//
// command_text.h's lexer stays as it is: it checks the shape of a command that
// runs across CSV cells. This reads one declaration into a tree, which is a
// different job, but the lexical rules are the same (parentheses, double-quoted
// strings with \" and \\ escapes, atoms split on whitespace).

#include "Errors/errorcontext.h"

#include <expected/include/tl/expected.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tabular::detail
{

struct sexpr
{
	enum class kind : uint8_t { Atom, String, List };

	kind               what = kind::Atom;
	std::string_view   text;    // atom text, or string contents with escapes still in place
	uint32_t           line = 0;
	std::vector<sexpr> items;   // List only
};

// Exactly one list, `(` ... `)`, from text. line is the source line text starts on.
// An error carries no position in its message; its context is `<label>:<line>`, as
// csv_text writes it. A '#' at a token's start comments out the rest of the line,
// strings escape only \" and \\, and lists nest at most 64 deep.
// The result views into text.
tl::expected<sexpr, ErrorContext> ParseList(std::string_view text, uint32_t line, std::string_view label);

// The items of a list whose parentheses the caller has already taken off: the
// inside of a `[table ...]` bracket reads as the list `(table ...)`, and viewing
// the original text avoids a copy the tree would then dangle into.
tl::expected<sexpr, ErrorContext> ParseItems(std::string_view inner, uint32_t line, std::string_view label);

// \" and \\ undone.
std::string Unescape(sexpr const& string);

}

#endif // TABULAR_SEXPR_H
