#include "tabular/relation.h"

#include "Support/strings.h"

#include <algorithm>
#include <atomic>
#include <iterator>
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
	case column_type::Vec2:      return "vec2";
	case column_type::String:    return "string";
	}
	return "unknown type";
}

// Keys and orders compare by equality, which only an integer value gives:
// doubles fail on NaN and -0.0, and conditions and commands are not values.
bool ComparesByEquality(column_type type)
{
	return type == column_type::Hash || type == column_type::Flags || type == column_type::Int;
}

// A key needs equality only. Interned strings have it, so a string can be a
// key; it has no order worth sorting by, so it cannot be in one.
bool CanBeKey(column_type type)
{
	return ComparesByEquality(type) || type == column_type::String;
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
		if (in.key && !CanBeKey(in.type))
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

	table_def out{std::string(c.name), std::move(*columns), {}, false, c.read_only};
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
	return type != column_type::Condition && type != column_type::Command && type != column_type::String;
}

// A flag set (AngelScript uint32) and a hash (CityHash32) are 32-bit values
// carried as int64_t, widened without sign. Anything outside [0, 2^32) is
// either not one of them or a sign-extended copy, which would be a different
// key that never matches the zero-extended one.
bool Is32BitColumn(column_type type)
{
	return type == column_type::Hash || type == column_type::Flags;
}

bool Fits32(int64_t v)
{
	return v >= 0 && v <= INT64_C(0xFFFFFFFF);
}

tl::expected<void, ErrorContext> Check32(column_def const& column, table_def const& def, int64_t v)
{
	if (Is32BitColumn(column.type) && !Fits32(v))
	{
		return tl::make_unexpected(ErrorContext("column '", column.name, "' of '", def.name, "' is a ",
			column.type == column_type::Hash ? "hash" : "flag set",
			" and holds a 32-bit value widened without sign; got ", v));
	}
	return {};
}

void ReleaseClosure(closure const& c)
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
		ReleaseClosure(c);
	}
}

// A condition shared by every holder of its row. A survivor of a reload sits
// in the old version and the new one, and release must still run exactly
// once: when the last of them lets go.
class HeldCondition
{
public:
	explicit HeldCondition(closure const& c) : _closure(c) {}
	~HeldCondition() { ReleaseClosure(_closure); }
	HeldCondition(HeldCondition const&)            = delete;
	HeldCondition& operator=(HeldCondition const&) = delete;

	closure const& get() const { return _closure; }

	void AddRef() const { _refs.fetch_add(1, std::memory_order_relaxed); }
	void Release() const
	{
		if (_refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
		{
			delete this;
		}
	}

private:
	closure                  _closure;
	mutable std::atomic<int> _refs{1};
};

using held_condition = counted_ptr<HeldCondition const>;

held_condition Hold(closure const& c)
{
	return UncountedWrap(static_cast<HeldCondition const*>(new HeldCondition(c)));
}

counted_string Intern(string_in s)
{
	return counted_string::MakeShared(s.text);
}

// Rows at or after row moved down one.
template<typename Map>
void ShiftRows(Map& index, uint32_t row)
{
	for (auto& entry : index)
	{
		for (uint32_t& r : entry.second)
		{
			if (r >= row)
			{
				++r;
			}
		}
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

// A table's rows: three parallel slices, one per kind of column, so a row
// moves as one when the table is sorted or compacted. A writable table's live
// in the store; a read-only table's live in its current TableVersion.
struct table_rows
{
	std::vector<cell>        cells;      // value columns, row-major
	std::vector<held_condition> conditions; // Condition columns, row-major
	std::vector<std::string> commands;   // Command columns, row-major
	std::vector<counted_string> strings; // String columns, row-major, interned
	std::vector<uint64_t>    owners;     // one per row
	uint64_t                 generation = 0;

	uint32_t              valueCount     = 0;
	uint32_t              conditionCount = 0;
	uint32_t              commandCount   = 0;
	uint32_t              stringCount    = 0;
	std::vector<uint32_t> slot;          // per column: its position among columns of its kind
	std::vector<uint32_t> orderSlots;    // the order's columns, as value slots

	// Per column, value -> rows in row order; filled for key columns only.
	std::vector<std::unordered_map<int64_t, std::vector<uint32_t>>> index;
	// The same for String key columns, keyed by text. The views point into the
	// handles in strings, whose bytes do not move.
	std::vector<std::unordered_map<std::string_view, std::vector<uint32_t>>> stringIndex;

	// No rows, laid out for def.
	explicit table_rows(table_def const& def)
	{
		slot.resize(def.columns.size());
		index.resize(def.columns.size());
		stringIndex.resize(def.columns.size());
		for (uint32_t c = 0; c < def.columns.size(); ++c)
		{
			switch (def.columns[c].type)
			{
			case column_type::Condition: slot[c] = conditionCount++; break;
			case column_type::Command:   slot[c] = commandCount++;   break;
			case column_type::String:    slot[c] = stringCount++;    break;
			default:                     slot[c] = valueCount++;     break;
			}
		}
		for (uint32_t k = 0; k < def.order.size(); ++k)
		{
			orderSlots.push_back(slot[def.order[k]]);
		}
	}

	size_t RowCount() const { return owners.size(); }

	void RebuildIndex(table_def const& def)
	{
		size_t rowCount = owners.size();
		for (uint32_t c = 0; c < def.columns.size(); ++c)
		{
			index[c].clear();
			stringIndex[c].clear();
			if (!def.columns[c].key)
			{
				continue;
			}
			for (size_t r = 0; r < rowCount; ++r)
			{
				if (def.columns[c].type == column_type::String)
				{
					stringIndex[c][std::string_view(strings[r * stringCount + slot[c]])].push_back(static_cast<uint32_t>(r));
				}
				else
				{
					index[c][cells[r * valueCount + slot[c]].i].push_back(static_cast<uint32_t>(r));
				}
			}
		}
	}
};

struct TableVersion::Data
{
	table_def  def;
	table_rows rows;
};

TableVersion::TableVersion(std::unique_ptr<Data>&& data)
	: _data(std::move(data))
{
}

TableVersion::~TableVersion() = default;

void TableVersion::Release() const
{
	if (_refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
	{
		delete this;
	}
}

struct relation_store::Storage
{
	schema                  definition;
	std::vector<table_rows> tables;     // a read-only table's stays empty
	std::vector<counted_ptr<TableVersion const>> versions;   // null for a writable table

	// TableVersion's constructor and data are private to the store.
	static counted_ptr<TableVersion const> NewVersion(table_def const& def, table_rows&& rows)
	{
		auto data = std::make_unique<TableVersion::Data>(TableVersion::Data{def, std::move(rows)});
		return UncountedWrap(static_cast<TableVersion const*>(new TableVersion(std::move(data))));
	}

	table_rows const& RowsOf(uint32_t table) const
	{
		return versions[table] ? versions[table]->_data->rows : tables[table];
	}
};

relation_store::relation_store(schema&& definition)
	: _storage(std::make_unique<Storage>())
{
	_storage->definition = std::move(definition);
	auto defs = _storage->definition.tables();
	_storage->tables.reserve(defs.size());
	_storage->versions.resize(defs.size());
	for (size_t t = 0; t < defs.size(); ++t)
	{
		_storage->tables.emplace_back(defs[t]);
	}
	for (size_t t = 0; t < defs.size(); ++t)
	{
		if (defs[t].read_only)
		{
			_storage->versions[t] = Storage::NewVersion(defs[t], table_rows(defs[t]));
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
	return _storage->RowsOf(table).RowCount();
}

uint64_t relation_store::OwnerOf(uint32_t table, size_t row) const
{
	return _storage->RowsOf(table).owners[row];
}

uint64_t relation_store::Generation(uint32_t table) const
{
	return _storage->RowsOf(table).generation;
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
	if (def.read_only)
	{
		return refuse(ErrorContext("table '", def.name, "' is read-only; its rows come only from Load"));
	}
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
	if (command.strings.size() != rows.stringCount)
	{
		return refuse(ErrorContext("insert into '", def.name, "' has ", command.strings.size(),
			" strings, but the table has ", rows.stringCount, " string columns"));
	}
	for (uint32_t c = 0; c < def.columns.size(); ++c)
	{
		column_def const& column = def.columns[c];
		if (IsValueColumn(column.type))
		{
			auto fits = Check32(column, def, command.values[rows.slot[c]].i);
			if (!fits)
			{
				return refuse(std::move(fits.error()));
			}
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
	std::vector<held_condition> held;
	held.reserve(command.conditions.size());
	for (closure const& c : command.conditions)
	{
		held.push_back(Hold(c));
	}
	rows.conditions.insert(rows.conditions.begin() + pos * rows.conditionCount,
		std::make_move_iterator(held.begin()), std::make_move_iterator(held.end()));
	rows.commands.insert(rows.commands.begin() + pos * rows.commandCount,
		command.commands.begin(), command.commands.end());
	std::vector<counted_string> interned;
	interned.reserve(command.strings.size());
	for (string_in s : command.strings)
	{
		interned.push_back(Intern(s));
	}
	rows.strings.insert(rows.strings.begin() + pos * rows.stringCount,
		std::make_move_iterator(interned.begin()), std::make_move_iterator(interned.end()));
	rows.owners.insert(rows.owners.begin() + pos, command.owner);

	auto row = static_cast<uint32_t>(pos);
	for (uint32_t c = 0; c < def.columns.size(); ++c)
	{
		if (!def.columns[c].key)
		{
			continue;
		}
		// Rows after the insertion point moved down one. An append moves
		// nothing, and is the common case: every unordered insert.
		if (pos != count)
		{
			ShiftRows(rows.index[c], row);
			ShiftRows(rows.stringIndex[c], row);
		}
		// Each list stays in row order, which is the order Find returns.
		auto& list = def.columns[c].type == column_type::String
			? rows.stringIndex[c][std::string_view(rows.strings[pos * rows.stringCount + rows.slot[c]])]
			: rows.index[c][command.values[rows.slot[c]].i];
		list.insert(std::lower_bound(list.begin(), list.end(), row), row);
	}
	++rows.generation;
	return {};
}

void relation_store::Retract(retract_owner const& command)
{
	auto defs = _storage->definition.tables();
	for (uint32_t t = 0; t < defs.size(); ++t)
	{
		if (_storage->versions[t])
		{
			// The version without the owner's rows, put in place like any load.
			// Removing rows cannot clash or carry a bad cell, so this load is
			// never refused, and there is nothing to report.
			auto removed = Load(load_rows{t, command.owner, 0, {}, {}});
			(void)removed;
			continue;
		}
		auto&  rows = _storage->tables[t];
		size_t kept = 0;
		for (size_t r = 0; r < rows.owners.size(); ++r)
		{
			if (rows.owners[r] == command.owner)
			{
				continue;
			}
			if (kept != r)
			{
				std::copy_n(rows.cells.begin() + r * rows.valueCount, rows.valueCount,
					rows.cells.begin() + kept * rows.valueCount);
				std::move(rows.conditions.begin() + r * rows.conditionCount,
					rows.conditions.begin() + (r + 1) * rows.conditionCount,
					rows.conditions.begin() + kept * rows.conditionCount);
				std::move(rows.commands.begin() + r * rows.commandCount,
					rows.commands.begin() + (r + 1) * rows.commandCount,
					rows.commands.begin() + kept * rows.commandCount);
				std::move(rows.strings.begin() + r * rows.stringCount,
					rows.strings.begin() + (r + 1) * rows.stringCount,
					rows.strings.begin() + kept * rows.stringCount);
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
		rows.conditions.resize(kept * rows.conditionCount);
		rows.commands.resize(kept * rows.commandCount);
		rows.strings.resize(kept * rows.stringCount);
		// A retract happens on reload, not per frame, so rebuilding the key
		// indexes is cheaper to get right than patching shifted row numbers.
		rows.RebuildIndex(defs[t]);
		++rows.generation;
	}
}

tl::expected<void, ErrorContext> relation_store::Load(load_rows const& command)
{
	// Every closure the load carries is ours from here on: a refusal releases
	// them when held goes out of scope, and a success moves them into rows.
	std::vector<held_condition> held;
	held.reserve(command.conditions.size());
	for (closure const& c : command.conditions)
	{
		held.push_back(Hold(c));
	}

	auto defs = _storage->definition.tables();
	if (command.table >= defs.size())
	{
		return tl::make_unexpected(ErrorContext("load into table ", command.table, ", but the schema has ", defs.size(), " tables"));
	}
	table_def const& def = defs[command.table];
	if (!def.read_only)
	{
		return tl::make_unexpected(ErrorContext("table '", def.name, "' is not read-only; its rows come from Insert"));
	}
	table_rows const& current = _storage->versions[command.table]->_data->rows;
	// Divided, not multiplied: a count large enough to wrap the product would
	// pass a size check against empty spans and read past them.
	auto spans = [&](size_t size, uint32_t width)
	{
		return width == 0 ? size == 0 : size % width == 0 && size / width == command.count;
	};
	if (!spans(command.values.size(), current.valueCount))
	{
		return tl::make_unexpected(ErrorContext("load into '", def.name, "' of ", command.count, " rows has ",
			command.values.size(), " values, but the table has ", current.valueCount, " value columns"));
	}
	if (!spans(command.commands.size(), current.commandCount))
	{
		return tl::make_unexpected(ErrorContext("load into '", def.name, "' of ", command.count, " rows has ",
			command.commands.size(), " commands, but the table has ", current.commandCount, " command columns"));
	}
	if (!spans(command.conditions.size(), current.conditionCount))
	{
		return tl::make_unexpected(ErrorContext("load into '", def.name, "' of ", command.count, " rows has ",
			command.conditions.size(), " conditions, but the table has ", current.conditionCount, " condition columns"));
	}
	if (!spans(command.strings.size(), current.stringCount))
	{
		return tl::make_unexpected(ErrorContext("load into '", def.name, "' of ", command.count, " rows has ",
			command.strings.size(), " strings, but the table has ", current.stringCount, " string columns"));
	}
	for (size_t i = 0; i < command.conditions.size(); ++i)
	{
		if (command.conditions[i].function == nullptr)
		{
			size_t const r = i / current.conditionCount;
			if (command.refusal)
			{
				command.refusal->row = r;
			}
			return tl::make_unexpected(ErrorContext("load into '", def.name, "' got a condition with no function")
				.with("row ", r));
		}
	}
	for (size_t r = 0; r < command.count; ++r)
	{
		for (uint32_t c = 0; c < def.columns.size(); ++c)
		{
			if (IsValueColumn(def.columns[c].type))
			{
				auto fits = Check32(def.columns[c], def, command.values[r * current.valueCount + current.slot[c]].i);
				if (!fits)
				{
					if (command.refusal)
					{
						command.refusal->row = r;
					}
					return tl::make_unexpected(std::move(fits.error()).with("row ", r));
				}
			}
		}
	}

	// Every row the next version holds, as (from the load?, row): survivors
	// first, so a survivor sorts before a loaded row with the same key, as an
	// earlier insert would.
	struct source
	{
		bool   loaded;
		size_t row;
	};
	std::vector<source> order;
	order.reserve(current.RowCount() + command.count);
	for (size_t r = 0; r < current.RowCount(); ++r)
	{
		if (current.owners[r] != command.owner)
		{
			order.push_back({false, r});
		}
	}
	if (order.size() == current.RowCount() && command.count == 0)
	{
		return {};   // nothing of this owner's here and nothing new: no change
	}
	for (size_t r = 0; r < command.count; ++r)
	{
		order.push_back({true, r});
	}
	auto cellsOf = [&](source s) -> std::span<cell const>
	{
		return s.loaded ? command.values.subspan(s.row * current.valueCount, current.valueCount)
		                : std::span<cell const>(current.cells).subspan(s.row * current.valueCount, current.valueCount);
	};
	auto ownerOf = [&](source s) { return s.loaded ? command.owner : current.owners[s.row]; };
	if (!current.orderSlots.empty())
	{
		auto keyCompare = [&](source a, source b) -> int
		{
			std::span<cell const> ac = cellsOf(a);
			std::span<cell const> bc = cellsOf(b);
			for (uint32_t s : current.orderSlots)
			{
				if (ac[s].i != bc[s].i)
				{
					return ac[s].i < bc[s].i ? -1 : 1;
				}
			}
			return 0;
		};
		std::stable_sort(order.begin(), order.end(), [&](source a, source b) { return keyCompare(a, b) < 0; });
		if (def.unique)
		{
			for (size_t i = 1; i < order.size(); ++i)
			{
				if (keyCompare(order[i - 1], order[i]) == 0)
				{
					// Survivors are unique among themselves and sort first, so the
					// later of a clashing pair is always a loaded row.
					source const earlier = order[i - 1];
					source const later   = order[i];
					if (command.refusal)
					{
						command.refusal->row     = later.row;
						command.refusal->earlier = earlier.loaded ? earlier.row : SIZE_MAX;
					}
					std::string key;
					for (uint32_t s : current.orderSlots)
					{
						key = cc(key, key.empty() ? "" : ", ", cellsOf(later)[s].i);
					}
					return tl::make_unexpected(ErrorContext("table '", def.name, "' has a unique order, and owner ",
						ownerOf(earlier), " already has a row with key (", key, ") of loaded row ", later.row));
				}
			}
		}
	}

	// The current version may be held, so survivors are copied, never moved.
	table_rows next(def);
	next.cells.reserve(order.size() * next.valueCount);
	next.commands.reserve(order.size() * next.commandCount);
	next.conditions.reserve(order.size() * next.conditionCount);
	next.strings.reserve(order.size() * next.stringCount);
	next.owners.reserve(order.size());
	for (source s : order)
	{
		std::span<cell const> from = cellsOf(s);
		next.cells.insert(next.cells.end(), from.begin(), from.end());
		for (uint32_t k = 0; k < next.conditionCount; ++k)
		{
			next.conditions.push_back(s.loaded ? held[s.row * next.conditionCount + k]
			                                   : current.conditions[s.row * next.conditionCount + k]);
		}
		for (uint32_t k = 0; k < next.commandCount; ++k)
		{
			if (s.loaded)
			{
				next.commands.emplace_back(command.commands[s.row * next.commandCount + k]);
			}
			else
			{
				next.commands.push_back(current.commands[s.row * next.commandCount + k]);
			}
		}
		for (uint32_t k = 0; k < next.stringCount; ++k)
		{
			next.strings.push_back(s.loaded ? Intern(command.strings[s.row * next.stringCount + k])
			                                : current.strings[s.row * next.stringCount + k]);
		}
		next.owners.push_back(ownerOf(s));
	}
	next.RebuildIndex(def);
	next.generation = current.generation + 1;
	_storage->versions[command.table] = Storage::NewVersion(def, std::move(next));
	return {};
}

tl::expected<counted_ptr<TableVersion const>, ErrorContext> relation_store::Version(uint32_t table) const
{
	auto defs = _storage->definition.tables();
	if (table >= defs.size())
	{
		return tl::make_unexpected(ErrorContext("no table ", table));
	}
	if (!_storage->versions[table])
	{
		return tl::make_unexpected(ErrorContext("table '", defs[table].name, "' is not read-only, so it has no version"));
	}
	return _storage->versions[table];
}

namespace
{

// Checks a (row, column) address and the column's kind, so each reader can
// refuse a bad address with the same message.
tl::expected<column_def const*, ErrorContext> CheckAddress(table_def const& def, size_t rowCount,
	size_t row, uint32_t column, column_type const* wanted, char const* wantedName)
{
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

tl::expected<cell, ErrorContext> ReadValue(table_def const& def, table_rows const& rows, size_t row, uint32_t column)
{
	auto checked = CheckAddress(def, rows.RowCount(), row, column, nullptr, "value column");
	if (!checked)
	{
		return tl::make_unexpected(std::move(checked.error()));
	}
	return rows.cells[row * rows.valueCount + rows.slot[column]];
}

tl::expected<std::string_view, ErrorContext> ReadCommand(table_def const& def, table_rows const& rows, size_t row, uint32_t column)
{
	column_type const wanted  = column_type::Command;
	auto              checked = CheckAddress(def, rows.RowCount(), row, column, &wanted, "command");
	if (!checked)
	{
		return tl::make_unexpected(std::move(checked.error()));
	}
	return std::string_view(rows.commands[row * rows.commandCount + rows.slot[column]]);
}

tl::expected<bool, ErrorContext> ReadHolds(table_def const& def, table_rows const& rows, size_t row, uint32_t column,
	void const* this_in)
{
	column_type const wanted  = column_type::Condition;
	auto              checked = CheckAddress(def, rows.RowCount(), row, column, &wanted, "condition");
	if (!checked)
	{
		return tl::make_unexpected(std::move(checked.error()));
	}
	closure const& c     = rows.conditions[row * rows.conditionCount + rows.slot[column]]->get();
	ErrorContext   error;
	bool           holds = c.function(c.data, this_in, &error);
	// An error may arrive as context alone, from a closure wrapping another's.
	if (!error.empty())
	{
		return tl::make_unexpected(std::move(error));
	}
	return holds;
}

tl::expected<counted_string, ErrorContext> ReadString(table_def const& def, table_rows const& rows, size_t row, uint32_t column)
{
	column_type const wanted  = column_type::String;
	auto              checked = CheckAddress(def, rows.RowCount(), row, column, &wanted, "string");
	if (!checked)
	{
		return tl::make_unexpected(std::move(checked.error()));
	}
	return rows.strings[row * rows.stringCount + rows.slot[column]];
}

tl::expected<std::vector<size_t>, ErrorContext> FindText(table_def const& def, table_rows const& rows, uint32_t column,
	std::string_view text)
{
	if (column >= def.columns.size())
	{
		return tl::make_unexpected(ErrorContext("table '", def.name, "' has no column ", column));
	}
	column_def const& c = def.columns[column];
	if (c.type != column_type::String || !c.key)
	{
		return tl::make_unexpected(ErrorContext("column '", c.name, "' of '", def.name,
			"' is not a string key, so it cannot be looked up by text"));
	}
	std::vector<size_t> out;
	auto const& index = rows.stringIndex[column];
	auto        found = index.find(text);
	if (found != index.end())
	{
		out.assign(found->second.begin(), found->second.end());
	}
	return out;
}

tl::expected<std::vector<size_t>, ErrorContext> FindKey(table_def const& def, table_rows const& rows, uint32_t column, cell value)
{
	if (column >= def.columns.size())
	{
		return tl::make_unexpected(ErrorContext("table '", def.name, "' has no column ", column));
	}
	if (!def.columns[column].key)
	{
		return tl::make_unexpected(ErrorContext("column '", def.columns[column].name, "' of '", def.name,
			"' is not a key, so it cannot be looked up"));
	}
	if (def.columns[column].type == column_type::String)
	{
		return tl::make_unexpected(ErrorContext("column '", def.columns[column].name, "' of '", def.name,
			"' is a string; look it up by text"));
	}
	auto fits = Check32(def.columns[column], def, value.i);
	if (!fits)
	{
		return tl::make_unexpected(std::move(fits.error()));
	}

	std::vector<size_t> out;
	auto const& index = rows.index[column];
	auto        found = index.find(value.i);
	if (found != index.end())
	{
		out.assign(found->second.begin(), found->second.end());
	}
	return out;
}

tl::expected<row_range, ErrorContext> FindPrefix(table_def const& def, table_rows const& rows, std::span<cell const> prefix)
{
	if (rows.orderSlots.empty())
	{
		return tl::make_unexpected(ErrorContext("table '", def.name, "' has no order to look up by"));
	}
	if (prefix.size() > rows.orderSlots.size())
	{
		return tl::make_unexpected(ErrorContext("a prefix of ", prefix.size(), " values for table '", def.name,
			"', whose order has ", rows.orderSlots.size(), " columns"));
	}
	for (size_t k = 0; k < prefix.size(); ++k)
	{
		auto fits = Check32(def.columns[def.order[static_cast<uint32_t>(k)]], def, prefix[k].i);
		if (!fits)
		{
			return tl::make_unexpected(std::move(fits.error()));
		}
	}
	size_t count = rows.RowCount();
	return row_range{
		OrderBound(rows.cells, count, rows.valueCount, rows.orderSlots, prefix, false),
		OrderBound(rows.cells, count, rows.valueCount, rows.orderSlots, prefix, true),
	};
}

}

table_def const& TableVersion::definition() const { return _data->def; }
size_t   TableVersion::RowCount() const           { return _data->rows.RowCount(); }
uint64_t TableVersion::OwnerOf(size_t row) const  { return _data->rows.owners[row]; }
uint64_t TableVersion::Generation() const         { return _data->rows.generation; }

tl::expected<cell, ErrorContext> TableVersion::Value(size_t row, uint32_t column) const
{
	return ReadValue(_data->def, _data->rows, row, column);
}

tl::expected<std::string_view, ErrorContext> TableVersion::CommandAt(size_t row, uint32_t column) const
{
	return ReadCommand(_data->def, _data->rows, row, column);
}

tl::expected<std::vector<size_t>, ErrorContext> TableVersion::Find(uint32_t column, cell value) const
{
	return FindKey(_data->def, _data->rows, column, value);
}

tl::expected<row_range, ErrorContext> TableVersion::FindOrdered(std::span<cell const> prefix) const
{
	return FindPrefix(_data->def, _data->rows, prefix);
}

tl::expected<cell, ErrorContext> relation_store::Value(uint32_t table, size_t row, uint32_t column) const
{
	if (table >= _storage->tables.size())
	{
		return tl::make_unexpected(ErrorContext("no table ", table));
	}
	return ReadValue(_storage->definition.tables()[table], _storage->RowsOf(table), row, column);
}

tl::expected<counted_string, ErrorContext> TableVersion::StringAt(size_t row, uint32_t column) const
{
	return ReadString(_data->def, _data->rows, row, column);
}

tl::expected<std::vector<size_t>, ErrorContext> TableVersion::Find(uint32_t column, std::string_view text) const
{
	return FindText(_data->def, _data->rows, column, text);
}

tl::expected<counted_string, ErrorContext> relation_store::StringAt(uint32_t table, size_t row, uint32_t column) const
{
	if (table >= _storage->tables.size())
	{
		return tl::make_unexpected(ErrorContext("no table ", table));
	}
	return ReadString(_storage->definition.tables()[table], _storage->RowsOf(table), row, column);
}

tl::expected<std::vector<size_t>, ErrorContext> relation_store::Find(uint32_t table, uint32_t column, std::string_view text) const
{
	if (table >= _storage->tables.size())
	{
		return tl::make_unexpected(ErrorContext("no table ", table));
	}
	return FindText(_storage->definition.tables()[table], _storage->RowsOf(table), column, text);
}

tl::expected<bool, ErrorContext> relation_store::Holds(uint32_t table, size_t row, uint32_t column, void const* this_in) const
{
	if (table >= _storage->tables.size())
	{
		return tl::make_unexpected(ErrorContext("no table ", table));
	}
	return ReadHolds(_storage->definition.tables()[table], _storage->RowsOf(table), row, column, this_in);
}

tl::expected<bool, ErrorContext> TableVersion::Holds(size_t row, uint32_t column, void const* this_in) const
{
	return ReadHolds(_data->def, _data->rows, row, column, this_in);
}

tl::expected<std::string_view, ErrorContext> relation_store::CommandAt(uint32_t table, size_t row, uint32_t column) const
{
	if (table >= _storage->tables.size())
	{
		return tl::make_unexpected(ErrorContext("no table ", table));
	}
	return ReadCommand(_storage->definition.tables()[table], _storage->RowsOf(table), row, column);
}

tl::expected<std::vector<size_t>, ErrorContext> relation_store::Find(uint32_t table, uint32_t column, cell value) const
{
	if (table >= _storage->tables.size())
	{
		return tl::make_unexpected(ErrorContext("no table ", table));
	}
	return FindKey(_storage->definition.tables()[table], _storage->RowsOf(table), column, value);
}

tl::expected<row_range, ErrorContext> relation_store::FindOrdered(uint32_t table, std::span<cell const> prefix) const
{
	if (table >= _storage->tables.size())
	{
		return tl::make_unexpected(ErrorContext("no table ", table));
	}
	return FindPrefix(_storage->definition.tables()[table], _storage->RowsOf(table), prefix);
}

}
