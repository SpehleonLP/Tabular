#ifndef TABULAR_RELATION_INTERNAL_H
#define TABULAR_RELATION_INTERNAL_H

// The store's row layout, shared by relation.cpp, pick.cpp and grid.cpp. Not a
// public header: nothing outside src/ includes it.

#include "tabular/relation.h"
#include "tabular/grid.h"
#include "table_layout.h"

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tabular
{

// Inline in detail, not an anonymous namespace: a type from an anonymous
// namespace inside table_rows would be a different type in every TU that
// includes this header.
namespace detail
{

inline void ReleaseClosure(closure const& c)
{
	if (c.release)
	{
		c.release(c.data);
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

inline held_condition Hold(closure const& c)
{
	return UncountedWrap(static_cast<HeldCondition const*>(new HeldCondition(c)));
}

}

// A table's rows: three parallel slices, one per kind of column, so a row
// moves as one when the table is sorted or compacted. A writable table's live
// in the store; a read-only table's live in its current TableVersion.
struct table_rows : slot_layout
{
	std::vector<cell>        cells;      // value columns, row-major
	std::vector<detail::held_condition> conditions; // Condition columns, row-major
	std::vector<std::string> commands;   // Command columns, row-major
	std::vector<counted_string> strings; // String columns, row-major, interned
	std::vector<uint64_t>    owners;     // one per row
	// Versions only: a row's index within its owner's load (or, in a mutable
	// best table, among the owner's inserts), so a change orders the rows it
	// keeps against the rows it brings by (owner, index).
	std::vector<uint32_t>    ownerRows;
	uint64_t                 generation = 0;

	std::vector<uint32_t> orderSlots;    // the order's columns, as value slots

	// Per column, value -> rows in row order; filled for key columns only.
	std::vector<std::unordered_map<int64_t, std::vector<uint32_t>>> index;
	// The same for String key columns, keyed by text. The views point into the
	// handles in strings, whose bytes do not move.
	std::vector<std::unordered_map<std::string_view, std::vector<uint32_t>>> stringIndex;

	// No rows, laid out for def.
	explicit table_rows(table_def const& def) : slot_layout(def)
	{
		index.resize(def.columns.size());
		stringIndex.resize(def.columns.size());
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

namespace detail
{

// The one call of a row's script condition: whether it holds for this_in,
// any failure the closure reports left in error (which stays empty when it
// answers). A row with no script condition holds. Adds nothing to error, so a
// caller that drops it allocates nothing the host did not.
inline bool CallCondition(table_rows const& rows, size_t row, uint32_t column, void const* this_in, ErrorContext& error)
{
	closure const& c = rows.conditions[row * rows.conditionCount + rows.slot[column]]->get();
	if (c.function == nullptr)
	{
		return true;
	}
	return c.function(c.data, this_in, &error);
}

}

struct TableVersion::Data
{
	table_def  def;
	table_rows rows;
};

struct GridVersion::Data
{
	table_def                                     def;
	uint64_t                                      owner      = 0;
	uint64_t                                      generation = 0;
	uint32_t                                      rows       = 0;
	uint32_t                                      columns    = 0;
	immutable_array<std::pair<int64_t, uint32_t>> rowKeys;      // (key, position), sorted by key
	immutable_array<std::pair<int64_t, uint32_t>> columnKeys;
	immutable_array<cell>                         values;       // value grid: rows * columns, row-major
	immutable_array<counted_string>               strings;      // String or Command grid
};

namespace detail
{
struct key_domains;
}

// Rows a change brings to one versioned table, laid out as load_rows lays
// them, with their conditions already held.
struct incoming_rows
{
	uint64_t                                owner = 0;
	size_t                                  count = 0;
	std::span<cell const>                   values;
	std::span<std::string_view const>       commands;
	std::span<detail::held_condition const> conditions;
	std::span<string_in const>              strings;
	// Insert: the owner's rows stay, and these follow them in the owner's
	// order. Load: these replace the owner's rows.
	bool                                    keepOwner = false;
};

struct relation_store::Storage
{
	schema                  definition;
	std::vector<table_rows> tables;     // a versioned table's stays empty
	// A read-only table's, a mutable best table's, or a mutable table's that
	// some best table's domain comes from; null for any other table.
	std::vector<counted_ptr<TableVersion const>> versions;
	std::vector<counted_ptr<GridVersion const>> grids;      // null for a table that is not a grid
	// Per table: the best tables whose key domain reads one of its columns.
	std::vector<std::vector<uint32_t>> dependents;

	// relation.cpp. The rows table holds, or candidate when table is the one
	// being changed: a domain reads the version a change would publish.
	table_rows const& RowsDuring(uint32_t table, uint32_t changed, table_rows const* candidate) const;
	void DomainOf(uint32_t table, table_rows const& rows, uint32_t changed, table_rows const* candidate,
		detail::key_domains& out) const;
	// Every best table whose domain comes from changed, checked against the
	// domain candidate gives it; orphans appended to report. change names the
	// change in a refusal.
	tl::expected<void, ErrorContext> CheckDependents(uint32_t changed, table_rows const& candidate,
		std::vector<run_note>* report, char const* change) const;
	// The tables marked in touched, and every best table depending on one,
	// surveyed in table order (a change that is never refused).
	void Survey(std::vector<uint8_t> const& touched, std::vector<run_note>& report) const;
	// A versioned table's next version: the current one with in's rows merged
	// in by order, owner and index, checked when in brings rows, then put in
	// place. False when nothing changed.
	tl::expected<bool, ErrorContext> Publish(uint32_t table, incoming_rows const& in, load_refusal* refusal,
		std::vector<run_note>* report, char const* noun, char const* change);

	static counted_ptr<GridVersion const> NewGrid(GridVersion::Data&& data)
	{
		auto owned = std::make_unique<GridVersion::Data>(std::move(data));
		return UncountedWrap(static_cast<GridVersion const*>(new GridVersion(std::move(owned))));
	}

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

	// The one place that decides whether a table is a grid: a grid has no
	// entry in tables or versions, so a row-table call must go through here
	// before it reaches them. Null for a table that is not a grid.
	GridVersion const* GridOf(uint32_t table) const
	{
		return grids[table].get();
	}

	// What a row-table call may read: the table's rows, or the refusal a grid
	// answers it with. Everything that takes a table index and then touches
	// rows asks here, so a grid never reaches the empty layout beside it.
	tl::expected<table_rows const*, ErrorContext> RowTable(uint32_t table) const
	{
		if (table >= tables.size())
		{
			return tl::make_unexpected(ErrorContext("no table ", table));
		}
		if (GridOf(table))
		{
			return tl::make_unexpected(ErrorContext("table '", definition.tables()[table].name,
				"' is a grid; read it with Grid"));
		}
		return &RowsOf(table);
	}
};

// Picks and grids read a version's rows; TableVersion keeps them private.
struct version_access
{
	static table_def const&  Def(TableVersion const& v)  { return v._data->def; }
	static table_rows const& Rows(TableVersion const& v) { return v._data->rows; }
};

}

#endif // TABULAR_RELATION_INTERNAL_H
