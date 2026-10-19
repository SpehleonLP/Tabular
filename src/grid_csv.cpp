#include "csv_cells.h"
#include "csv_text.h"

#include <string>
#include <vector>

namespace tabular
{

tl::expected<void, ErrorContext> LoadGridCsv(relation_store& store, csv_grid const& command)
{
	auto label = [&](uint32_t line) { return cc(std::string(), command.label, ":", line); };
	auto defs  = store.definition().tables();
	if (command.table >= defs.size())
	{
		return tl::make_unexpected(ErrorContext("no table ", command.table).with(std::string(command.label)));
	}
	table_def const& def = defs[command.table];
	if (!def.is_grid)
	{
		return tl::make_unexpected(ErrorContext("table '", def.name, "' is not a grid").with(std::string(command.label)));
	}

	auto read = detail::csv_text::Read(command.text, command.label, command.first_line);
	if (!read)
	{
		return tl::make_unexpected(std::move(read.error()));
	}
	detail::csv_text const& file = *read;
	if (file.rows() == 0 || file.used_width(0) < 2)
	{
		return tl::make_unexpected(ErrorContext("grid '", def.name, "' needs a header row naming its ",
			def.grid_columns.name, " keys").with(std::string(command.label)));
	}
	// Trailing empty header cells are a spreadsheet's padding.
	size_t const   width      = file.used_width(0);
	uint32_t const headerLine = file.line_of_row(0);

	std::vector<cell> columnKeys;
	detail::hash_names names;
	for (size_t h = 1; h < width; ++h)
	{
		auto key = detail::ReadCell(def.grid_columns, file.cell(h, 0), names, headerLine);
		if (!key)
		{
			return tl::make_unexpected(std::move(key.error()).with(label(headerLine)));
		}
		columnKeys.push_back(*key);
	}

	bool const text_cells = def.grid_cells == column_type::String || def.grid_cells == column_type::Command;
	std::vector<cell>      rowKeys;
	std::vector<cell>      values;
	std::vector<string_in> strings;
	std::vector<uint32_t>  lineOf;
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
		auto key = detail::ReadCell(def.grid_rows, file.cell(0, r), names, line);
		if (!key)
		{
			return tl::make_unexpected(std::move(key.error()).with(label(line)));
		}
		rowKeys.push_back(*key);
		lineOf.push_back(line);
		for (size_t h = 1; h < width; ++h)
		{
			std::string_view const cellText = file.cell(h, r);
			// The header names the column, so an error points at the cell.
			column_def const column{std::string(file.cell(h, 0)), def.grid_cells};
			if (text_cells)
			{
				auto present = detail::RefuseBlank(column, cellText);
				if (!present)
				{
					return tl::make_unexpected(std::move(present.error()).with(label(line)));
				}
				strings.emplace_back(cellText);
				continue;
			}
			auto value = detail::ReadCell(column, cellText, names, line);
			if (!value)
			{
				return tl::make_unexpected(std::move(value.error()).with(label(line)));
			}
			values.push_back(*value);
		}
	}

	grid_refusal refusal;
	auto loaded = store.LoadGrid(load_grid{command.table, command.owner, rowKeys, columnKeys, values, strings, &refusal});
	if (!loaded)
	{
		ErrorContext error = std::move(loaded.error());
		if (refusal.row < lineOf.size())
		{
			return tl::make_unexpected(std::move(error).with("same key as line ", lineOf[refusal.earlier_row])
				.with(label(lineOf[refusal.row])));
		}
		if (refusal.column < columnKeys.size())
		{
			return tl::make_unexpected(std::move(error).with("header '", file.cell(refusal.column + 1, 0),
				"' repeats header ", refusal.earlier_column + 2).with(label(headerLine)));
		}
		return tl::make_unexpected(std::move(error).with(std::string(command.label)));
	}
	return {};
}

}
