#ifndef TABULAR_RELATION_H
#define TABULAR_RELATION_H

#include "Errors/errorcontext.h"
#include "Support/counted_ptr.hpp"
#include "Support/counted_string.h"
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
// s-expression handed to its consumer, never interpreted here. A String column
// holds an interned counted_string in a slot array of its own.
enum class column_type : uint8_t
{
	Hash,
	Flags,
	Int,
	Double,
	Condition,
	Command,
	Vec2,
	String,
	// A condition stored as a vec2 cell: the row counts when its input x
	// satisfies start <= x < end, half-open as animation's phase bands are.
	Band,
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
// A null function is no condition: the row counts, and data and release are null too.
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

// Where a best table's key column takes its domain: the key values every
// published version must be total over. Nothing is inferred, so a best
// table's key that declares none is refused; a run whose key falls outside the
// domain is an orphan, reported and never checked.
enum class domain_kind : uint8_t
{
	None,      // not declared
	Members,   // a literal list
	Column,    // the distinct values another table's column holds in its current rows
	Rows,      // the table's own rows: the keys over their rows together take the sub-tuples the rows name
	// A message's type (a hash key; at most one per table). Open: any type may
	// arrive, and one no run keys on falls through to the blank run (key 0, a
	// blank CSV cell, "no message"). So the domain is the blank for every tuple
	// of the other keys, plus every run the rows name whose other keys are in
	// it: the blank run is the catch-all that must be total, and a run keyed on
	// a type is checked because it is there.
	Message,
	// A machine clause's output closes a key the same way; it is a fifth kind.
};

struct key_domain
{
	domain_kind           kind = domain_kind::None;
	std::span<cell const> members;   // Members: a hash key's as their CityHash32, an int key's as values
	std::string_view      table;     // Column: the table, by name; it may come later in the list
	std::string_view      column;    // Column: that table's column, by name

	static key_domain Rows()
	{
		key_domain d;
		d.kind = domain_kind::Rows;
		return d;
	}

	static key_domain Message()
	{
		key_domain d;
		d.kind = domain_kind::Message;
		return d;
	}
};

struct add_column
{
	std::string_view name;
	column_type      type = column_type::Int;
	bool             key  = false;   // a secondary equality index
	uint32_t         input = UINT32_MAX;   // Band or ranking Flags: index into create_table::inputs
	key_domain       domain;               // a best table's key: required; any other column: refused
};

// What a pick is given: a named, typed slot a band or a ranking flags column
// reads.
struct add_input
{
	std::string_view name;
	column_type      type   = column_type::Double;   // Double or Flags
	bool             ranged = false;                 // Double only: lo <= x < hi; picks clamp to it
	float            lo     = 0;
	float            hi     = 0;
};

// Weighted draws one row of the key run in proportion to a Double weight
// column; it has no conditions to test first (see PickWeighted).
enum class pick_policy : uint8_t { None, Best, All, Or, Weighted };

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

// A table written as a grid in CSV: keys down the side, the members of a flags
// column across the top, and each non-empty cell one row. Stored as ordinary
// rows; the pivot is the front end's.
struct pivot_by
{
	uint32_t headers = UINT32_MAX;   // the flags column the headers are members of
	uint32_t cells   = UINT32_MAX;   // the column each non-empty cell fills
};

struct grid_axis
{
	std::string_view name;
	column_type      type = column_type::Hash;   // Hash or Int
};

// A dense 2D array: every (row key, column key) pair has one cell of one type.
struct grid_shape
{
	grid_axis   rows;
	grid_axis   columns;
	column_type cells = column_type::Int;   // a value type, String or Command
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
	// What a pick is given, in this order. A band names a Double input and a
	// ranking flags column a Flags one; Tabular resolves no names at pick time.
	std::span<add_input const>    inputs;
	pick_policy                   pick   = pick_policy::None;
	uint32_t                      output = UINT32_MAX;   // Or: the Flags value column OR'd
	pivot_by const*               pivot  = nullptr;
	grid_shape const*             grid   = nullptr;   // set: this table is a grid
	// Named, the table gets one more Command column after its columns, and a CSV
	// load reads everything after the declared cells as that column's text, so
	// a command may hold commas. Not for a grid or a pivoted table.
	std::string_view              suffix;
	uint32_t                      weight = UINT32_MAX;   // Weighted: the Double column that is the weight
};

struct column_def
{
	std::string name;
	column_type type = column_type::Int;
	bool        key  = false;
	uint32_t    input = UINT32_MAX;
};

struct input_def
{
	std::string name;
	column_type type   = column_type::Double;
	bool        ranged = false;
	float       lo     = 0;
	float       hi     = 0;
};

// A key column's domain, resolved: members sorted and distinct, a foreign
// column as table and column numbers.
struct domain_def
{
	domain_kind              kind   = domain_kind::None;
	immutable_array<int64_t> members;
	uint32_t                 table  = UINT32_MAX;
	uint32_t                 column = UINT32_MAX;
};

struct table_def
{
	std::string                  name;
	immutable_array<column_def>  columns;
	immutable_array<uint32_t>    order;    // empty: insertion order
	bool                         unique    = false;
	bool                         read_only = false;
	immutable_array<input_def>   inputs;
	pick_policy                  pick      = pick_policy::None;
	uint32_t                     output    = UINT32_MAX;
	uint32_t                     ranking   = UINT32_MAX;   // the Flags column with an input
	uint32_t                     key_count = 0;            // columns with key: true
	uint32_t                     pivot_headers = UINT32_MAX;
	uint32_t                     pivot_cells   = UINT32_MAX;
	bool                         is_grid       = false;
	column_def                   grid_rows;               // name and type of the row axis
	column_def                   grid_columns;
	column_type                  grid_cells    = column_type::Int;
	bool                         suffix        = false;   // the last column is the suffix command
	uint32_t                     weight        = UINT32_MAX;
	immutable_array<domain_def>  domains;                 // a best table: one per key column, in column order
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

// A string cell to write: a view, or a counted_string the caller already
// holds. Either way the store interns it, so equal text is one handle, and a
// handle already interned in the default table is kept as it is. Valid only
// during the call, like every command view.
struct string_in
{
	string_in() = default;
	string_in(std::string_view text_in) : text(text_in) {}
	string_in(counted_string const& s) : text(s) {}

	std::string_view text;
};

// How a best table's key run stands after a change the store accepted. An
// orphan's key is outside its table's domain, so nothing about it is checked;
// a run that is not total is in the domain and some input picks no row from
// it, which only a bare retract (or a load of no rows) is allowed to leave.
enum class run_status : uint8_t
{
	Orphan,
	NotTotal,
};

struct run_note
{
	uint32_t          table  = 0;
	run_status        status = run_status::Orphan;
	std::vector<cell> key;   // the run's key values, in key column order
};

struct insert_row
{
	uint32_t                          table = 0;
	uint64_t                          owner = 0;
	std::span<cell const>             values;       // one per value column, in column order
	std::span<closure const>          conditions;   // one per Condition column, in column order
	std::span<std::string_view const> commands;     // one per Command column, in column order
	std::span<string_in const>        strings;      // one per String column, in column order
	std::vector<run_note>*            report = nullptr;   // if given, see relation_store
};

// Removes the owner's rows from every table, because one contributor (a
// package, a file) usually owns rows in several.
struct retract_owner
{
	uint64_t               owner  = 0;
	std::vector<run_note>* report = nullptr;   // if given, see relation_store
};

// Which rows of a refused load were at fault, as indices into the load, so a
// front end can name the lines its author wrote. SIZE_MAX when not a row's
// fault; earlier is set when two rows of the load clash with each other, and
// covered_by when row is a best table's dead row: the loaded row tried just
// before it that covers it.
struct load_refusal
{
	size_t row        = SIZE_MAX;
	size_t earlier    = SIZE_MAX;
	size_t covered_by = SIZE_MAX;
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
	std::span<string_in const>        strings;    // count * String columns
	std::vector<run_note>*            report = nullptr;   // if given, see relation_store
};

// Which keys of a refused grid load clash, as indices into the load's key spans.
struct grid_refusal
{
	size_t row            = SIZE_MAX;
	size_t earlier_row    = SIZE_MAX;
	size_t column         = SIZE_MAX;
	size_t earlier_column = SIZE_MAX;
};

// A whole grid from one contributor: a load replaces it. Cells are row-major,
// row_keys.size() * column_keys.size() of them, in values for a value grid or
// strings for a String or Command grid; the other span stays empty.
struct load_grid
{
	uint32_t                   table = 0;
	uint64_t                   owner = 0;
	std::span<cell const>      row_keys;
	std::span<cell const>      column_keys;
	std::span<cell const>      values;
	std::span<string_in const> strings;
	grid_refusal*              refusal = nullptr;
};

// Rows [first, last), in row order.
struct row_range
{
	size_t first = 0;
	size_t last  = 0;
};

struct version_access;
class GridVersion;

// One version of a read-only table or of a mutable best table. Load, Insert
// and Retract build a whole new version, check it, and only then put it in
// place of the one before; a version is never modified once built. Its row
// numbers are valid for as long as it is held, so a pick of a best table reads
// one version whichever kind of table it is.
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
	tl::expected<counted_string, ErrorContext>      StringAt(size_t row, uint32_t column) const;
	tl::expected<std::vector<size_t>, ErrorContext> Find(uint32_t column, std::string_view text) const;
	tl::expected<std::vector<size_t>, ErrorContext> Find(uint32_t column, cell value) const;
	tl::expected<row_range, ErrorContext>           FindOrdered(std::span<cell const> prefix) const;

	void AddRef() const { _refs.fetch_add(1, std::memory_order_relaxed); }
	void Release() const;

private:
	friend class relation_store;
	friend struct version_access;
	struct Data;
	explicit TableVersion(std::unique_ptr<Data>&& data);

	std::unique_ptr<Data const> _data;
	mutable std::atomic<int>    _refs{1};
};

// Live rows for every table of a schema.
//
// Rows keep their table's order, or insertion order without one; a retract
// keeps the survivors' order. A versioned table (a read-only one, a mutable
// best one, or a mutable one some best table's key domain comes from) keeps
// rows with equal order keys by owner id, then by index within the owner's
// load (or, mutable, the owner's inserts), so a version does not depend on
// which owner wrote last. Row indices are valid only until the next Insert, Load or
// Retract, because each moves rows; a reader that must keep its rows across
// them holds the table's Version.
//
// A best table's published version is total over its key domain. A change
// that brings rows (an Insert, a Load of one row or more) is refused unless the
// version it would publish is total, has no dead row and no two owners' rows
// with equal flags overlapping, and unless every best table whose domain comes
// from the changed table stays so against its new domain. A bare Retract, or a
// Load of no rows, is never refused. A command's report, when given, is
// cleared and, if the change is accepted, filled with every orphan run of each
// table the change touched and of their dependents, and after a bare retract
// with every run of theirs that is not total too: their state, not what the
// change altered. By table index, then key.
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
	// closures are released, so a caller never has to. Mutable tables only.
	tl::expected<void, ErrorContext> Insert(insert_row const& command);
	void                             Retract(retract_owner const& command);
	// Builds the table's next version, checks it, and only then puts it in
	// place; on refusal the current version is untouched. Read-only tables only.
	tl::expected<void, ErrorContext> Load(load_rows const& command);

	// A grid's whole next version, checked and then put in place; on refusal
	// the current one is untouched. Grid tables only.
	tl::expected<void, ErrorContext>                           LoadGrid(load_grid const& command);
	// A grid's current version; refused for a table that is not a grid.
	tl::expected<counted_ptr<GridVersion const>, ErrorContext> Grid(uint32_t table) const;

	// The current version of a versioned table (a read-only table, a mutable
	// best table, or a mutable table a best table's key domain comes from), for
	// a reader that holds it across changes; refused for any other table.
	tl::expected<counted_ptr<TableVersion const>, ErrorContext> Version(uint32_t table) const;

	size_t   RowCount(uint32_t table) const;
	uint64_t OwnerOf(uint32_t table, size_t row) const;

	// A value column's cell. Condition, Command and String columns are read
	// with Holds, CommandAt and StringAt instead.
	tl::expected<cell, ErrorContext>             Value(uint32_t table, size_t row, uint32_t column) const;
	tl::expected<bool, ErrorContext>             Holds(uint32_t table, size_t row, uint32_t column, void const* this_in) const;
	tl::expected<std::string_view, ErrorContext> CommandAt(uint32_t table, size_t row, uint32_t column) const;
	tl::expected<counted_string, ErrorContext>   StringAt(uint32_t table, size_t row, uint32_t column) const;

	// Changes whenever the table's rows change, and only then. A consumer
	// compares it with the value it last saw to learn that it must re-read.
	uint64_t Generation(uint32_t table) const;

	// Rows whose key column equals value, in row order. Only key columns can
	// be searched; a lookup, not a scan.
	tl::expected<std::vector<size_t>, ErrorContext> Find(uint32_t table, uint32_t column, cell value) const;

	// Rows whose String key column holds text, in row order.
	tl::expected<std::vector<size_t>, ErrorContext> Find(uint32_t table, uint32_t column, std::string_view text) const;

	// Rows whose leading order columns equal prefix, by binary search. The
	// prefix may be shorter than the order; an empty one matches every row.
	tl::expected<row_range, ErrorContext> FindOrdered(uint32_t table, std::span<cell const> prefix) const;

private:
	struct Storage;
	std::unique_ptr<Storage> _storage;
};

}

#endif // TABULAR_RELATION_H
