#include "tabular/relation.h"

#include "Support/strings.h"

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace tabular
{

namespace
{

char const* TypeName(column_type type)
{
	switch (type)
	{
	case column_type::Hash:      return "hash";
	case column_type::Flags:     return "flag set";
	case column_type::Int:       return "int";
	case column_type::Double:    return "double";
	case column_type::Condition: return "condition";
	case column_type::Command:   return "command";
	}
	return "unknown type";
}

// Keys and orders compare by equality, which only an integer value gives:
// doubles fail on NaN and -0.0, and conditions and commands are not values.
bool ComparesByEquality(column_type type)
{
	return type == column_type::Hash || type == column_type::Flags || type == column_type::Int;
}

tl::expected<immutable_array<column_def>, ErrorContext> BuildColumns(create_table const& c)
{
	if (c.columns.empty())
	{
		return tl::make_unexpected(ErrorContext("table '", c.name, "' has no columns"));
	}
	shared_array<column_def> columns(c.columns.size());
	for (uint32_t i = 0; i < c.columns.size(); ++i)
	{
		add_column const& in = c.columns[i];
		if (in.name.empty())
		{
			return tl::make_unexpected(ErrorContext("column ", i, " of table '", c.name, "' needs a name"));
		}
		for (uint32_t earlier = 0; earlier < i; ++earlier)
		{
			if (columns[earlier].name == in.name)
			{
				return tl::make_unexpected(ErrorContext("table '", c.name, "' has column '", in.name, "' twice"));
			}
		}
		if (in.key && !ComparesByEquality(in.type))
		{
			return tl::make_unexpected(ErrorContext("column '", in.name, "' of table '", c.name, "' is a ",
				TypeName(in.type), " and cannot be a key"));
		}
		columns[i] = column_def{std::string(in.name), in.type, in.key};
	}
	return immutable_array<column_def>(std::move(columns));
}

tl::expected<immutable_array<uint32_t>, ErrorContext> BuildOrder(create_table const& c,
	immutable_array<column_def> const& columns)
{
	order_by const& order = *c.ordering;
	if (order.columns.empty())
	{
		return tl::make_unexpected(ErrorContext("the order of table '", c.name, "' names no columns"));
	}
	for (size_t k = 0; k < order.columns.size(); ++k)
	{
		uint32_t column = order.columns[k];
		if (column >= columns.size())
		{
			return tl::make_unexpected(ErrorContext("the order of table '", c.name, "' names column ", column,
				", but the table has ", columns.size()));
		}
		for (size_t earlier = 0; earlier < k; ++earlier)
		{
			if (order.columns[earlier] == column)
			{
				return tl::make_unexpected(ErrorContext("the order of table '", c.name, "' names column '",
					columns[column].name, "' twice"));
			}
		}
		if (!ComparesByEquality(columns[column].type))
		{
			return tl::make_unexpected(ErrorContext("column '", columns[column].name, "' of table '", c.name,
				"' is a ", TypeName(columns[column].type), " and cannot be in an order"));
		}
	}
	return immutable_array<uint32_t>::FromArray(order.columns.data(), order.columns.size());
}

tl::expected<table_def, ErrorContext> BuildTable(create_table const& c, std::span<table_def const> earlier)
{
	if (c.name.empty())
	{
		return tl::make_unexpected(ErrorContext("a table needs a name"));
	}
	for (table_def const& t : earlier)
	{
		if (t.name == c.name)
		{
			return tl::make_unexpected(ErrorContext("table '", c.name, "' is defined twice"));
		}
	}

	auto columns = BuildColumns(c);
	if (!columns)
	{
		return tl::make_unexpected(std::move(columns.error()));
	}

	table_def out{std::string(c.name), std::move(*columns), {}, false};
	if (c.ordering)
	{
		auto order = BuildOrder(c, out.columns);
		if (!order)
		{
			return tl::make_unexpected(std::move(order.error()));
		}
		out.order  = std::move(*order);
		out.unique = c.ordering->unique;
	}
	return out;
}

}

tl::expected<uint32_t, ErrorContext> schema::FindTable(std::string_view name) const
{
	for (uint32_t i = 0; i < _tables.size(); ++i)
	{
		if (_tables[i].name == name)
		{
			return i;
		}
	}
	return tl::make_unexpected(ErrorContext("no table named '", name, "'"));
}

tl::expected<uint32_t, ErrorContext> schema::FindColumn(uint32_t table, std::string_view name) const
{
	if (table >= _tables.size())
	{
		return tl::make_unexpected(ErrorContext("no table ", table));
	}
	auto const& columns = _tables[table].columns;
	for (uint32_t i = 0; i < columns.size(); ++i)
	{
		if (columns[i].name == name)
		{
			return i;
		}
	}
	return tl::make_unexpected(ErrorContext("table '", _tables[table].name, "' has no column '", name, "'"));
}

tl::expected<schema, ErrorContext> BuildSchema(std::span<create_table const> tables)
{
	shared_array<table_def> out(tables.size());
	for (uint32_t t = 0; t < tables.size(); ++t)
	{
		auto built = BuildTable(tables[t], std::span<table_def const>(out.begin(), t));
		if (!built)
		{
			return tl::make_unexpected(ErrorContext("table ", t, ": ", built.error().message));
		}
		out[t] = std::move(*built);
	}
	return schema(immutable_array<table_def>(std::move(out)));
}

}

namespace tabular
{

namespace
{

bool IsValueColumn(column_type type)
{
	return type != column_type::Condition && type != column_type::Command;
}

// A flag set is AngelScript uint32; anything outside it could not round-trip.
bool FitsFlags(int64_t v)
{
	return v >= 0 && v <= INT64_C(0xFFFFFFFF);
}

void Release(closure const& c)
{
	if (c.release)
	{
		c.release(c.data);
	}
}

void ReleaseAll(std::span<closure const> closures)
{
	for (closure const& c : closures)
	{
		Release(c);
	}
}

// Negative, zero or positive as the row sorts before, with or after key, over
// the first key.size() order slots. Order columns are integers, so every
// compared cell is one.
int CompareOrder(std::span<cell const> row, std::span<uint32_t const> orderSlots, std::span<cell const> key)
{
	for (size_t k = 0; k < key.size(); ++k)
	{
		int64_t a = row[orderSlots[k]].i;
		int64_t b = key[k].i;
		if (a != b)
		{
			return a < b ? -1 : 1;
		}
	}
	return 0;
}

// The first row that does not sort before key (lower), or the first that
// sorts after it (upper), by binary search over rows already in order.
size_t OrderBound(std::span<cell const> cells, size_t rowCount, size_t stride,
	std::span<uint32_t const> orderSlots, std::span<cell const> key, bool upper)
{
	size_t first = 0;
	size_t count = rowCount;
	while (count > 0)
	{
		size_t half = count / 2;
		size_t mid  = first + half;
		int    cmp  = CompareOrder(cells.subspan(mid * stride, stride), orderSlots, key);
		if (upper ? cmp <= 0 : cmp < 0)
		{
			first  = mid + 1;
			count -= half + 1;
		}
		else
		{
			count = half;
		}
	}
	return first;
}

}

struct relation_store::Storage
{
	// Each row is three parallel slices, one per kind of column, so a row
	// moves as one when the table is sorted or compacted.
	struct table_rows
	{
		std::vector<cell>        cells;      // value columns, row-major
		std::vector<closure>     closures;   // Condition columns, row-major
		std::vector<std::string> commands;   // Command columns, row-major
		std::vector<uint64_t>    owners;     // one per row
		uint64_t                 generation = 0;

		uint32_t              valueCount     = 0;
		uint32_t              conditionCount = 0;
		uint32_t              commandCount   = 0;
		std::vector<uint32_t> slot;          // per column: its position among columns of its kind
		std::vector<uint32_t> orderSlots;    // the order's columns, as value slots

		// Per column, value -> rows in row order; filled for key columns only.
		std::vector<std::unordered_map<int64_t, std::vector<uint32_t>>> index;

		void RebuildIndex(table_def const& def)
		{
			size_t rowCount = owners.size();
			for (uint32_t c = 0; c < def.columns.size(); ++c)
			{
				index[c].clear();
				if (!def.columns[c].key)
				{
					continue;
				}
				for (size_t r = 0; r < rowCount; ++r)
				{
					index[c][cells[r * valueCount + slot[c]].i].push_back(static_cast<uint32_t>(r));
				}
			}
		}
	};

	schema                  definition;
	std::vector<table_rows> tables;

	~Storage()
	{
		for (table_rows const& rows : tables)
		{
			ReleaseAll(rows.closures);
		}
	}
};

relation_store::relation_store(schema&& definition)
	: _storage(std::make_unique<Storage>())
{
	_storage->definition = std::move(definition);
	auto defs = _storage->definition.tables();
	_storage->tables.resize(defs.size());
	for (size_t t = 0; t < defs.size(); ++t)
	{
		auto&            rows = _storage->tables[t];
		table_def const& def  = defs[t];
		rows.slot.resize(def.columns.size());
		rows.index.resize(def.columns.size());
		for (uint32_t c = 0; c < def.columns.size(); ++c)
		{
			switch (def.columns[c].type)
			{
			case column_type::Condition: rows.slot[c] = rows.conditionCount++; break;
			case column_type::Command:   rows.slot[c] = rows.commandCount++;   break;
			default:                     rows.slot[c] = rows.valueCount++;     break;
			}
		}
		for (uint32_t k = 0; k < def.order.size(); ++k)
		{
			rows.orderSlots.push_back(rows.slot[def.order[k]]);
		}
	}
}

relation_store::~relation_store() = default;
relation_store::relation_store(relation_store&&) noexcept = default;
relation_store& relation_store::operator=(relation_store&&) noexcept = default;

schema const& relation_store::definition() const
{
	return _storage->definition;
}

size_t relation_store::RowCount(uint32_t table) const
{
	return _storage->tables[table].owners.size();
}

uint64_t relation_store::OwnerOf(uint32_t table, size_t row) const
{
	return _storage->tables[table].owners[row];
}

uint64_t relation_store::Generation(uint32_t table) const
{
	return _storage->tables[table].generation;
}

tl::expected<void, ErrorContext> relation_store::Insert(insert_row const& command)
{
	// The row's closures are ours from here on, so every refusal releases them.
	auto refuse = [&](ErrorContext&& error) -> tl::expected<void, ErrorContext>
	{
		ReleaseAll(command.conditions);
		return tl::make_unexpected(std::move(error));
	};

	auto defs = _storage->definition.tables();
	if (command.table >= defs.size())
	{
		return refuse(ErrorContext("insert into table ", command.table, ", but the schema has ", defs.size(), " tables"));
	}
	table_def const& def  = defs[command.table];
	auto&            rows = _storage->tables[command.table];
	if (command.values.size() != rows.valueCount)
	{
		return refuse(ErrorContext("insert into '", def.name, "' has ", command.values.size(),
			" values, but the table has ", rows.valueCount, " value columns"));
	}
	if (command.conditions.size() != rows.conditionCount)
	{
		return refuse(ErrorContext("insert into '", def.name, "' has ", command.conditions.size(),
			" conditions, but the table has ", rows.conditionCount, " condition columns"));
	}
	if (command.commands.size() != rows.commandCount)
	{
		return refuse(ErrorContext("insert into '", def.name, "' has ", command.commands.size(),
			" commands, but the table has ", rows.commandCount, " command columns"));
	}
	for (uint32_t c = 0; c < def.columns.size(); ++c)
	{
		column_def const& column = def.columns[c];
		if (column.type == column_type::Flags && !FitsFlags(command.values[rows.slot[c]].i))
		{
			return refuse(ErrorContext("column '", column.name, "' of '", def.name,
				"' is a flag set and holds a uint32; got ", command.values[rows.slot[c]].i));
		}
		if (column.type == column_type::Condition && command.conditions[rows.slot[c]].function == nullptr)
		{
			return refuse(ErrorContext("column '", column.name, "' of '", def.name, "' got a condition with no function"));
		}
	}

	size_t count = rows.owners.size();
	size_t pos   = count;
	if (!rows.orderSlots.empty())
	{
		std::vector<cell> key;
		key.reserve(rows.orderSlots.size());
		for (uint32_t s : rows.orderSlots)
		{
			key.push_back(command.values[s]);
		}
		size_t lower = OrderBound(rows.cells, count, rows.valueCount, rows.orderSlots, key, false);
		pos          = OrderBound(rows.cells, count, rows.valueCount, rows.orderSlots, key, true);
		if (def.unique && lower != pos)
		{
			return refuse(ErrorContext("table '", def.name, "' has a unique order, and owner ",
				rows.owners[lower], " already has a row with this key"));
		}
	}

	rows.cells.insert(rows.cells.begin() + pos * rows.valueCount, command.values.begin(), command.values.end());
	rows.closures.insert(rows.closures.begin() + pos * rows.conditionCount,
		command.conditions.begin(), command.conditions.end());
	rows.commands.insert(rows.commands.begin() + pos * rows.commandCount,
		command.commands.begin(), command.commands.end());
	rows.owners.insert(rows.owners.begin() + pos, command.owner);

	auto row = static_cast<uint32_t>(pos);
	for (uint32_t c = 0; c < def.columns.size(); ++c)
	{
		if (!def.columns[c].key)
		{
			continue;
		}
		// Rows after the insertion point moved down one.
		for (auto& entry : rows.index[c])
		{
			for (uint32_t& r : entry.second)
			{
				if (r >= row)
				{
					++r;
				}
			}
		}
		// Each list stays in row order, which is the order Find returns.
		auto& list = rows.index[c][command.values[rows.slot[c]].i];
		list.insert(std::lower_bound(list.begin(), list.end(), row), row);
	}
	++rows.generation;
	return {};
}

void relation_store::Retract(retract_owner const& command)
{
	auto defs = _storage->definition.tables();
	for (size_t t = 0; t < defs.size(); ++t)
	{
		auto&  rows = _storage->tables[t];
		size_t kept = 0;
		for (size_t r = 0; r < rows.owners.size(); ++r)
		{
			if (rows.owners[r] == command.owner)
			{
				ReleaseAll(std::span<closure const>(rows.closures).subspan(r * rows.conditionCount, rows.conditionCount));
				continue;
			}
			if (kept != r)
			{
				std::copy_n(rows.cells.begin() + r * rows.valueCount, rows.valueCount,
					rows.cells.begin() + kept * rows.valueCount);
				std::copy_n(rows.closures.begin() + r * rows.conditionCount, rows.conditionCount,
					rows.closures.begin() + kept * rows.conditionCount);
				std::move(rows.commands.begin() + r * rows.commandCount,
					rows.commands.begin() + (r + 1) * rows.commandCount,
					rows.commands.begin() + kept * rows.commandCount);
				rows.owners[kept] = rows.owners[r];
			}
			++kept;
		}
		if (kept == rows.owners.size())
		{
			// None of this owner's rows are here: nothing changed, so the
			// generation must not either.
			continue;
		}
		rows.owners.resize(kept);
		rows.cells.resize(kept * rows.valueCount);
		rows.closures.resize(kept * rows.conditionCount);
		rows.commands.resize(kept * rows.commandCount);
		// A retract happens on reload, not per frame, so rebuilding the key
		// indexes is cheaper to get right than patching shifted row numbers.
		rows.RebuildIndex(defs[t]);
		++rows.generation;
	}
}

namespace
{

// Checks a (table, row, column) address and the column's kind, so each reader
// can refuse a bad address with the same message.
tl::expected<column_def const*, ErrorContext> CheckAddress(schema const& definition, size_t rowCount,
	uint32_t table, size_t row, uint32_t column, column_type const* wanted, char const* wantedName)
{
	auto defs = definition.tables();
	if (table >= defs.size())
	{
		return tl::make_unexpected(ErrorContext("no table ", table));
	}
	table_def const& def = defs[table];
	if (row >= rowCount)
	{
		return tl::make_unexpected(ErrorContext("table '", def.name, "' has no row ", row));
	}
	if (column >= def.columns.size())
	{
		return tl::make_unexpected(ErrorContext("table '", def.name, "' has no column ", column));
	}
	column_def const& c = def.columns[column];
	bool matches = wanted ? c.type == *wanted : IsValueColumn(c.type);
	if (!matches)
	{
		return tl::make_unexpected(ErrorContext("column '", c.name, "' of '", def.name, "' is not a ", wantedName));
	}
	return &c;
}

}

tl::expected<cell, ErrorContext> relation_store::Value(uint32_t table, size_t row, uint32_t column) const
{
	size_t rowCount = table < _storage->tables.size() ? _storage->tables[table].owners.size() : 0;
	auto   checked  = CheckAddress(_storage->definition, rowCount, table, row, column, nullptr, "value column");
	if (!checked)
	{
		return tl::make_unexpected(std::move(checked.error()));
	}
	auto const& rows = _storage->tables[table];
	return rows.cells[row * rows.valueCount + rows.slot[column]];
}

tl::expected<bool, ErrorContext> relation_store::Holds(uint32_t table, size_t row, uint32_t column, void const* this_in) const
{
	column_type const wanted   = column_type::Condition;
	size_t            rowCount = table < _storage->tables.size() ? _storage->tables[table].owners.size() : 0;
	auto checked = CheckAddress(_storage->definition, rowCount, table, row, column, &wanted, "condition");
	if (!checked)
	{
		return tl::make_unexpected(std::move(checked.error()));
	}
	auto const&    rows  = _storage->tables[table];
	closure const& c     = rows.closures[row * rows.conditionCount + rows.slot[column]];
	ErrorContext   error;
	bool           holds = c.function(c.data, this_in, &error);
	if (!error.message.empty())
	{
		return tl::make_unexpected(std::move(error));
	}
	return holds;
}

tl::expected<std::string_view, ErrorContext> relation_store::CommandAt(uint32_t table, size_t row, uint32_t column) const
{
	column_type const wanted   = column_type::Command;
	size_t            rowCount = table < _storage->tables.size() ? _storage->tables[table].owners.size() : 0;
	auto checked = CheckAddress(_storage->definition, rowCount, table, row, column, &wanted, "command");
	if (!checked)
	{
		return tl::make_unexpected(std::move(checked.error()));
	}
	auto const& rows = _storage->tables[table];
	return std::string_view(rows.commands[row * rows.commandCount + rows.slot[column]]);
}

tl::expected<std::vector<size_t>, ErrorContext> relation_store::Find(uint32_t, uint32_t, cell) const
{
	return tl::make_unexpected(ErrorContext("relation_store::Find is not implemented yet"));
}

tl::expected<row_range, ErrorContext> relation_store::FindOrdered(uint32_t, std::span<cell const>) const
{
	return tl::make_unexpected(ErrorContext("relation_store::FindOrdered is not implemented yet"));
}

}
