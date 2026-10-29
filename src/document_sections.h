#ifndef TABULAR_DOCUMENT_SECTIONS_H
#define TABULAR_DOCUMENT_SECTIONS_H

// A .tabular file cut into its bracket lines and the CSV text under each. Not a
// public header.

#include "sexpr.h"

namespace tabular::detail
{

struct section
{
	sexpr            header;       // the bracket, parsed as a list: `[table t ...]` reads as `(table t ...)`
	std::string_view body;         // the CSV text up to the next bracket line; may be empty
	uint32_t         body_line = 0;
};

// A bracket line starts with '[' at column 0, outside an open CSV quote (a quoted
// cell may span lines, and a continuation line may start with '['). It ends at
// the ']' that closes it, outside strings and parentheses, and may span lines;
// only a '#' comment may follow that ']' on its line. Before the first bracket
// only blank and '#' lines may appear. The head word is not checked here.
// Every sexpr and body views into text. Errors name label and a file line.
tl::expected<std::vector<section>, ErrorContext> SplitSections(std::string_view text, std::string_view label);

}

#endif // TABULAR_DOCUMENT_SECTIONS_H
