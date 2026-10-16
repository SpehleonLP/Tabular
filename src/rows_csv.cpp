#include "tabular/rows_csv.h"
#include "Utility/load_csv.h"
#include "cityhash/src/city.h"

#include <charconv>
#include <type_traits>
#include <string>
#include <vector>

namespace tabular
{

namespace
{

// The whole of text as T, or nothing: "12abc" is not 12.
template<typename T>
bool ParseWhole(std::string_view text, T& out, int base = 10)
{
	char const* end = text.data() + text.size();
	std::from_chars_result r;
	if constexpr (std::is_floating_point_v<T>)
	{
		r = std::from_chars(text.data(), end, out);
	}
	else
	{
		r = std::from_chars(text.data(), end, out, base);
	}
	return r.ec == std::errc() && r.ptr == end;
}

// Closures built for the cells read so far. Load adopts them all, refused or
// not; until it is called they are ours, so an early return releases them.
struct pending_conditions
{
	std::vector<closure> list;
	bool                 handedOver = false;

	explicit pending_conditions(size_t count) : list(count) {}
	pending_conditions(pending_conditions const&)            = delete;
	pending_conditions& operator=(pending_conditions const&) = delete;
	~pending_conditions()
	{
		if (handedOver)
		{
			return;
		}
		for (closure const& c : list)
		{
			if (c.release)
			{
				c.release(c.data);
			}
		}
	}
};

tl::expected<cell, ErrorContext> ReadCell(column_def const& column, std::string_view text)
{
	if (text.empty())
	{
		return tl::make_unexpected(ErrorContext("column '", column.name, "' is empty"));
	}
	switch (column.type)
	{
	case column_type::Hash:
		return cell::Integer(static_cast<int64_t>(CityHash32(text.data(), text.size())));
	case column_type::Flags:
	{
		uint64_t v    = 0;
		bool     hex  = text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
		bool     read = hex ? ParseWhole(text.substr(2), v, 16) : ParseWhole(text, v);
		if (!read || v > UINT64_C(0xFFFFFFFF))
		{
			return tl::make_unexpected(ErrorContext("column '", column.name, "' is a flag set, unsigned decimal or 0x hex below 2^32; got '", text, "'"));
		}
		return cell::Integer(static_cast<int64_t>(v));
	}
	case column_type::Int:
	{
		int64_t v = 0;
		if (!ParseWhole(text, v))
		{
			return tl::make_unexpected(ErrorContext("column '", column.name, "' is an integer; got '", text, "'"));
		}
		return cell::Integer(v);
	}
	case column_type::Double:
	{
		double v = 0;
		if (!ParseWhole(text, v))
		{
			return tl::make_unexpected(ErrorContext("column '", column.name, "' is a number; got '", text, "'"));
		}
		return cell::Real(v);
	}
	case column_type::Vec2:
	{
		size_t const dots = text.find("..");
		float        x    = 0;
		float        y    = 0;
		if (dots == std::string_view::npos || !ParseWhole(text.substr(0, dots), x) || !ParseWhole(text.substr(dots + 2), y))
		{
			return tl::make_unexpected(ErrorContext("column '", column.name, "' is a vec2, written a..b; got '", text, "'"));
		}
		return cell::Vector(x, y);
	}
	default:
		return tl::make_unexpected(ErrorContext("column '", column.name, "' holds no value"));
	}
}

}

tl::expected<void, ErrorContext> LoadCsv(relation_store& store, csv_rows const& command)
{
	auto label = [&](uint32_t line) { return cc(std::string(), command.label, ":", line); };
	auto defs  = store.definition().tables();
	if (command.table >= defs.size())
	{
		return tl::make_unexpected(ErrorContext("no table ", command.table).with(std::string(command.label)));
	}
	table_def const& def = defs[command.table];
	if (!command.condition)
	{
		for (column_def const& column : def.columns)
		{
			if (column.type == column_type::Condition)
			{
				return tl::make_unexpected(ErrorContext("table '", def.name, "' has condition column '", column.name,
					"', and no condition function was given").with(std::string(command.label)));
			}
		}
	}

	// Each column's position among the columns of its kind, as the store numbers them.
	std::vector<uint32_t> slot(def.columns.size());
	uint32_t valueCount                         = 0;
	uint32_t commandCount                       = 0;
	uint32_t stringCount                        = 0;
	uint32_t conditionCount                     = 0;
	for (uint32_t c = 0; c < def.columns.size(); ++c)
	{
		switch (def.columns[c].type)
		{
		case column_type::Command:   slot[c] = commandCount++;   break;
		case column_type::String:    slot[c] = stringCount++;    break;
		case column_type::Condition: slot[c] = conditionCount++; break;
		default:                     slot[c] = valueCount++;     break;
		}
	}

	// A BOM is the editor's, not the first column's name. Unicode is left as
	// written: a Hash column must hash the bytes a script literal hashes.
	std::string_view text = command.text;
	if (text.starts_with("\xEF\xBB\xBF"))
	{
		text.remove_prefix(3);
	}
	CSV::Options options;
	options.normalizeUnicode = false;
	CSV::File file(text, options, command.first_line);
	if (file.rows() == 0)
	{
		return tl::make_unexpected(ErrorContext("no header row naming the columns of '", def.name, "'")
			.with(std::string(command.label)));
	}

	// Header cell -> column.
	size_t const          width = file[0].size();
	std::vector<uint32_t> columnOf(width);
	std::vector<bool>     named(def.columns.size(), false);
	for (size_t h = 0; h < width; ++h)
	{
		std::string_view name  = file.cell(h, 0);
		auto             found = store.definition().FindColumn(command.table, name);
		if (!found)
		{
			return tl::make_unexpected(std::move(found.error()).with(label(file.line_of_row(0))));
		}
		if (named[*found])
		{
			return tl::make_unexpected(ErrorContext("the header names column '", name, "' twice").with(label(file.line_of_row(0))));
		}
		named[*found] = true;
		columnOf[h]   = *found;
	}
	for (uint32_t c = 0; c < def.columns.size(); ++c)
	{
		if (!named[c])
		{
			return tl::make_unexpected(ErrorContext("the header does not name column '", def.columns[c].name, "'")
				.with(label(file.line_of_row(0))));
		}
	}

	size_t const                  count = file.rows() - 1;
	std::vector<cell>             values(count * valueCount);
	std::vector<std::string_view> commands(count * commandCount);
	std::vector<string_in>        strings(count * stringCount);
	pending_conditions            conditions(count * conditionCount);
	for (size_t r = 0; r < count; ++r)
	{
		uint32_t line = file.line_of_row(r + 1);
		if (file[r + 1].size() != width)
		{
			return tl::make_unexpected(ErrorContext("row has ", file[r + 1].size(), " cells, but the header has ", width)
				.with(label(line)));
		}
		for (size_t h = 0; h < width; ++h)
		{
			column_def const& column = def.columns[columnOf[h]];
			std::string_view  text   = file.cell(h, r + 1);
			if (column.type == column_type::Command)
			{
				commands[r * commandCount + slot[columnOf[h]]] = text;
				continue;
			}
			if (column.type == column_type::String)
			{
				if (text.empty())
				{
					return tl::make_unexpected(ErrorContext("column '", column.name, "' is empty").with(label(line)));
				}
				strings[r * stringCount + slot[columnOf[h]]] = text;
				continue;
			}
			if (column.type == column_type::Condition)
			{
				auto built = command.condition(text);
				if (!built)
				{
					return tl::make_unexpected(std::move(built.error())
						.with("column '", column.name, "'").with(label(line)));
				}
				conditions.list[r * conditionCount + slot[columnOf[h]]] = *built;
				continue;
			}
			auto value = ReadCell(column, text);
			if (!value)
			{
				return tl::make_unexpected(std::move(value.error()).with(label(line)));
			}
			values[r * valueCount + slot[columnOf[h]]] = *value;
		}
	}

	load_refusal refusal;
	conditions.handedOver = true;   // Load adopts them, whatever it decides
	auto loaded = store.Load(load_rows{command.table, command.owner, count, values, commands, &refusal,
		conditions.list, strings});
	if (!loaded)
	{
		ErrorContext error = std::move(loaded.error());
		if (refusal.earlier < count)
		{
			error = std::move(error).with("same key as line ", file.line_of_row(refusal.earlier + 1));
		}
		if (refusal.row < count)
		{
			return tl::make_unexpected(std::move(error).with(label(file.line_of_row(refusal.row + 1))));
		}
		return tl::make_unexpected(std::move(error).with(std::string(command.label)));
	}
	return {};
}

}
