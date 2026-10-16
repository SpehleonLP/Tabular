#ifndef TABULAR_RELATION_H
#define TABULAR_RELATION_H

#include "Errors/errorcontext.h"
#include "Support/shared_array.hpp"
#include <expected/include/tl/expected.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tabular
{

// The value types are the only values Tabular knows; a host converts its own
// values to these, and a flag set (AngelScript uint32) and a hash (CityHash32)
// travel as int64_t, differing only in how a condition reads them.
//
// Condition and Command columns hold no value. A condition is a closure the
// host compiled, called with the object being tested. A command is an
// s-expression handed to its consumer, never interpreted here.
enum class column_type : uint8_t
{
	Hash,
	Flags,
	Int,
	Double,
	Condition,
	Command,
};

// One value in a row. The column's type says which member is live.
union cell
{
	int64_t i;
	double  d;

	static cell Integer(int64_t v) { cell c; c.i = v; return c; }
	static cell Real(double v)     { cell c; c.d = v; return c; }
};
static_assert(sizeof(cell) == 8);

// A condition the host compiled. function returns whether the row counts for
// this_in; when it cannot answer it fills error_out. release frees data and is
// called once, when the row is retracted, refused or destroyed.
struct closure
{
	bool (*function)(void* data, void const* this_in, ErrorContext* error_out) = nullptr;
	void* data                                                            = nullptr;
	void (*release)(void* data)                                           = nullptr;
};

// ---- Schema commands --------------------------------------------------------
// Commands are views, valid only during the call that consumes them; the
// builder copies what it keeps. Tables are numbered by their position in the
// list given to BuildSchema.

struct add_column
{
	std::string_view name;
	column_type      type = column_type::Int;
	bool             key  = false;   // a secondary equality index
};

// The order a table's rows are stored in: rows compare by these columns in
// turn, most significant first, and rows that compare equal keep insertion
// order. A unique order refuses a second row with the same key instead of
// replacing the first, so two contributors claiming one key is an error
// someone sees.
struct order_by
{
	std::span<uint32_t const> columns;   // column numbers of the table
	bool                      unique = false;
};

struct create_table
{
	std::string_view              name;
	std::span<add_column const>   columns;
	order_by const*               ordering = nullptr;   // none: insertion order
};

struct column_def
{
	std::string name;
	column_type type = column_type::Int;
	bool        key  = false;
};

struct table_def
{
	std::string                  name;
	immutable_array<column_def>  columns;
	immutable_array<uint32_t>    order;    // empty: insertion order
	bool                         unique = false;
};

// Immutable once built. Hot reload builds a new one.
class schema
{
public:
	schema() = default;

	std::span<table_def const> tables() const { return {_tables.begin(), _tables.size()}; }

	tl::expected<uint32_t, ErrorContext> FindTable(std::string_view name) const;
	tl::expected<uint32_t, ErrorContext> FindColumn(uint32_t table, std::string_view name) const;

private:
	explicit schema(immutable_array<table_def>&& tables) : _tables(std::move(tables)) {}
	friend tl::expected<schema, ErrorContext> BuildSchema(std::span<create_table const> tables);

	immutable_array<table_def> _tables;
};

tl::expected<schema, ErrorContext> BuildSchema(std::span<create_table const> tables);

// ---- Row commands -----------------------------------------------------------
// Every row records the owner that contributed it. Reloading a contributor is
// retract_owner followed by its insert_rows again: what an install placed is
// recorded as rows, so it can be found and replaced.

struct insert_row
{
	uint32_t                          table = 0;
	uint64_t                          owner = 0;
	std::span<cell const>             values;       // one per value column, in column order
	std::span<closure const>          conditions;   // one per Condition column, in column order
	std::span<std::string_view const> commands;     // one per Command column, in column order
};

// Removes the owner's rows from every table, because one contributor (a
// package, a file) usually owns rows in several.
struct retract_owner
{
	uint64_t owner = 0;
};

// Rows [first, last), in row order.
struct row_range
{
	size_t first = 0;
	size_t last  = 0;
};

// Live rows for every table of a schema.
//
// Rows keep their table's order, or insertion order without one; a retract
// keeps the survivors' order. Row indices are valid only until the next
// Insert or Retract, because both move rows.
class relation_store
{
public:
	explicit relation_store(schema&& definition);
	~relation_store();
	relation_store(relation_store&&) noexcept;
	relation_store& operator=(relation_store&&) noexcept;
	relation_store(relation_store const&)            = delete;
	relation_store& operator=(relation_store const&) = delete;

	schema const& definition() const;

	// Adopts the row's closures whether or not it succeeds: a refused row's
	// closures are released, so a caller never has to.
	tl::expected<void, ErrorContext> Insert(insert_row const& command);
	void                             Retract(retract_owner const& command);

	size_t   RowCount(uint32_t table) const;
	uint64_t OwnerOf(uint32_t table, size_t row) const;

	// A value column's cell. Condition and Command columns are read with
	// Holds and CommandAt instead.
	tl::expected<cell, ErrorContext>             Value(uint32_t table, size_t row, uint32_t column) const;
	tl::expected<bool, ErrorContext>             Holds(uint32_t table, size_t row, uint32_t column, void const* this_in) const;
	tl::expected<std::string_view, ErrorContext> CommandAt(uint32_t table, size_t row, uint32_t column) const;

	// Changes whenever the table's rows change, and only then. A consumer
	// compares it with the value it last saw to learn that it must re-read.
	uint64_t Generation(uint32_t table) const;

	// Rows whose key column equals value, in row order. Only key columns can
	// be searched; a lookup, not a scan.
	tl::expected<std::vector<size_t>, ErrorContext> Find(uint32_t table, uint32_t column, cell value) const;

	// Rows whose leading order columns equal prefix, by binary search. The
	// prefix may be shorter than the order; an empty one matches every row.
	tl::expected<row_range, ErrorContext> FindOrdered(uint32_t table, std::span<cell const> prefix) const;

private:
	struct Storage;
	std::unique_ptr<Storage> _storage;
};

}

#endif // TABULAR_RELATION_H
