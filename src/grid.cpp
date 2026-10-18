#include "tabular/grid.h"

#include "relation_internal.h"

#include <algorithm>
#include <cassert>

namespace tabular
{

namespace
{

bool IsTextGrid(column_type cells)
{
	return cells == column_type::String || cells == column_type::Command;
}

// Sorted (key, position) pairs; on a clash, the two positions.
struct sorted_keys
{
	std::vector<std::pair<int64_t, uint32_t>> keys;
	size_t                                    clash   = SIZE_MAX;
	size_t                                    earlier = SIZE_MAX;
};

sorted_keys SortKeys(std::span<cell const> in)
{
	sorted_keys out;
	out.keys.reserve(in.size());
	for (uint32_t i = 0; i < in.size(); ++i)
	{
		out.keys.push_back({in[i].i, i});
	}
	std::sort(out.keys.begin(), out.keys.end());
	for (size_t i = 1; i < out.keys.size(); ++i)
	{
		if (out.keys[i].first == out.keys[i - 1].first)
		{
			out.clash   = std::max(out.keys[i].second, out.keys[i - 1].second);
			out.earlier = std::min(out.keys[i].second, out.keys[i - 1].second);
			break;
		}
	}
	return out;
}

tl::expected<uint32_t, ErrorContext> Find(std::vector<std::pair<int64_t, uint32_t>> const& keys, cell key,
	column_def const& axis, table_def const& def)
{
	auto found = std::lower_bound(keys.begin(), keys.end(), std::pair<int64_t, uint32_t>{key.i, 0});
	if (found == keys.end() || found->first != key.i)
	{
		return tl::make_unexpected(ErrorContext("grid '", def.name, "' has no ", axis.name, " ", key.i));
	}
	return found->second;
}

}

GridVersion::GridVersion(std::unique_ptr<Data>&& data) : _data(std::move(data)) {}
GridVersion::~GridVersion() = default;

void GridVersion::Release() const
{
	if (_refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
	{
		delete this;
	}
}

table_def const& GridVersion::definition() const { return _data->def; }
uint32_t GridVersion::Rows() const               { return _data->rows; }
uint32_t GridVersion::Columns() const            { return _data->columns; }
uint64_t GridVersion::Owner() const              { return _data->owner; }
uint64_t GridVersion::Generation() const         { return _data->generation; }

tl::expected<uint32_t, ErrorContext> GridVersion::RowOf(cell key) const
{
	return Find(_data->rowKeys, key, _data->def.grid_rows, _data->def);
}

tl::expected<uint32_t, ErrorContext> GridVersion::ColumnOf(cell key) const
{
	return Find(_data->columnKeys, key, _data->def.grid_columns, _data->def);
}

tl::expected<cell, ErrorContext> GridVersion::At(cell row_key, cell column_key) const
{
	if (IsTextGrid(_data->def.grid_cells))
	{
		return tl::make_unexpected(ErrorContext("grid '", _data->def.name, "' holds text; read it with StringAt"));
	}
	auto r = RowOf(row_key);
	if (!r)
	{
		return tl::make_unexpected(std::move(r.error()));
	}
	auto c = ColumnOf(column_key);
	if (!c)
	{
		return tl::make_unexpected(std::move(c.error()));
	}
	return At(*r, *c);
}

tl::expected<counted_string, ErrorContext> GridVersion::StringAt(cell row_key, cell column_key) const
{
	if (!IsTextGrid(_data->def.grid_cells))
	{
		return tl::make_unexpected(ErrorContext("grid '", _data->def.name, "' holds values; read it with At"));
	}
	auto r = RowOf(row_key);
	if (!r)
	{
		return tl::make_unexpected(std::move(r.error()));
	}
	auto c = ColumnOf(column_key);
	if (!c)
	{
		return tl::make_unexpected(std::move(c.error()));
	}
	return StringAt(*r, *c);
}

cell GridVersion::At(uint32_t row, uint32_t column) const
{
	assert(row < _data->rows && column < _data->columns && !IsTextGrid(_data->def.grid_cells));
	return _data->values[size_t(row) * _data->columns + column];
}

counted_string const& GridVersion::StringAt(uint32_t row, uint32_t column) const
{
	assert(row < _data->rows && column < _data->columns && IsTextGrid(_data->def.grid_cells));
	return _data->strings[size_t(row) * _data->columns + column];
}

tl::expected<void, ErrorContext> relation_store::LoadGrid(load_grid const& command)
{
	auto defs = _storage->definition.tables();
	if (command.table >= defs.size())
	{
		return tl::make_unexpected(ErrorContext("load into table ", command.table, ", but the schema has ", defs.size(), " tables"));
	}
	table_def const& def = defs[command.table];
	if (!def.is_grid)
	{
		return tl::make_unexpected(ErrorContext("table '", def.name, "' is not a grid; its rows come from Load or Insert"));
	}
	bool const   text  = IsTextGrid(def.grid_cells);
	size_t const given = text ? command.strings.size() : command.values.size();
	// Divided, not multiplied, as Load checks its spans: the product of two
	// span sizes could wrap.
	size_t const rows    = command.row_keys.size();
	size_t const columns = command.column_keys.size();
	bool const   fits    = rows == 0 || columns == 0 ? given == 0 : given % rows == 0 && given / rows == columns;
	if (!fits || (text ? !command.values.empty() : !command.strings.empty()))
	{
		return tl::make_unexpected(ErrorContext("grid '", def.name, "' of ", rows, " rows and ", columns,
			" columns was given ", given, " cells"));
	}
	if (rows > UINT32_MAX || columns > UINT32_MAX)
	{
		return tl::make_unexpected(ErrorContext("grid '", def.name, "' is too large"));
	}
	for (auto const& [keys, axis] : {std::pair{command.row_keys, &def.grid_rows}, std::pair{command.column_keys, &def.grid_columns}})
	{
		for (cell k : keys)
		{
			auto fits = Check32(*axis, def, k.i);
			if (!fits)
			{
				return tl::make_unexpected(std::move(fits.error()));
			}
		}
	}
	if (def.grid_cells == column_type::Hash || def.grid_cells == column_type::Flags)
	{
		column_def const cells{"cells", def.grid_cells};
		for (cell v : command.values)
		{
			auto fits = Check32(cells, def, v.i);
			if (!fits)
			{
				return tl::make_unexpected(std::move(fits.error()));
			}
		}
	}
	sorted_keys rowKeys = SortKeys(command.row_keys);
	if (rowKeys.clash != SIZE_MAX)
	{
		if (command.refusal)
		{
			command.refusal->row         = rowKeys.clash;
			command.refusal->earlier_row = rowKeys.earlier;
		}
		return tl::make_unexpected(ErrorContext("grid '", def.name, "' has ", def.grid_rows.name, " ",
			command.row_keys[rowKeys.clash].i, " twice"));
	}
	sorted_keys columnKeys = SortKeys(command.column_keys);
	if (columnKeys.clash != SIZE_MAX)
	{
		if (command.refusal)
		{
			command.refusal->column         = columnKeys.clash;
			command.refusal->earlier_column = columnKeys.earlier;
		}
		return tl::make_unexpected(ErrorContext("grid '", def.name, "' has ", def.grid_columns.name, " ",
			command.column_keys[columnKeys.clash].i, " twice"));
	}

	// A grid has one contributor: replacing another owner's would leave its
	// Retract with nothing to remove. A grid holding nothing has none.
	GridVersion::Data const& held = *_storage->grids[command.table]->_data;
	if (held.owner != command.owner && (held.rows != 0 || held.columns != 0))
	{
		return tl::make_unexpected(ErrorContext("grid '", def.name, "' is owned by owner ", held.owner,
			", and owner ", command.owner, " cannot replace it; a grid has one contributor"));
	}

	GridVersion::Data next{def};
	next.owner      = command.owner;
	next.generation = _storage->grids[command.table]->_data->generation + 1;
	next.rows       = static_cast<uint32_t>(rows);
	next.columns    = static_cast<uint32_t>(columns);
	next.rowKeys    = std::move(rowKeys.keys);
	next.columnKeys = std::move(columnKeys.keys);
	if (text)
	{
		next.strings.reserve(given);
		for (string_in s : command.strings)
		{
			next.strings.push_back(counted_string::MakeShared(s.text));
		}
	}
	else
	{
		next.values.assign(command.values.begin(), command.values.end());
	}
	_storage->grids[command.table] = Storage::NewGrid(std::move(next));
	return {};
}

tl::expected<counted_ptr<GridVersion const>, ErrorContext> relation_store::Grid(uint32_t table) const
{
	auto defs = _storage->definition.tables();
	if (table >= defs.size())
	{
		return tl::make_unexpected(ErrorContext("no table ", table));
	}
	if (!defs[table].is_grid)
	{
		return tl::make_unexpected(ErrorContext("table '", defs[table].name, "' is not a grid"));
	}
	return _storage->grids[table];
}

}
