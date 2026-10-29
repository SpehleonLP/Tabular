#include "csv_cells.h"
#include "table_layout.h"
#include "csv_text.h"

#include <cctype>
#include <string>
#include <vector>

namespace tabular::detail
{

namespace
{

// The CSV reader trims whole cells; a combo's parts are trimmed here.
std::string_view Trim(std::string_view s)
{
	while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
	{
		s.remove_prefix(1);
	}
	while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
	{
		s.remove_suffix(1);
	}
	return s;
}

bool IsNone(std::string_view name)
{
	if (name.size() != 4)
	{
		return false;
	}
	char const* none = "none";
	for (size_t i = 0; i < 4; ++i)
	{
		if ((name[i] | 0x20) != none[i])
		{
			return false;
		}
	}
	return true;
}

struct flag_header
{
	std::string name;
	uint32_t    flags  = 0;
	bool        simple = true;
};

// The flag domain the headers spell, read once, left to right.
tl::expected<std::vector<flag_header>, ErrorContext> ReadHeaders(std::vector<std::string_view> const& headers,
	bool firstIsNone)
{
	std::vector<flag_header> out;
	uint32_t nextBit = 0;
	for (size_t h = 0; h < headers.size(); ++h)
	{
		std::string_view const name = headers[h];
		if (name.empty())
		{
			return tl::make_unexpected(ErrorContext("flag header ", h + 1, " is empty"));
		}
		if (h == 0 && firstIsNone)
		{
			if (!IsNone(name))
			{
				return tl::make_unexpected(ErrorContext("the first flag header is None, the 0 value; got '", name, "'"));
			}
			out.push_back({std::string(name), 0, false});
			continue;
		}
		// None is the 0 value, a best table's first header; spelled anywhere
		// else it would silently become a flag bit that no one meant.
		if (IsNone(name))
		{
			return tl::make_unexpected(ErrorContext("flag header ", h + 1, " is '", name,
				"'; None names the 0 value, which only a best table's first header is"));
		}
		for (flag_header const& earlier : out)
		{
			if (earlier.name == name)
			{
				return tl::make_unexpected(ErrorContext("flag header '", name, "' twice"));
			}
		}
		if (name.find('|') == std::string_view::npos)
		{
			if (nextBit == 32)
			{
				return tl::make_unexpected(ErrorContext("flag header '", name, "' would be a 33rd bit; a flag set has 32 bits"));
			}
			out.push_back({std::string(name), uint32_t(1) << nextBit++, true});
			continue;
		}
		uint32_t         flags = 0;
		std::string_view rest  = name;
		while (true)
		{
			size_t const     bar  = rest.find('|');
			std::string_view part = Trim(rest.substr(0, bar));
			uint32_t         found = 0;
			for (flag_header const& earlier : out)
			{
				if (earlier.simple && earlier.name == part)
				{
					found = earlier.flags;
				}
			}
			if (found == 0)
			{
				return tl::make_unexpected(ErrorContext("combo header '", name, "' names '", part,
					"', which is not a simple header to its left"));
			}
			flags |= found;
			if (bar == std::string_view::npos)
			{
				break;
			}
			rest = rest.substr(bar + 1);
		}
		// Two spellings of one flag set would be two columns with one rank,
		// and nothing but row order to tell them apart: refused by value.
		for (flag_header const& earlier : out)
		{
			if (earlier.flags == flags)
			{
				return tl::make_unexpected(ErrorContext("flag headers '", earlier.name, "' and '", name,
					"' are the same flag set; headers are distinct flag sets"));
			}
		}
		out.push_back({std::string(name), flags, false});
	}
	return out;
}

}

tl::expected<void, ErrorContext> LoadPivotedCsv(relation_store& store, csv_rows const& command)
{
	auto             label = [&](uint32_t line) { return cc(std::string(), command.label, ":", line); };
	table_def const& def   = store.definition().tables()[command.table];
	if (def.pivot_headers == UINT32_MAX)
	{
		return tl::make_unexpected(ErrorContext("table '", def.name, "' has no pivot").with(std::string(command.label)));
	}

	// Slots as the store numbers them: keys and headers are values; the cells
	// column is a value, a band or a string.
	slot_layout const layout(def);
	std::vector<uint32_t> const& slot       = layout.slot;
	uint32_t const               valueCount = layout.valueCount;
	column_def const&            cells      = def.columns[def.pivot_cells];

	auto read = command.record_lines.empty() ? csv_text::Read(command.text, command.label, command.first_line)
		: csv_text::Read(command.text, command.label, command.record_lines);
	if (!read)
	{
		return tl::make_unexpected(std::move(read.error()));
	}
	csv_text const& file = *read;
	if (file.rows() == 0)
	{
		return tl::make_unexpected(ErrorContext("no header row for pivoted table '", def.name, "'")
			.with(std::string(command.label)));
	}
	// Trailing empty header cells are a spreadsheet's padding.
	size_t const width      = file.used_width(0);
	uint32_t const headerLine = file.line_of_row(0);

	// The keys, in key order (a picked table's keys are its leading order columns).
	if (width < def.key_count)
	{
		return tl::make_unexpected(ErrorContext("the header names ", width, " columns; table '", def.name,
			"' has ", def.key_count, " keys").with(label(headerLine)));
	}
	for (uint32_t k = 0; k < def.key_count; ++k)
	{
		column_def const& key = def.columns[def.order[k]];
		if (file.cell(k, 0) != key.name)
		{
			return tl::make_unexpected(ErrorContext("header cell ", k + 1, " is key column '", key.name, "'; got '",
				file.cell(k, 0), "'").with(label(headerLine)));
		}
	}
	// No flag headers would load no rows: a silent retract of the owner.
	if (width == def.key_count)
	{
		return tl::make_unexpected(ErrorContext("the header names no flag headers after the keys of '", def.name, "'")
			.with(label(headerLine)));
	}
	std::vector<std::string_view> names;
	for (size_t h = def.key_count; h < width; ++h)
	{
		names.push_back(file.cell(h, 0));
	}
	auto headers = ReadHeaders(names, def.pick == pick_policy::Best);
	if (!headers)
	{
		return tl::make_unexpected(std::move(headers.error()).with(label(headerLine)));
	}

	std::vector<cell>      values;
	std::vector<string_in> strings;
	std::vector<uint32_t>  lineOf;   // per loaded row
	hash_names             hashed;
	for (size_t r = 1; r < file.rows(); ++r)
	{
		uint32_t const line = file.line_of_row(r);
		// A spreadsheet pads every row to its widest with empty cells, so
		// trailing empties past the header say nothing.
		if (file.width(r) < width || file.used_width(r) > width)
		{
			return tl::make_unexpected(ErrorContext("row has ", file.used_width(r), " cells, but the header has ", width)
				.with(label(line)));
		}
		std::vector<cell> keys;
		for (uint32_t k = 0; k < def.key_count; ++k)
		{
			auto key = ReadCell(def, def.order[k], file.cell(k, r), hashed, line);
			if (!key)
			{
				return tl::make_unexpected(std::move(key.error()).with(label(line)));
			}
			keys.push_back(*key);
		}
		for (size_t h = 0; h < headers->size(); ++h)
		{
			std::string_view const cellText = file.cell(def.key_count + h, r);
			if (cellText.empty())
			{
				continue;   // a blank cell is no row
			}
			std::vector<cell> row(valueCount);
			for (uint32_t k = 0; k < def.key_count; ++k)
			{
				row[slot[def.order[k]]] = keys[k];
			}
			row[slot[def.pivot_headers]] = cell::Integer((*headers)[h].flags);
			if (cells.type == column_type::String)
			{
				strings.emplace_back(cellText);   // blanks were skipped above
			}
			else
			{
				auto value = ReadCell(column_def{(*headers)[h].name, cells.type}, cellText, hashed, line);
				if (!value)
				{
					return tl::make_unexpected(std::move(value.error()).with(label(line)));
				}
				row[slot[def.pivot_cells]] = *value;
			}
			values.insert(values.end(), row.begin(), row.end());
			lineOf.push_back(line);
		}
	}

	load_refusal refusal;
	auto loaded = store.Load(load_rows{command.table, command.owner, lineOf.size(), values, {}, &refusal, {},
		strings, command.report});
	if (!loaded)
	{
		return tl::make_unexpected(LocateRefusal(std::move(loaded.error()), refusal, lineOf.size(), command.label,
			[&](size_t row) { return lineOf[row]; }));
	}
	// Only now: a refused load changes nothing, the caller's names included.
	if (command.flag_names)
	{
		command.flag_names->clear();
		for (flag_header const& h : *headers)
		{
			if (h.simple)
			{
				command.flag_names->push_back(h.name);
			}
		}
	}
	return {};
}

}
