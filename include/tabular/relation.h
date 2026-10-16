#ifndef TABULAR_RELATION_H
#define TABULAR_RELATION_H

#include "Errors/errorcontext.h"
#include "Support/counted_ptr.hpp"
#include "Support/shared_array.hpp"
#include <expected/include/tl/expected.hpp>

#include <atomic>
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
// travel as int64_t, differing only in how a condition reads them. A vec2 is
// two floats; it is the only vector, and a band a..b is one.
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
	Vec2,
};

// One value in a row. The column's type says which member is live.
union cell
{
	int64_t i;
	double  d;
	float   v[2];

	static cell Integer(int64_t v) { cell c; c.i = v; return c; }
	static cell Real(double v)     { cell c; c.d = v; return c; }
	static cell Vector(float x, float y) { cell c; c.v[0] = x; c.v[1] = y; return c; }
};
static_assert(sizeof(cell) == 8);

// A condition the host compiled. function returns whether the row counts for
// this_in; when it cannot answer it fills error_out. release frees data and is
// called once, when no table version or writable table holds the row any longer, or when the row is refused.
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
	order_by const*               ordering  = nullptr;   // none: insertion order
	// Rows come only from Load, and each load replaces the table with a new
	// version rather than updating it. A table loaded from files is one of
	// these; engine data that scripts add rows to is not.
	bool                          read_only = false;
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
	bool                         unique    = false;
	bool                         read_only = false;
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

// Which rows of a refused load were at fault, as indices into the load, so a
// front end can name the lines its author wrote. SIZE_MAX when not a row's
// fault; earlier is set when two rows of the load clash with each other.
struct load_refusal
{
	size_t row     = SIZE_MAX;
	size_t earlier = SIZE_MAX;
};

// Replaces every row the owner has in one read-only table with count new
// rows: a file reloaded is one load. Values, conditions and commands are
// row-major, one row's columns of that kind after another. Load adopts every
// closure it is given, succeed or fail: a refused load releases them.
struct load_rows
{
	uint32_t                          table  = 0;
	uint64_t                          owner  = 0;
	size_t                            count  = 0;
	std::span<cell const>             values;     // count * value columns
	std::span<std::string_view const> commands;   // count * Command columns
	load_refusal*                     refusal = nullptr;   // filled on a refusal, if given
	std::span<closure const>          conditions; // count * Condition columns
};

// Rows [first, last), in row order.
struct row_range
{
	size_t first = 0;
	size_t last  = 0;
};

// One version of a read-only table. Load and Retract build a whole new
// version, check it, and only then put it in place of the one before; a
// version is never modified once built. Its row numbers are valid for as long
// as it is held.
class TableVersion
{
public:
	~TableVersion();
	TableVersion(TableVersion const&)            = delete;
	TableVersion& operator=(TableVersion const&) = delete;

	table_def const& definition() const;
	size_t           RowCount() const;
	uint64_t         OwnerOf(size_t row) const;
	uint64_t         Generation() const;

	tl::expected<cell, ErrorContext>                Value(size_t row, uint32_t column) const;
	tl::expected<std::string_view, ErrorContext>    CommandAt(size_t row, uint32_t column) const;
	tl::expected<bool, ErrorContext>                Holds(size_t row, uint32_t column, void const* this_in) const;
	tl::expected<std::vector<size_t>, ErrorContext> Find(uint32_t column, cell value) const;
	tl::expected<row_range, ErrorContext>           FindOrdered(std::span<cell const> prefix) const;

	void AddRef() const { _refs.fetch_add(1, std::memory_order_relaxed); }
	void Release() const;

private:
	friend class relation_store;
	struct Data;
	explicit TableVersion(std::unique_ptr<Data>&& data);

	std::unique_ptr<Data const> _data;
	mutable std::atomic<int>    _refs{1};
};

// Live rows for every table of a schema.
//
// Rows keep their table's order, or insertion order without one; a retract
// keeps the survivors' order. Row indices are valid only until the next
// Insert, Load or Retract, because each moves rows; a reader that must keep
// its rows across them holds a read-only table's Version.
//
// One thread at a time: a Load or Retract replacing a version while another
// thread takes it from Version is a race.
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
	// Builds the table's next version, checks it, and only then puts it in
	// place; on refusal the current version is untouched. Read-only tables only.
	tl::expected<void, ErrorContext> Load(load_rows const& command);

	// The current version of a read-only table, for a reader that holds it
	// across loads; refused for a writable table.
	tl::expected<counted_ptr<TableVersion const>, ErrorContext> Version(uint32_t table) const;

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
