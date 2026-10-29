#include "sexpr.h"

namespace tabular::detail
{

namespace
{

bool IsSpace(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

class reader
{
public:
	reader(std::string_view text, uint32_t line, std::string_view label) : _text(text), _line(line), _label(label) {}

	// Items up to a ')' (consumed) when nested, or to the end when not.
	tl::expected<void, ErrorContext> Items(sexpr& list, bool nested, uint32_t openedOn, size_t depth)
	{
		for (;;)
		{
			SkipSpace();
			if (_at == _text.size())
			{
				if (nested)
				{
					return Fail("the '(' is never closed", openedOn);
				}
				return {};
			}
			char const c = _text[_at];
			if (c == ')')
			{
				if (!nested)
				{
					return Fail("a ')' closes nothing", _line);
				}
				++_at;
				return {};
			}
			sexpr item;
			item.line = _line;
			if (c == '(')
			{
				if (depth >= kMaxDepth)
				{
					return Fail("lists nest too deep; the limit is 64", _line);
				}
				++_at;
				item.what = sexpr::kind::List;
				auto inner = Items(item, true, item.line, depth + 1);
				if (!inner)
				{
					return inner;
				}
			}
			else if (c == '"')
			{
				auto string = String();
				if (!string)
				{
					return tl::make_unexpected(std::move(string.error()));
				}
				item.what = sexpr::kind::String;
				item.text = *string;
			}
			else
			{
				size_t const begin = _at;
				while (_at < _text.size() && !IsSpace(_text[_at]) && _text[_at] != '(' && _text[_at] != ')'
					&& _text[_at] != '"')
				{
					++_at;
				}
				item.what = sexpr::kind::Atom;
				item.text = _text.substr(begin, _at - begin);
			}
			list.items.push_back(std::move(item));
		}
	}

	// A '#' at the start of a token comments out the rest of its line, so a
	// multi-line bracket can annotate its options.
	void SkipSpace()
	{
		while (_at < _text.size())
		{
			char const c = _text[_at];
			if (c == '#')
			{
				while (_at < _text.size() && _text[_at] != '\n')
				{
					++_at;
				}
				continue;
			}
			if (!IsSpace(c))
			{
				break;
			}
			if (c == '\n')
			{
				++_line;
			}
			++_at;
		}
	}

	tl::unexpected<ErrorContext> Fail(std::string_view what, uint32_t line) const
	{
		return tl::make_unexpected(ErrorContext(what).with(cc(std::string(), _label, ":", line)));
	}

	bool AtEnd() const { return _at == _text.size(); }
	char Peek() const { return _text[_at]; }
	void Skip() { ++_at; }
	uint32_t line() const { return _line; }

private:
	tl::expected<std::string_view, ErrorContext> String()
	{
		uint32_t const openedOn = _line;
		++_at;   // the opening quote
		size_t const begin = _at;
		while (_at < _text.size())
		{
			char const c = _text[_at];
			if (c == '\\')
			{
				if (_at + 1 >= _text.size() || (_text[_at + 1] != '"' && _text[_at + 1] != '\\'))
				{
					return Fail("a string holds a backslash that is not \\\" or \\\\; those are its only escapes", _line);
				}
				_at += 2;
				continue;
			}
			if (c == '"')
			{
				std::string_view const contents = _text.substr(begin, _at - begin);
				++_at;
				return contents;
			}
			if (c == '\n')
			{
				++_line;
			}
			++_at;
		}
		return Fail("the string is never closed", openedOn);
	}

	std::string_view _text;
	size_t           _at   = 0;
	uint32_t         _line;
	std::string_view _label;

	static constexpr size_t kMaxDepth = 64;
};

}

tl::expected<sexpr, ErrorContext> ParseItems(std::string_view inner, uint32_t line, std::string_view label)
{
	reader in(inner, line, label);
	sexpr  list;
	list.what = sexpr::kind::List;
	list.line = line;
	auto read = in.Items(list, false, line, 0);
	if (!read)
	{
		return tl::make_unexpected(std::move(read.error()));
	}
	return list;
}

tl::expected<sexpr, ErrorContext> ParseList(std::string_view text, uint32_t line, std::string_view label)
{
	reader in(text, line, label);
	in.SkipSpace();
	if (in.AtEnd() || in.Peek() != '(')
	{
		return in.Fail("expected a list starting with '('", in.line());
	}
	sexpr list;
	list.what = sexpr::kind::List;
	list.line = in.line();
	in.Skip();
	auto read = in.Items(list, true, list.line, 1);
	if (!read)
	{
		return tl::make_unexpected(std::move(read.error()));
	}
	in.SkipSpace();
	if (!in.AtEnd())
	{
		return in.Fail("text after the list's closing ')'", in.line());
	}
	return list;
}

std::string Unescape(sexpr const& string)
{
	std::string out;
	out.reserve(string.text.size());
	for (size_t i = 0; i < string.text.size(); ++i)
	{
		if (string.text[i] == '\\' && i + 1 < string.text.size())
		{
			++i;
		}
		out += string.text[i];
	}
	return out;
}

}
