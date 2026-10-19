#include "tabular/rows_csv.h"
#include "csv_cells.h"
#include "table_layout.h"
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

}

namespace detail
{

bool ParseRange(std::string_view text, float& start, float& end)
{
	size_t const dots = text.find("..");
	return dots != std::string_view::npos && text.find("...") == std::string_view::npos
		&& ParseWhole(text.substr(0, dots), start) && ParseWhole(text.substr(dots + 2), end);
}

tl::expected<void, ErrorContext> RefuseBlank(column_def const& column, std::string_view text)
{
	if (text.empty())
	{
		return tl::make_unexpected(ErrorContext("column '", column.name, "' is empty"));
	}
	return {};
}

tl::expected<cell, ErrorContext> ReadCell(column_def const& column, std::string_view text)
{
	auto present = RefuseBlank(column, text);
	if (!present)
	{
		return tl::make_unexpected(std::move(present.error()));
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
	case column_type::Band:
	{
		float x = 0;
		float y = 0;
		if (!ParseRange(text, x, y))
		{
			return tl::make_unexpected(ErrorContext("column '", column.name, "' is a ",
				column.type == column_type::Band ? "band" : "vec2", ", written a..b; got '", text, "'"));
		}
		// A band's well-formedness is Load's to check, where every front end's
		// rows enter a table and the refusal still maps to the CSV line.
		return cell::Vector(x, y);
	}
	default:
		return tl::make_unexpected(ErrorContext("column '", column.name, "' holds no value"));
	}
}

tl::expected<void, ErrorContext> hash_names::Note(std::string_view text, int64_t hash, uint32_t line)
{
	if (hash == 0)
	{
		return tl::make_unexpected(ErrorContext("'", text, "' hashes to 0, which no name may"));
	}
	// Looked up first: a key repeats on many lines, and only its first copy is kept.
	auto it = _seen.find(hash);
	if (it == _seen.end())
	{
		_seen.emplace(hash, std::pair<std::string, uint32_t>(std::string(text), line));
		return {};
	}
	if (it->second.first != text)
	{
		return tl::make_unexpected(ErrorContext("'", text, "' has the same hash as '", it->second.first,
			"' on line ", it->second.second));
	}
	return {};
}

tl::expected<cell, ErrorContext> ReadCell(column_def const& column, std::string_view text, hash_names& names,
	uint32_t line)
{
	auto value = ReadCell(column, text);
	if (value && column.type == column_type::Hash)
	{
		auto noted = names.Note(text, value->i, line);
		if (!noted)
		{
			return tl::make_unexpected(std::move(noted.error()));
		}
	}
	return value;
}

tl::expected<cell, ErrorContext> ReadCell(table_def const& def, uint32_t column, std::string_view text,
	hash_names& names, uint32_t line)
{
	if (text.empty() && IsMessageKey(def, column))
	{
		return cell::Integer(0);
	}
	return ReadCell(def.columns[column], text, names, line);
}

}

tl::expected<void, ErrorContext> LoadCsv(relation_store& store, csv_rows const& command)
{
	// A refusal leaves the report empty, and most refusals here come before
	// the store sees the load.
	if (command.report)
	{
		command.report->clear();
	}
	auto label = [&](uint32_t line) { return cc(std::string(), command.label, ":", line); };
	auto defs  = store.definition().tables();
	if (command.table >= defs.size())
	{
		return tl::make_unexpected(ErrorContext("no table ", command.table).with(std::string(command.label)));
	}
	table_def const& def = defs[command.table];
	if (command.pivoted)
	{
		return detail::LoadPivotedCsv(store, command);
	}

	// Each column's position among the columns of its kind, as the store numbers them.
	slot_layout const layout(def);
	std::vector<uint32_t> const& slot = layout.slot;
	uint32_t const valueCount     = layout.valueCount;
	uint32_t const commandCount   = layout.commandCount;
	uint32_t const stringCount    = layout.stringCount;
	uint32_t const conditionCount = layout.conditionCount;

	// A BOM is the editor's, not the first column's name. Unicode is left as
	// written: a Hash column must hash the bytes a script literal hashes.
	std::string_view text = command.text;
	if (text.starts_with("\xEF\xBB\xBF"))
	{
		text.remove_prefix(3);
	}
	CSV::Options options;
	options.normalizeUnicode = false;
	// A suffix table's last column is the rest of every line, commas included;
	// the reader already knows how to cut a row that way.
	options.restInLastColumn = def.suffix;
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
	// Where the rest of the line starts is where the header's last cell is, so
	// the suffix column must be that cell.
	if (def.suffix && columnOf[width - 1] != def.columns.size() - 1)
	{
		return tl::make_unexpected(ErrorContext("the suffix column '", def.columns.back().name,
			"' must be the header's last cell, because it takes the rest of each line").with(label(file.line_of_row(0))));
	}

	size_t const                  count = file.rows() - 1;
	std::vector<cell>             values(count * valueCount);
	std::vector<std::string_view> commands(count * commandCount);
	std::vector<string_in>        strings(count * stringCount);
	pending_conditions            conditions(count * conditionCount);
	detail::hash_names            names;
	for (size_t r = 0; r < count; ++r)
	{
		uint32_t line = file.line_of_row(r + 1);
		if (def.suffix && file[r + 1].size() + 1 == width)
		{
			return tl::make_unexpected(ErrorContext("row has no suffix cell; the text after the declared cells is the command '",
				def.columns.back().name, "', and it is missing").with(label(line)));
		}
		if (file[r + 1].size() != width)
		{
			return tl::make_unexpected(ErrorContext("row has ", file[r + 1].size(), " cells, but the header has ", width)
				.with(label(line)));
		}
		for (size_t h = 0; h < width; ++h)
		{
			column_def const& column = def.columns[columnOf[h]];
			std::string_view  text   = file.cell(h, r + 1);
			// The one blank cell CSV accepts is a condition's: "no condition",
			// which the loader must see to prove a table total.
			if (column.type == column_type::Condition && text.empty())
			{
				conditions.list[r * conditionCount + slot[columnOf[h]]] = closure{};
				continue;
			}
			if (column.type == column_type::Condition)
			{
				// Only a cell with text needs the function that compiles it.
				if (!command.condition)
				{
					return tl::make_unexpected(ErrorContext("table '", def.name, "' has condition column '", column.name,
						"', and no condition function was given").with(label(line)));
				}
				auto built = command.condition(text);
				if (!built)
				{
					return tl::make_unexpected(std::move(built.error())
						.with("column '", column.name, "'").with(label(line)));
				}
				conditions.list[r * conditionCount + slot[columnOf[h]]] = *built;
				continue;
			}
			if (column.type == column_type::Command || column.type == column_type::String)
			{
				auto present = detail::RefuseBlank(column, text);
				if (!present)
				{
					return tl::make_unexpected(std::move(present.error()).with(label(line)));
				}
				if (column.type == column_type::Command)
				{
					commands[r * commandCount + slot[columnOf[h]]] = text;
				}
				else
				{
					strings[r * stringCount + slot[columnOf[h]]] = text;
				}
				continue;
			}
			auto value = detail::ReadCell(def, columnOf[h], text, names, line);
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
		conditions.list, strings, command.report});
	if (!loaded)
	{
		return tl::make_unexpected(detail::LocateRefusal(std::move(loaded.error()), refusal, count, command.label,
			[&](size_t row) { return file.line_of_row(row + 1); }));
	}
	return {};
}

}
