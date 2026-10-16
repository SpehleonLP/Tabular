#ifndef TABULAR_RELATION_H
#define TABULAR_RELATION_H

#include "Errors/errorcontext.h"
#include <expected/include/tl/expected.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace tabular
{

// The only value types Tabular knows. A host converts its own values to these;
// a flag set (AngelScript uint32) and a hash (CityHash32) travel as int64_t and
// differ only in how a condition reads them.
enum class column_type : uint8_t
{
	Hash,
	Flags,
	Int,
	Double,
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

// ---- Schema commands --------------------------------------------------------
// A table definition is a list of these, applied in order. Tables are numbered
// by the order of their create_table commands, so add_column names a table by
// that number rather than by a string lookup.

struct create_table
{
	std::string name;
};

struct add_column
{
	uint32_t    table = 0;
	std::string name;
	column_type type  = column_type::Int;
	bool        key   = false;
};

// The order a table's rows are stored in, and whether it is unique: rows
// compare by these columns in turn, most significant first, and rows that
// compare equal keep insertion order. A table without one keeps insertion
// order. At most one per table. A unique order refuses a second row with the
// same key instead of replacing the first, so two contributors claiming one
// key is an error someone sees.
struct order_by
{
	uint32_t              table = 0;
	std::vector<uint32_t> columns;   // column numbers of that table
	bool                  unique = false;
};

using schema_command = std::variant<create_table, add_column, order_by>;

struct column_def
{
	std::string name;
	column_type type;
	bool        key;
};

struct table_def
{
	std::string             name;
	std::vector<column_def> columns;
	std::vector<uint32_t>   order;    // empty: insertion order
	bool                    unique = false;
};

// Immutable once built. Hot reload builds a new one.
class schema
{
public:
	schema() = default;

	std::span<table_def const> tables() const { return _tables; }

	tl::expected<uint32_t, ErrorContext> FindTable(std::string_view name) const;
	tl::expected<uint32_t, ErrorContext> FindColumn(uint32_t table, std::string_view name) const;

private:
	explicit schema(std::vector<table_def>&& tables) : _tables(std::move(tables)) {}
	friend tl::expected<schema, ErrorContext> BuildSchema(std::span<schema_command const> commands);

	std::vector<table_def> _tables;
};

tl::expected<schema, ErrorContext> BuildSchema(std::span<schema_command const> commands);

// ---- Row commands -----------------------------------------------------------
// Every row records the owner that contributed it. Reloading a contributor is
// retract_owner followed by its insert_rows again: what an install placed is
// recorded as rows, so it can be found and replaced.

struct insert_row
{
	uint32_t          table = 0;
	uint64_t          owner = 0;
	std::vector<cell> values;   // one per column, in column order
};

// Removes the owner's rows from every table, because one contributor (a
// package, a file) usually owns rows in several.
struct retract_owner
{
	uint64_t owner = 0;
};

using row_command = std::variant<insert_row, retract_owner>;

// Rows [first, last), in row order.
struct row_range
{
	size_t first = 0;
	size_t last  = 0;
};

// Live rows for every table of a schema.
//
// Rows keep insertion order; a retract keeps the survivors' order. Row indices
// are valid only until the next Apply, because a retract compacts the rows.
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

	tl::expected<void, ErrorContext> Apply(row_command const& command);

	size_t                RowCount(uint32_t table) const;
	std::span<cell const> Row(uint32_t table, size_t row) const;
	uint64_t              OwnerOf(uint32_t table, size_t row) const;

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
