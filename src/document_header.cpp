#include "document_header.h"

namespace tabular::detail
{

namespace
{

// In column_type's order: the index is the enumerator.
constexpr std::string_view kTypeNames[] = {"hash", "flags", "int", "double", "condition", "command", "vec2", "string",
	"band"};

bool IsSpace(char c)
{
	return c == ' ' || c == '\t' || c == '\r';
}

// What ends a name or a type: the grammar's punctuation, or a space.
bool EndsWord(char c)
{
	return IsSpace(c) || c == '?' || c == ':' || c == '=' || c == '>' || c == '^';
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

std::string_view TakeWord(std::string_view& rest)
{
	size_t end = 0;
	while (end < rest.size() && !EndsWord(rest[end]))
	{
		++end;
	}
	std::string_view const word = rest.substr(0, end);
	rest.remove_prefix(end);
	return word;
}

void SkipSpace(std::string_view& rest)
{
	while (!rest.empty() && IsSpace(rest.front()))
	{
		rest.remove_prefix(1);
	}
}

constexpr char const* kGrammar = "; a header cell is [>|^] name [?] [: type] [= default]";

tl::unexpected<ErrorContext> Malformed(std::string_view cell, std::string_view why)
{
	return tl::make_unexpected(ErrorContext("header cell '", cell, "' ", why, kGrammar));
}

}

std::optional<column_type> TypeNamed(std::string_view name)
{
	for (size_t t = 0; t < std::size(kTypeNames); ++t)
	{
		if (name == kTypeNames[t])
		{
			return static_cast<column_type>(t);
		}
	}
	return std::nullopt;
}

tl::expected<header_cell, ErrorContext> ParseHeaderCell(std::string_view cell)
{
	header_cell out;
	out.text = Trim(cell);
	if (out.text.empty())
	{
		return tl::make_unexpected(ErrorContext("an empty header cell names no column", kGrammar));
	}
	std::string_view rest = out.text;
	if (rest.front() == '>' || rest.front() == '^')
	{
		out.what = rest.front() == '>' ? header_cell::mark::Key : header_cell::mark::Edge;
		rest.remove_prefix(1);
	}
	out.name = TakeWord(rest);
	if (out.name.empty())
	{
		return Malformed(out.text, "has no name");
	}
	if (!rest.empty() && rest.front() == '?')
	{
		if (out.what != header_cell::mark::Key)
		{
			return Malformed(out.text, "has '?', which only a key ('>name?') takes");
		}
		out.fallthrough = true;
		rest.remove_prefix(1);
	}
	SkipSpace(rest);
	if (!rest.empty() && rest.front() == ':')
	{
		rest.remove_prefix(1);
		SkipSpace(rest);
		std::string_view const type = TakeWord(rest);
		if (type.empty())
		{
			return Malformed(out.text, "has ':' and no type after it");
		}
		out.type = TypeNamed(type);
		if (!out.type)
		{
			return Malformed(out.text, "has the unknown type '" + std::string(type) +
				"'; a type is hash, flags, int, double, condition, command, vec2, string or band");
		}
		SkipSpace(rest);
	}
	if (!rest.empty() && rest.front() == '=')
	{
		if (out.what == header_cell::mark::Key)
		{
			return Malformed(out.text, "gives a key a default; a key's cell is what the row is for");
		}
		rest.remove_prefix(1);
		std::string_view const fallback = Trim(rest);
		if (fallback.empty())
		{
			return Malformed(out.text, "has '=' and no default after it");
		}
		if (fallback.find('=') != std::string_view::npos)
		{
			return Malformed(out.text, "has a second '='");
		}
		out.fallback = fallback;
		rest         = {};
	}
	if (!rest.empty())
	{
		return Malformed(out.text, "has '" + std::string(rest) + "' after its name, type and default");
	}
	return out;
}

tl::expected<column_type, ErrorContext> TypeOfCell(header_cell const& cell)
{
	struct known
	{
		std::string_view name;
		column_type      type;
	};
	constexpr known kKnown[] = {
		{"when", column_type::Condition}, {"weight", column_type::Double}, {"command", column_type::Command}};
	for (known const& k : kKnown)
	{
		if (cell.name != k.name)
		{
			continue;
		}
		if (cell.type && *cell.type != k.type)
		{
			return tl::make_unexpected(ErrorContext("header cell '", cell.text, "': a column named '", k.name, "' is a ",
				kTypeNames[static_cast<size_t>(k.type)]));
		}
		return k.type;
	}
	if (cell.what == header_cell::mark::Edge)
	{
		if (cell.type && *cell.type != column_type::Band)
		{
			return tl::make_unexpected(ErrorContext("header cell '", cell.text, "': an edge ('^') is a band"));
		}
		return column_type::Band;
	}
	return cell.type.value_or(column_type::Hash);
}

}
