#include "command_text.h"

namespace tabular::detail
{

namespace
{

bool IsSpace(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

std::string_view Trim(std::string_view text)
{
	while (!text.empty() && IsSpace(text.front()))
	{
		text.remove_prefix(1);
	}
	while (!text.empty() && IsSpace(text.back()))
	{
		text.remove_suffix(1);
	}
	return text;
}

// `()`, `( )` and the like: the explicit "do nothing", which adds nothing.
bool IsEmptyList(std::string_view text)
{
	if (text.size() < 2 || text.front() != '(' || text.back() != ')')
	{
		return false;
	}
	return Trim(text.substr(1, text.size() - 2)).empty();
}

// Where a lex stands, carried across the cells of one row, since a list may
// run over several.
struct command_lexer
{
	size_t depth     = 0;
	size_t items     = 0;       // top-level items begun
	bool   inString  = false;
	bool   escaped   = false;
	bool   inAtom    = false;
	bool   overClosed = false;  // a ')' closed nothing

	bool Balanced() const { return depth == 0 && !inString && !overClosed; }

	void Feed(std::string_view text)
	{
		for (char const c : text)
		{
			if (inString)
			{
				if (escaped)
				{
					escaped = false;
				}
				else if (c == '\\')
				{
					escaped = true;
				}
				else if (c == '"')
				{
					inString = false;
				}
				continue;
			}
			if (IsSpace(c))
			{
				inAtom = false;
				continue;
			}
			bool const begins = c == '(' || c == '"' || (c != ')' && !inAtom);
			if (begins && depth == 0)
			{
				++items;
			}
			inAtom = false;
			if (c == '(')
			{
				++depth;
			}
			else if (c == ')')
			{
				if (depth == 0)
				{
					overClosed = true;
				}
				else
				{
					--depth;
				}
			}
			else if (c == '"')
			{
				inString = true;
			}
			else
			{
				inAtom = true;
			}
		}
	}

	// Cells are joined with a space, which ends an atom (inside a string it is
	// the string's own text).
	void EndCell()
	{
		inAtom = false;
	}
};

}

tl::expected<void, ErrorContext> CheckCommandList(std::string_view command)
{
	command_lexer lex;
	lex.Feed(command);
	if (lex.overClosed)
	{
		return tl::make_unexpected(ErrorContext("the command '", command, "' has a ')' that closes nothing"));
	}
	if (lex.inString)
	{
		return tl::make_unexpected(ErrorContext("the command '", command, "' has a string that is never closed"));
	}
	if (lex.depth != 0)
	{
		return tl::make_unexpected(ErrorContext("the command '", command, "' has a '(' that is never closed"));
	}
	// Balanced and one item: that item is a list only when it opens with '('.
	std::string_view const text = Trim(command);
	if (lex.items != 1 || text.front() != '(')
	{
		return tl::make_unexpected(ErrorContext("the command '", command,
			"' is not one list; a row's commands are one list, like ((anim chew) idle), and () for none"));
	}
	return {};
}

tl::expected<std::string, ErrorContext> CommandList(std::span<std::string_view const> cells)
{
	std::string   out = "(";
	command_lexer across;
	for (std::string_view raw : cells)
	{
		std::string_view const text = Trim(raw);
		bool const topLevel = across.depth == 0 && !across.inString;
		if (text.empty() || (topLevel && IsEmptyList(text)))
		{
			continue;
		}
		if (out.size() > 1)
		{
			out += ' ';
		}
		if (topLevel && text.front() != '(')
		{
			command_lexer own;
			own.Feed(text);
			if (own.items > 1 && !own.Balanced())
			{
				return tl::make_unexpected(ErrorContext("the cell '", text, "' holds several items and is not whole by "
					"itself; a command that runs across cells starts with '('"));
			}
			if (own.items > 1)
			{
				out += '(';
				out += text;
				out += ')';
				continue;
			}
		}
		out += text;
		across.Feed(text);
		across.EndCell();
		// Cells are quoted (RFC 4180), so a string open at a cell's end is a
		// comma inside it that split the cell: `(say "hi, there")` as two.
		// Joining with a space would silently drop the comma and the spaces
		// around it.
		if (across.inString)
		{
			return tl::make_unexpected(ErrorContext("the cell '", text, "' has a string that is never closed within "
				"it; a string holding a comma is written whole in one quoted cell, as a spreadsheet saves it"));
		}
	}
	out += ')';
	auto checked = CheckCommandList(out);
	if (!checked)
	{
		return tl::make_unexpected(std::move(checked.error()));
	}
	return out;
}

}
