#include "document_sections.h"

namespace tabular::detail
{

namespace
{

bool IsBlank(char c)
{
	return c == ' ' || c == '\t' || c == '\r';
}

// Where a bracket line ends: the ']' that closes it, outside strings, parentheses
// and '#' comments. close is npos when the file ends first.
struct bracket_end
{
	size_t   close = std::string_view::npos;
	uint32_t line  = 0;   // the line the ']' is on
};

tl::expected<bracket_end, ErrorContext> FindClose(std::string_view text, size_t open, uint32_t openLine,
	std::string_view label)
{
	auto at = [&](uint32_t line) { return cc(std::string(), label, ":", line); };

	bracket_end           end;
	std::vector<uint32_t> parens;   // the line each open '(' is on
	bool     inString    = false;
	uint32_t stringLine  = 0;
	bool     tokenStart  = true;    // a '#' only comments at a token's start, as the sexpr reader reads it
	uint32_t line        = openLine;
	for (size_t i = open + 1; i < text.size(); ++i)
	{
		char const c = text[i];
		if (c == '\n')
		{
			++line;
		}
		if (inString)
		{
			if (c == '\\')
			{
				++i;
				if (i < text.size() && text[i] == '\n')
				{
					++line;
				}
			}
			else if (c == '"')
			{
				inString   = false;
				tokenStart = true;
			}
			continue;
		}
		if (c == '#' && tokenStart)
		{
			while (i + 1 < text.size() && text[i + 1] != '\n')
			{
				++i;
			}
			continue;
		}
		tokenStart = IsBlank(c) || c == '\n' || c == '(' || c == ')';
		if (c == '"')
		{
			inString   = true;
			stringLine = line;
		}
		else if (c == '(')
		{
			parens.push_back(line);
		}
		else if (c == ')')
		{
			if (!parens.empty())
			{
				parens.pop_back();
			}
		}
		else if (c == ']')
		{
			if (!parens.empty())
			{
				return tl::make_unexpected(ErrorContext("the '(' is never closed before the bracket's ']'")
					.with(at(parens.back())));
			}
			end.close = i;
			end.line  = line;
			return end;
		}
	}
	if (inString)
	{
		return tl::make_unexpected(ErrorContext("the string is never closed").with(at(stringLine)));
	}
	return end;
}

// csv_text's quoting rule: a '"' opens a quoted cell only at a cell's start, and
// inside one a doubled quote is a quote. Returns the quote state at the line's end;
// opened says a quote opened on this line and is the one still open, if any.
bool ScanCsvLine(std::string_view line, bool inQuote, bool& opened)
{
	opened = false;
	bool atCellStart = !inQuote;
	for (size_t i = 0; i < line.size(); ++i)
	{
		char const c = line[i];
		if (inQuote)
		{
			if (c == '"')
			{
				if (i + 1 < line.size() && line[i + 1] == '"')
				{
					++i;
				}
				else
				{
					inQuote = false;
				}
			}
		}
		else if (c == ',')
		{
			atCellStart = true;
		}
		else if (c == '"' && atCellStart)
		{
			inQuote     = true;
			opened      = true;
			atCellStart = false;
		}
		else if (!IsBlank(c))
		{
			atCellStart = false;
		}
	}
	return inQuote;
}

}

tl::expected<std::vector<section>, ErrorContext> SplitSections(std::string_view text, std::string_view label)
{
	auto at = [&](uint32_t line) { return cc(std::string(), label, ":", line); };

	std::vector<section> sections;
	size_t   pos      = 0;
	uint32_t line     = 1;
	size_t   bodyFrom = 0;
	bool     inQuote  = false;
	uint32_t quoteLine = 0;
	if (text.starts_with("\xEF\xBB\xBF"))
	{
		pos      = 3;
		bodyFrom = 3;
	}

	auto closeBody = [&](size_t end)
	{
		if (!sections.empty())
		{
			sections.back().body = text.substr(bodyFrom, end - bodyFrom);
		}
	};

	while (pos < text.size())
	{
		size_t const eol = text.find('\n', pos);
		size_t const end = eol == std::string_view::npos ? text.size() : eol;
		std::string_view const row = text.substr(pos, end - pos);

		if (!inQuote && !row.empty() && row[0] == '[')
		{
			closeBody(pos);
			auto closed = FindClose(text, pos, line, label);
			if (!closed)
			{
				return tl::make_unexpected(std::move(closed.error()));
			}
			bracket_end const found = *closed;
			if (found.close == std::string_view::npos)
			{
				return tl::make_unexpected(ErrorContext("the '[' is never closed; a bracket runs to its ']', and no ']' follows")
					.with(at(line)));
			}
			size_t const lineEnd = text.find('\n', found.close);
			size_t const after   = lineEnd == std::string_view::npos ? text.size() : lineEnd;
			std::string_view rest = text.substr(found.close + 1, after - found.close - 1);
			while (!rest.empty() && IsBlank(rest.front()))
			{
				rest.remove_prefix(1);
			}
			if (!rest.empty() && rest.front() != '#')
			{
				return tl::make_unexpected(ErrorContext("text after the bracket's ']'; only a # comment may follow it")
					.with(at(found.line)));
			}
			auto header = ParseItems(text.substr(pos + 1, found.close - pos - 1), line, label);
			if (!header)
			{
				return tl::make_unexpected(std::move(header.error()));
			}
			if (header->items.empty())
			{
				return tl::make_unexpected(ErrorContext("the bracket is empty").with(at(line)));
			}
			section made;
			made.header    = std::move(*header);
			made.body_line = found.line + 1;
			sections.push_back(std::move(made));
			line     = found.line + 1;
			pos      = lineEnd == std::string_view::npos ? text.size() : lineEnd + 1;
			bodyFrom = pos;
			continue;
		}

		if (!inQuote)
		{
			size_t first = 0;
			while (first < row.size() && IsBlank(row[first]))
			{
				++first;
			}
			bool const skippable = first == row.size() || row[first] == '#';
			if (!skippable && sections.empty())
			{
				return tl::make_unexpected(ErrorContext("rows before any bracket line; a file starts with a [table], [grid] or [machine]")
					.with(at(line)));
			}
			if (skippable)
			{
				pos = eol == std::string_view::npos ? text.size() : eol + 1;
				++line;
				continue;
			}
		}
		bool opened = false;
		inQuote     = ScanCsvLine(row, inQuote, opened);
		if (opened)
		{
			quoteLine = line;
		}
		pos     = eol == std::string_view::npos ? text.size() : eol + 1;
		++line;
	}
	if (inQuote)
	{
		return tl::make_unexpected(ErrorContext("a quoted cell is never closed; the file ends inside it").with(at(quoteLine)));
	}
	closeBody(text.size());
	return sections;
}

}
