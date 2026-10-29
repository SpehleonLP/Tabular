#include "csv_text.h"

#include "Support/strings.h"

namespace tabular::detail
{

namespace
{

bool IsBlank(char c)
{
	return c == ' ' || c == '\t' || c == '\r';
}

std::string_view Trim(std::string_view text)
{
	while (!text.empty() && IsBlank(text.front()))
	{
		text.remove_prefix(1);
	}
	while (!text.empty() && IsBlank(text.back()))
	{
		text.remove_suffix(1);
	}
	return text;
}

}

tl::expected<csv_text, ErrorContext> csv_text::Read(std::string_view text, std::string_view label, uint32_t first_line)
{
	// A BOM is the editor's, not the first column's name. Unicode is left as
	// written: a Hash column must hash the bytes a script literal hashes.
	if (text.starts_with("\xEF\xBB\xBF"))
	{
		text.remove_prefix(3);
	}
	auto at = [&](uint32_t line) { return cc(std::string(), label, ":", line); };

	csv_text out;
	out._text.reserve(text.size());
	size_t   pos  = 0;
	uint32_t line = first_line;
	while (pos < text.size())
	{
		uint32_t const rowLine  = line;
		uint32_t const first    = static_cast<uint32_t>(out._cells.size());
		bool           anyText  = false;
		bool           comment  = false;
		bool           rowEnded = false;
		size_t         cellIndex = 0;
		while (!rowEnded)
		{
			while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t'))
			{
				++pos;
			}
			// A row is a comment when its first unquoted character is '#', so
			// a quoted "#3" is a cell and a spreadsheet's "# note,,," a comment.
			if (cellIndex == 0 && pos < text.size() && text[pos] == '#')
			{
				comment = true;
			}
			cell_span span{static_cast<uint32_t>(out._text.size()), 0};
			if (!comment && pos < text.size() && text[pos] == '"')
			{
				uint32_t const opened = line;
				++pos;
				for (;;)
				{
					if (pos >= text.size())
					{
						return tl::make_unexpected(ErrorContext("a quoted cell is never closed").with(at(opened)));
					}
					char const c = text[pos++];
					if (c == '"')
					{
						if (pos < text.size() && text[pos] == '"')
						{
							out._text.push_back('"');
							++pos;
							continue;
						}
						break;
					}
					if (c == '\n')
					{
						++line;
					}
					out._text.push_back(c);
				}
				while (pos < text.size() && IsBlank(text[pos]))
				{
					++pos;
				}
				if (pos < text.size() && text[pos] != ',' && text[pos] != '\n')
				{
					return tl::make_unexpected(ErrorContext("text follows the closing quote of a cell; a quote inside a quoted cell is written twice")
						.with(at(line)));
				}
			}
			else
			{
				size_t end = pos;
				while (end < text.size() && text[end] != '\n' && (comment || text[end] != ','))
				{
					++end;
				}
				std::string_view const raw = Trim(text.substr(pos, end - pos));
				pos = end;
				if (!comment && raw.find('"') != std::string_view::npos)
				{
					return tl::make_unexpected(ErrorContext("a cell that is not quoted holds '\"'; a cell holding quotes is quoted whole, each quote inside written twice")
						.with(at(line)));
				}
				out._text.append(raw);
			}
			span.size = static_cast<uint32_t>(out._text.size()) - span.offset;
			anyText   = anyText || span.size > 0;
			out._cells.push_back(span);
			++cellIndex;
			if (pos < text.size() && text[pos] == ',')
			{
				++pos;
				continue;
			}
			// The end of the line, or of the text.
			if (pos < text.size())
			{
				++pos;
				++line;
			}
			rowEnded = true;
		}
		// A comment row, and a blank one (a spreadsheet writes an empty row as
		// its separators), are not rows.
		if (comment || !anyText)
		{
			out._text.resize(out._cells[first].offset);
			out._cells.resize(first);
			continue;
		}
		out._rows.push_back(row_span{first, static_cast<uint32_t>(out._cells.size()) - first, rowLine});
	}
	return out;
}

tl::expected<csv_text, ErrorContext> csv_text::Read(std::string_view text, std::string_view label,
	std::span<uint32_t const> record_lines)
{
	auto read = Read(text, label, uint32_t(1));
	if (!read)
	{
		return read;
	}
	if (read->_rows.size() != record_lines.size())
	{
		return tl::make_unexpected(ErrorContext("the text has ", read->_rows.size(), " records and ", record_lines.size(),
			" source lines for them").with(std::string(label)));
	}
	for (size_t r = 0; r < record_lines.size(); ++r)
	{
		read->_rows[r].line = record_lines[r];
	}
	return read;
}

}

namespace tabular
{

size_t cell_rows::used_width(size_t row) const
{
	size_t width = _rows[row].count;
	while (width > 0 && _cells[_rows[row].first + width - 1].size == 0)
	{
		--width;
	}
	return width;
}

std::string_view cell_rows::cell(size_t column, size_t row) const
{
	if (row >= _rows.size() || column >= _rows[row].count)
	{
		return {};
	}
	cell_span const& span = _cells[_rows[row].first + column];
	return std::string_view(_text).substr(span.offset, span.size);
}

tl::expected<cell_rows, ErrorContext> ReadCells(std::string_view text, std::string_view label, uint32_t first_line)
{
	auto read = detail::csv_text::Read(text, label, first_line);
	if (!read)
	{
		return tl::make_unexpected(std::move(read.error()));
	}
	// csv_text adds only its reader, so the rows it read are all of it.
	return cell_rows(std::move(*read));
}

}
