# Relations step 1: table definitions as command structs — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The first code of the new Tabular: plain relations (tables of typed rows, each row owned by a contributor) built and filled entirely from flat command structs, with no parser.

**Architecture:** Two command variants. `schema_command` (`create_table`, `add_column`) builds an immutable `schema` through `BuildSchema`. `row_command` (`insert_row`, `retract_owner`) is applied to a `relation_store` made from that schema. Rows are flat arrays of 8-byte `cell`s, kept in insertion order. A table may declare an order (`order_by`, owner 2026-10-15): rows are stored sorted by those columns, optionally unique, and looked up by prefix with a binary search. Each table has a generation counter that changes whenever its rows change, and an equality index on its key columns. YAML and CSV front ends (steps 2 and 3) will produce these same commands. Machines are step 4.

**Tech Stack:** C++20, `tl::expected` + `ErrorContext` from `Spehleon/lib`, GoogleTest compiled into the Engine binary, CMake source lists.

**Spec:** `Spehleon/Tabular/docs/definition.md`, especially "First version (owner, 2026-10-15)", "Order of work", "Structure" (rows, keys, lifetimes, surface), "What it is not" (native types) and "The rule every other line answers to".

## Global Constraints

- The core must not include any Engine header (`src/…`), any script-engine header or any file IO: "Tabular can be defined, run and tested without a world: no objects, no threads, no script engine, no files." Allowed includes are the standard library, `Errors/errorcontext.h`, `Support/strings.h` and `expected/include/tl/expected.hpp`.
- Native value types are exactly **hash, flag set, `int64_t` and `double`**. A flag set (AngelScript `uint32`) and a hash (`CityHash32`) are carried as `int64_t`.
- **A key is equality**, so keys can be indexed: finding a row's candidates is a lookup, not a scan.
- Public headers live in `Spehleon/Tabular/include/tabular/`, sources in `Spehleon/Tabular/src/`, gtests in `Spehleon/Tabular/test/`. The namespace is `tabular` (lowercase), which keeps it apart from the old engine `Tabular` namespace in `src/Tabular` while that still builds.
- Every CMake path under `cmake/` goes through `${SPEHLEON_ROOT}`. Source lists are authoritative: add files to the lists, never glob.
- Errors are returned as `tl::expected<T, ErrorContext>`, never logged. Never use `LOG_F(ERROR)` or `LOG_F(FATAL)`.
- Always brace control-flow bodies. Comments explain why, not what.
- Big objects are never by-value parameters: use `const T&` or `T&&`.
- `snake_case` types are never `new`ed. A heap-allocated type is `PascalCase`.
- Work in a worktree: `scripts/wt-create.sh tabular-relations`, path `/mnt/Passport/Worktrees/tabular-relations` (written `$WT` below). Build with `env -C $WT PWD=$WT scripts/wt-build.sh`. Test with `$WT/Build/Engine --gtest --gtest_filter='Relation*'`, the worktree's own binary and never the main tree's. Never build while a subagent is live.
- Subagents make no git state changes. The coordinator commits. Commits go leaves first: `Spehleon/Tabular`, then `Spehleon`, then the superproject.

## Review Focus

1. **Row indices are held across an `Apply`.** A retract compacts rows, so an index saved earlier names a different row. Expect the header to say indices are valid until the next `Apply`, and `Find` after a retract to return the new positions. Pinned in Task 3 (`FindAfterRetractReturnsNewPositions`).
2. **Reloading the same owner twice, or retracting an owner that has no rows.** Expect no error and no generation change, because nothing changed. Pinned in Task 2 (`RetractOfAbsentOwnerChangesNothing`).
3. **A flag value outside `uint32`.** A negative value or one above 2^32-1 can't round-trip to AngelScript `uint32`. Expect `insert_row` to refuse it and name the column. Pinned in Task 2 (`FlagsOutOfRangeIsRefused`).
4. **A key on a `double` column.** Equality on doubles breaks on NaN and `-0.0`. Expect `BuildSchema` to refuse it. Pinned in Task 1 (`KeyOnDoubleIsRefused`).
5. **Extreme values: owner `UINT64_MAX`, negative `Int` keys, hash values with bit 31 set.** Expect them to store, look up and retract like any other value. Pinned in Task 3 (`ExtremeValuesLookUpAndRetract`).

---

### Task 1: Wiring, the public header, and `BuildSchema`

The header is the contract: it is written in full here, including the store, which Tasks 2 and 3 implement. **After this task, stop for the owner to review `include/tabular/relation.h` before Task 2 starts** (retrospective, conclusion 6: the first thing reviewed is the public header).

**Files:**
- Create: `Spehleon/Tabular/include/tabular/relation.h`
- Create: `Spehleon/Tabular/src/relation.cpp`
- Create: `Spehleon/Tabular/test/relation_schema_gtest.cpp`
- Create: `Spehleon/cmake/sources_tabular.cmake`
- Modify: `cmake/sources_spehleon.cmake` (line 1: add an `include`; next to `${CLIP_BUNDLE_SOURCES}` around line 294: add `${TABULAR_SOURCES}`)
- Modify: `cmake/include_dirs.cmake` (next to `${SPEHLEON_ROOT}/rintintin/include`: add `${SPEHLEON_ROOT}/Tabular/include`)
- Modify: `cmake/sources_tests.cmake` (after the `ClipBundle/test/*_gtest.cpp` lines: add the Tabular tests)

**Interfaces:**
- Consumes: `ErrorContext` (`Errors/errorcontext.h`; its constructor forwards its arguments to `cc`), `cc` (`Support/strings.h`), `tl::expected` / `tl::make_unexpected`.
- Produces: everything declared in `relation.h` below. Task 1 defines `schema::FindTable`, `schema::FindColumn` and `BuildSchema`.

- [ ] **Step 1: Create the worktree**

```bash
cd /mnt/Passport/Kreatures-Engine && scripts/wt-create.sh tabular-relations
```

- [ ] **Step 2: Write the public header**

`Spehleon/Tabular/include/tabular/relation.h`:

```cpp
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
```

- [ ] **Step 3: Wire the build**

`Spehleon/cmake/sources_tabular.cmake`:

```cmake
# The new Tabular library (Spehleon/Tabular). Its core includes only the standard
# library and Spehleon/lib's Errors and Support, so it builds and tests without a world.
set(TABULAR_SOURCES
	${SPEHLEON_ROOT}/Tabular/src/relation.cpp
)
```

In `cmake/sources_spehleon.cmake`, below the existing first line `include(${SPEHLEON_ROOT}/cmake/sources_clipbundle.cmake)`, add:

```cmake
include(${SPEHLEON_ROOT}/cmake/sources_tabular.cmake)
```

and on the line after `${CLIP_BUNDLE_SOURCES}` (around line 294) add:

```cmake
	${TABULAR_SOURCES}
```

In `cmake/include_dirs.cmake`, after `${SPEHLEON_ROOT}/rintintin/include`, add:

```cmake
	${SPEHLEON_ROOT}/Tabular/include
```

In `cmake/sources_tests.cmake`, after `${SPEHLEON_ROOT}/lib/ClipBundle/test/clip_move_eval_gtest.cpp`, add:

```cmake
	${SPEHLEON_ROOT}/Tabular/test/relation_schema_gtest.cpp
```

- [ ] **Step 4: Write the failing tests**

`Spehleon/Tabular/test/relation_schema_gtest.cpp`:

```cpp
#include <gtest/gtest.h>
#include "tabular/relation.h"

#include <string>
#include <string_view>
#include <vector>

using namespace tabular;

// The message mentions the text: errors must name what they refuse.
static bool Mentions(std::string const& message, std::string_view text)
{
	return message.find(text) != std::string::npos;
}

namespace
{

add_column const kSpawnColumns[] = {
	{"package", column_type::Hash,   true},
	{"section", column_type::Hash,   true},
	{"x",       column_type::Double, false},
	{"y",       column_type::Double, false},
};

tl::expected<schema, ErrorContext> BuildOne(create_table const& table)
{
	return BuildSchema(std::span<create_table const>(&table, 1));
}

tl::expected<schema, ErrorContext> SpawnPointsWith(order_by const& order)
{
	return BuildOne(create_table{"spawn_points", kSpawnColumns, &order});
}

}

TEST(RelationSchema, EmptyListBuildsNoTables)
{
	auto s = BuildSchema({});
	ASSERT_TRUE(s.has_value()) << s.error().message;
	EXPECT_TRUE(s->tables().empty());
}

TEST(RelationSchema, TablesAndColumnsKeepTheirOrder)
{
	add_column const rules[] = {
		{"when", column_type::Condition, false},
		{"then", column_type::Command,   false},
	};
	create_table const tables[] = {
		{"spawn_points", kSpawnColumns},
		{"rules", rules},
	};
	auto s = BuildSchema(tables);
	ASSERT_TRUE(s.has_value()) << s.error().message;
	ASSERT_EQ(s->tables().size(), 2u);

	table_def const& t = s->tables()[0];
	EXPECT_EQ(t.name, "spawn_points");
	ASSERT_EQ(t.columns.size(), 4u);
	EXPECT_EQ(t.columns[0].name, "package");
	EXPECT_EQ(t.columns[0].type, column_type::Hash);
	EXPECT_TRUE(t.columns[0].key);
	EXPECT_EQ(t.columns[3].name, "y");
	EXPECT_EQ(t.columns[3].type, column_type::Double);
	EXPECT_FALSE(t.columns[3].key);

	table_def const& r = s->tables()[1];
	EXPECT_EQ(r.name, "rules");
	EXPECT_EQ(r.columns[0].type, column_type::Condition);
	EXPECT_EQ(r.columns[1].type, column_type::Command);
}

TEST(RelationSchema, CommandViewsNeedNotOutliveTheBuild)
{
	tl::expected<schema, ErrorContext> s;
	{
		std::string name   = "temporary";
		std::string column = "n";
		add_column const columns[] = {{column, column_type::Int, false}};
		s = BuildOne(create_table{name, columns});
		name.assign("overwritten");
		column.assign("z");
	}
	ASSERT_TRUE(s.has_value()) << s.error().message;
	EXPECT_EQ(s->tables()[0].name, "temporary");
	EXPECT_EQ(s->tables()[0].columns[0].name, "n");
}

TEST(RelationSchema, FindTableAndColumnByName)
{
	auto s = BuildOne(create_table{"spawn_points", kSpawnColumns});
	ASSERT_TRUE(s.has_value()) << s.error().message;

	auto table = s->FindTable("spawn_points");
	ASSERT_TRUE(table.has_value());
	EXPECT_EQ(*table, 0u);

	auto column = s->FindColumn(*table, "y");
	ASSERT_TRUE(column.has_value());
	EXPECT_EQ(*column, 3u);

	auto missingTable = s->FindTable("nope");
	ASSERT_FALSE(missingTable.has_value());
	EXPECT_TRUE(Mentions(missingTable.error().message, "nope")) << missingTable.error().message;

	auto missingColumn = s->FindColumn(*table, "z");
	ASSERT_FALSE(missingColumn.has_value());
	EXPECT_TRUE(Mentions(missingColumn.error().message, "'z'")) << missingColumn.error().message;

	EXPECT_FALSE(s->FindColumn(7, "x").has_value());
}

TEST(RelationSchema, DuplicateTableNameIsRefused)
{
	add_column const columns[] = {{"n", column_type::Int, false}};
	create_table const tables[] = {{"a", columns}, {"a", columns}};
	auto s = BuildSchema(tables);
	ASSERT_FALSE(s.has_value());
	EXPECT_TRUE(Mentions(s.error().message, "'a'")) << s.error().message;
	EXPECT_TRUE(Mentions(s.error().message, "table 1")) << s.error().message;
}

TEST(RelationSchema, DuplicateColumnNameIsRefused)
{
	add_column const columns[] = {{"n", column_type::Int, false}, {"n", column_type::Double, false}};
	auto s = BuildOne(create_table{"a", columns});
	ASSERT_FALSE(s.has_value());
	EXPECT_TRUE(Mentions(s.error().message, "'n'")) << s.error().message;
}

TEST(RelationSchema, EmptyNamesAreRefused)
{
	add_column const columns[] = {{"n", column_type::Int, false}};
	EXPECT_FALSE(BuildOne(create_table{"", columns}).has_value());

	add_column const unnamed[] = {{"", column_type::Int, false}};
	EXPECT_FALSE(BuildOne(create_table{"a", unnamed}).has_value());
}

TEST(RelationSchema, TableWithoutColumnsIsRefused)
{
	auto s = BuildOne(create_table{"bare", {}});
	ASSERT_FALSE(s.has_value());
	EXPECT_TRUE(Mentions(s.error().message, "'bare'")) << s.error().message;
}

TEST(RelationSchema, OnlyHashFlagsAndIntCanBeKeys)
{
	for (column_type type : {column_type::Double, column_type::Condition, column_type::Command})
	{
		add_column const columns[] = {{"k", type, true}};
		auto s = BuildOne(create_table{"a", columns});
		ASSERT_FALSE(s.has_value());
		EXPECT_TRUE(Mentions(s.error().message, "'k'")) << s.error().message;
	}
	for (column_type type : {column_type::Hash, column_type::Flags, column_type::Int})
	{
		add_column const columns[] = {{"k", type, true}};
		EXPECT_TRUE(BuildOne(create_table{"a", columns}).has_value());
	}
}

TEST(RelationSchema, OrderIsRecorded)
{
	uint32_t const columns[] = {1, 0};
	auto s = SpawnPointsWith(order_by{columns, true});
	ASSERT_TRUE(s.has_value()) << s.error().message;
	table_def const& t = s->tables()[0];
	ASSERT_EQ(t.order.size(), 2u);
	EXPECT_EQ(t.order[0], 1u);
	EXPECT_EQ(t.order[1], 0u);
	EXPECT_TRUE(t.unique);
}

TEST(RelationSchema, TableWithoutOrderKeepsInsertionOrder)
{
	auto s = BuildOne(create_table{"spawn_points", kSpawnColumns});
	ASSERT_TRUE(s.has_value()) << s.error().message;
	EXPECT_EQ(s->tables()[0].order.size(), 0u);
	EXPECT_FALSE(s->tables()[0].unique);
}

TEST(RelationSchema, BadOrdersAreRefused)
{
	auto noColumns = SpawnPointsWith(order_by{{}, false});
	ASSERT_FALSE(noColumns.has_value());
	EXPECT_TRUE(Mentions(noColumns.error().message, "spawn_points")) << noColumns.error().message;

	uint32_t const missing[] = {9};
	auto missingColumn = SpawnPointsWith(order_by{missing, false});
	ASSERT_FALSE(missingColumn.has_value());
	EXPECT_TRUE(Mentions(missingColumn.error().message, "9")) << missingColumn.error().message;

	uint32_t const twice[] = {1, 1};
	auto repeated = SpawnPointsWith(order_by{twice, false});
	ASSERT_FALSE(repeated.has_value());
	EXPECT_TRUE(Mentions(repeated.error().message, "'section'")) << repeated.error().message;

	uint32_t const real[] = {2};
	auto onDouble = SpawnPointsWith(order_by{real, false});
	ASSERT_FALSE(onDouble.has_value());
	EXPECT_TRUE(Mentions(onDouble.error().message, "'x'")) << onDouble.error().message;

	add_column const rules[] = {{"when", column_type::Condition, false}};
	uint32_t const first[] = {0};
	order_by const byCondition{first, false};
	auto onCondition = BuildOne(create_table{"rules", rules, &byCondition});
	ASSERT_FALSE(onCondition.has_value());
	EXPECT_TRUE(Mentions(onCondition.error().message, "'when'")) << onCondition.error().message;
}
```

- [ ] **Step 5: Run the build to verify the tests fail**

Run: `env -C $WT PWD=$WT scripts/wt-build.sh`
Expected: a link failure with undefined references to `tabular::BuildSchema`, `schema::FindTable` and `schema::FindColumn`, because `src/relation.cpp` doesn't exist yet. Create it empty (just `#include "tabular/relation.h"`) so CMake configures. The tests must not pass at this point.

- [ ] **Step 6: Implement `BuildSchema` and the lookups**

`Spehleon/Tabular/src/relation.cpp`:

```cpp
#include "tabular/relation.h"

#include "Support/strings.h"

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
```

- [ ] **Step 7: Build and run the tests to verify they pass**

Run: `env -C $WT PWD=$WT scripts/wt-build.sh && $WT/Build/Engine --gtest --gtest_filter='RelationSchema.*'`
Expected: all 12 `RelationSchema` tests PASS.

- [ ] **Step 8: Check the no-world constraint**

Run: `grep -n '#include' $WT/Spehleon/Tabular/include/tabular/relation.h $WT/Spehleon/Tabular/src/relation.cpp`
Expected: only standard headers, `Errors/errorcontext.h`, `Support/strings.h` and `expected/include/tl/expected.hpp`. Anything else is a defect.

- [ ] **Step 9: Commit (coordinator), leaves first**

```bash
git -C $WT/Spehleon/Tabular add include/tabular/relation.h src/relation.cpp test/relation_schema_gtest.cpp
git -C $WT/Spehleon/Tabular commit -m "Relations: public header and BuildSchema from command structs"
git -C $WT/Spehleon add Tabular cmake/sources_tabular.cmake
git -C $WT/Spehleon commit -m "Build the new Tabular library"
git -C $WT add Spehleon cmake/sources_spehleon.cmake cmake/include_dirs.cmake cmake/sources_tests.cmake
git -C $WT commit -m "Build and test the new Tabular relations"
```

Each message ends with the session's attribution lines.

- [ ] **Step 10: Owner checkpoint**

Stop. Show the owner `include/tabular/relation.h` and wait for their review before Task 2.

---

### Task 2: `relation_store`: insert, retract, generation

> **Amended 2026-10-15 (owner header review).** The header in Task 1 is authoritative and changes this task's interface: no `row_command` variant (`Insert(insert_row const&) -> tl::expected<void, ErrorContext>` and `Retract(retract_owner const&) -> void`); `insert_row` carries views (`values` for value columns only, `conditions` adopted closures, `commands` copied text); `Row()` is replaced by `Value(table, row, column)`, plus `Holds(...)` and `CommandAt(...)` for Condition and Command columns. Where the code below conflicts with the header, the header wins; the executor ledgers each adaptation as a ruling. Tests for closure adoption (released exactly once on retract, on refusal and on destruction) and for `Holds` passing `this_in` and reporting `error_out` are added here.

**Files:**
- Modify: `Spehleon/Tabular/src/relation.cpp` (append the store)
- Create: `Spehleon/Tabular/test/relation_store_gtest.cpp`
- Modify: `cmake/sources_tests.cmake` (add the new test after `relation_schema_gtest.cpp`)

**Interfaces:**
- Consumes: `schema`, `BuildSchema`, the row commands and the `relation_store` declarations from Task 1.
- Produces: definitions of `relation_store`'s constructor, destructor, moves, `definition`, `Apply`, `RowCount`, `Row`, `OwnerOf` and `Generation`. `Find` returns "not implemented" until Task 3. The private `Storage` struct holds per-table `cells`, `owners`, `generation`, and an `index` vector sized to the column count, which Task 3 fills.

- [ ] **Step 1: Write the failing tests**

`Spehleon/Tabular/test/relation_store_gtest.cpp`:

```cpp
#include <gtest/gtest.h>
#include "tabular/relation.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace tabular;

// The message mentions the text: errors must name what they refuse.
static bool Mentions(std::string const& message, std::string_view text)
{
	return message.find(text) != std::string::npos;
}

namespace
{

// spawn_points(package: hash key, section: hash key, x: double, y: double)
// flags_table(mask: flags)
relation_store MakeStore()
{
	std::vector<schema_command> commands = {
		create_table{"spawn_points"},
		add_column{0, "package", column_type::Hash,   true},
		add_column{0, "section", column_type::Hash,   true},
		add_column{0, "x",       column_type::Double, false},
		add_column{0, "y",       column_type::Double, false},
		create_table{"flags_table"},
		add_column{1, "mask", column_type::Flags, false},
	};
	auto s = BuildSchema(commands);
	EXPECT_TRUE(s.has_value());
	return relation_store(std::move(*s));
}

insert_row Spawn(uint64_t owner, int64_t package, int64_t section, double x, double y)
{
	return insert_row{0, owner, {cell::Integer(package), cell::Integer(section), cell::Real(x), cell::Real(y)}};
}

}

TEST(RelationStore, StartsEmptyAtGenerationZero)
{
	relation_store store = MakeStore();
	EXPECT_EQ(store.RowCount(0), 0u);
	EXPECT_EQ(store.RowCount(1), 0u);
	EXPECT_EQ(store.Generation(0), 0u);
	EXPECT_EQ(store.definition().tables().size(), 2u);
}

TEST(RelationStore, InsertKeepsValuesOwnerAndOrder)
{
	relation_store store = MakeStore();
	ASSERT_TRUE(store.Apply(Spawn(7, 100, 200, 1.5, 2.5)).has_value());
	ASSERT_TRUE(store.Apply(Spawn(8, 101, 201, 3.5, 4.5)).has_value());

	ASSERT_EQ(store.RowCount(0), 2u);
	auto first = store.Row(0, 0);
	ASSERT_EQ(first.size(), 4u);
	EXPECT_EQ(first[0].i, 100);
	EXPECT_EQ(first[1].i, 200);
	EXPECT_EQ(first[2].d, 1.5);
	EXPECT_EQ(first[3].d, 2.5);
	EXPECT_EQ(store.OwnerOf(0, 0), 7u);
	EXPECT_EQ(store.Row(0, 1)[0].i, 101);
	EXPECT_EQ(store.OwnerOf(0, 1), 8u);
}

TEST(RelationStore, InsertChangesOnlyItsTablesGeneration)
{
	relation_store store = MakeStore();
	ASSERT_TRUE(store.Apply(Spawn(7, 1, 2, 0, 0)).has_value());
	EXPECT_NE(store.Generation(0), 0u);
	EXPECT_EQ(store.Generation(1), 0u);
}

TEST(RelationStore, RetractRemovesOnlyThatOwnerAndKeepsSurvivorOrder)
{
	relation_store store = MakeStore();
	ASSERT_TRUE(store.Apply(Spawn(1, 10, 0, 0, 0)).has_value());
	ASSERT_TRUE(store.Apply(Spawn(2, 20, 0, 0, 0)).has_value());
	ASSERT_TRUE(store.Apply(Spawn(1, 30, 0, 0, 0)).has_value());
	ASSERT_TRUE(store.Apply(Spawn(3, 40, 0, 0, 0)).has_value());

	ASSERT_TRUE(store.Apply(retract_owner{1}).has_value());

	ASSERT_EQ(store.RowCount(0), 2u);
	EXPECT_EQ(store.Row(0, 0)[0].i, 20);
	EXPECT_EQ(store.Row(0, 1)[0].i, 40);
	EXPECT_EQ(store.OwnerOf(0, 0), 2u);
	EXPECT_EQ(store.OwnerOf(0, 1), 3u);
}

TEST(RelationStore, RetractReachesEveryTable)
{
	relation_store store = MakeStore();
	ASSERT_TRUE(store.Apply(Spawn(5, 1, 2, 0, 0)).has_value());
	ASSERT_TRUE(store.Apply(insert_row{1, 5, {cell::Integer(3)}}).has_value());
	uint64_t before0 = store.Generation(0);
	uint64_t before1 = store.Generation(1);

	ASSERT_TRUE(store.Apply(retract_owner{5}).has_value());

	EXPECT_EQ(store.RowCount(0), 0u);
	EXPECT_EQ(store.RowCount(1), 0u);
	EXPECT_NE(store.Generation(0), before0);
	EXPECT_NE(store.Generation(1), before1);
}

TEST(RelationStore, RetractOfAbsentOwnerChangesNothing)
{
	relation_store store = MakeStore();
	ASSERT_TRUE(store.Apply(Spawn(1, 10, 0, 0, 0)).has_value());
	uint64_t before = store.Generation(0);

	ASSERT_TRUE(store.Apply(retract_owner{99}).has_value());
	ASSERT_TRUE(store.Apply(retract_owner{99}).has_value());

	EXPECT_EQ(store.RowCount(0), 1u);
	EXPECT_EQ(store.Generation(0), before);
}

TEST(RelationStore, ReloadIsRetractThenInsert)
{
	relation_store store = MakeStore();
	ASSERT_TRUE(store.Apply(Spawn(1, 10, 0, 0, 0)).has_value());
	ASSERT_TRUE(store.Apply(Spawn(2, 20, 0, 0, 0)).has_value());

	ASSERT_TRUE(store.Apply(retract_owner{1}).has_value());
	ASSERT_TRUE(store.Apply(Spawn(1, 11, 0, 0, 0)).has_value());

	ASSERT_EQ(store.RowCount(0), 2u);
	EXPECT_EQ(store.Row(0, 0)[0].i, 20);
	EXPECT_EQ(store.Row(0, 1)[0].i, 11);
}

TEST(RelationStore, WrongArityIsRefused)
{
	relation_store store = MakeStore();
	auto r = store.Apply(insert_row{0, 1, {cell::Integer(1)}});
	ASSERT_FALSE(r.has_value());
	EXPECT_TRUE(Mentions(r.error().message, "spawn_points")) << r.error().message;
	EXPECT_EQ(store.RowCount(0), 0u);
	EXPECT_EQ(store.Generation(0), 0u);
}

TEST(RelationStore, UnknownTableIsRefused)
{
	relation_store store = MakeStore();
	auto r = store.Apply(insert_row{9, 1, {cell::Integer(1)}});
	ASSERT_FALSE(r.has_value());
	EXPECT_TRUE(Mentions(r.error().message, "9")) << r.error().message;
}

TEST(RelationStore, FlagsOutOfRangeIsRefused)
{
	relation_store store = MakeStore();
	EXPECT_TRUE(store.Apply(insert_row{1, 1, {cell::Integer(0)}}).has_value());
	EXPECT_TRUE(store.Apply(insert_row{1, 1, {cell::Integer(INT64_C(0xFFFFFFFF))}}).has_value());

	auto negative = store.Apply(insert_row{1, 1, {cell::Integer(-1)}});
	ASSERT_FALSE(negative.has_value());
	EXPECT_TRUE(Mentions(negative.error().message, "'mask'")) << negative.error().message;

	auto tooBig = store.Apply(insert_row{1, 1, {cell::Integer(INT64_C(0x100000000))}});
	ASSERT_FALSE(tooBig.has_value());
	EXPECT_EQ(store.RowCount(1), 2u);
}

namespace
{

// sorted(section: int, package: int, mask: flags), ordered by (section, package).
relation_store MakeOrderedStore(bool unique)
{
	std::vector<schema_command> commands = {
		create_table{"sorted"},
		add_column{0, "section", column_type::Int,   false},
		add_column{0, "package", column_type::Int,   true},
		add_column{0, "mask",    column_type::Flags, false},
		order_by{0, {0, 1}, unique},
	};
	auto s = BuildSchema(commands);
	EXPECT_TRUE(s.has_value());
	return relation_store(std::move(*s));
}

insert_row Sorted(uint64_t owner, int64_t section, int64_t package, int64_t mask)
{
	return insert_row{0, owner, {cell::Integer(section), cell::Integer(package), cell::Integer(mask)}};
}

}

TEST(RelationStore, OrderedTableSortsRowsAndTiesKeepInsertionOrder)
{
	relation_store store = MakeOrderedStore(false);
	ASSERT_TRUE(store.Apply(Sorted(1, 5, 2, 0)).has_value());
	ASSERT_TRUE(store.Apply(Sorted(1, 3, 9, 0)).has_value());
	ASSERT_TRUE(store.Apply(Sorted(1, 5, 1, 0)).has_value());
	ASSERT_TRUE(store.Apply(Sorted(1, 5, 2, 1)).has_value());   // ties with the first row
	ASSERT_TRUE(store.Apply(Sorted(1, -4, 0, 0)).has_value());

	ASSERT_EQ(store.RowCount(0), 5u);
	std::vector<std::vector<int64_t>> got;
	for (size_t r = 0; r < store.RowCount(0); ++r)
	{
		auto row = store.Row(0, r);
		got.push_back({row[0].i, row[1].i, row[2].i});
	}
	std::vector<std::vector<int64_t>> want = {
		{-4, 0, 0}, {3, 9, 0}, {5, 1, 0}, {5, 2, 0}, {5, 2, 1},
	};
	EXPECT_EQ(got, want);
}

TEST(RelationStore, UniqueOrderRefusesASecondRowWithTheKey)
{
	relation_store store = MakeOrderedStore(true);
	ASSERT_TRUE(store.Apply(Sorted(1, 5, 2, 0)).has_value());
	uint64_t before = store.Generation(0);

	auto r = store.Apply(Sorted(2, 5, 2, 7));
	ASSERT_FALSE(r.has_value());
	EXPECT_TRUE(Mentions(r.error().message, "sorted")) << r.error().message;
	EXPECT_TRUE(Mentions(r.error().message, "owner 1")) << r.error().message;
	EXPECT_EQ(store.RowCount(0), 1u);
	EXPECT_EQ(store.Generation(0), before);

	EXPECT_TRUE(store.Apply(Sorted(2, 5, 3, 7)).has_value());   // a different key is fine
}

TEST(RelationStore, UniqueKeyCanBeReusedAfterItsOwnerReloads)
{
	relation_store store = MakeOrderedStore(true);
	ASSERT_TRUE(store.Apply(Sorted(1, 5, 2, 0)).has_value());
	ASSERT_TRUE(store.Apply(retract_owner{1}).has_value());
	auto r = store.Apply(Sorted(1, 5, 2, 9));
	ASSERT_TRUE(r.has_value()) << r.error().message;
	EXPECT_EQ(store.Row(0, 0)[2].i, 9);
}

TEST(RelationStore, RetractKeepsAnOrderedTableSorted)
{
	relation_store store = MakeOrderedStore(false);
	ASSERT_TRUE(store.Apply(Sorted(1, 4, 0, 0)).has_value());
	ASSERT_TRUE(store.Apply(Sorted(2, 2, 0, 0)).has_value());
	ASSERT_TRUE(store.Apply(Sorted(1, 3, 0, 0)).has_value());
	ASSERT_TRUE(store.Apply(Sorted(2, 1, 0, 0)).has_value());

	ASSERT_TRUE(store.Apply(retract_owner{2}).has_value());

	ASSERT_EQ(store.RowCount(0), 2u);
	EXPECT_EQ(store.Row(0, 0)[0].i, 3);
	EXPECT_EQ(store.Row(0, 1)[0].i, 4);
}

TEST(RelationStore, MovedStoreKeepsItsRows)
{
	relation_store store = MakeStore();
	ASSERT_TRUE(store.Apply(Spawn(1, 10, 0, 0, 0)).has_value());
	relation_store moved(std::move(store));
	EXPECT_EQ(moved.RowCount(0), 1u);
	EXPECT_EQ(moved.Row(0, 0)[0].i, 10);
}
```

Add `${SPEHLEON_ROOT}/Tabular/test/relation_store_gtest.cpp` to `cmake/sources_tests.cmake` after the schema test.

- [ ] **Step 2: Run the build to verify the tests fail**

Run: `env -C $WT PWD=$WT scripts/wt-build.sh`
Expected: a link failure, with undefined references to `relation_store::relation_store`, `Apply` and the rest.

- [ ] **Step 3: Implement the store**

Append to `Spehleon/Tabular/src/relation.cpp`, adding `#include <unordered_map>` and `#include <algorithm>` with the other includes:

```cpp
namespace tabular
{

struct relation_store::Storage
{
	struct table_rows
	{
		std::vector<cell>     cells;    // row-major, stride = column count
		std::vector<uint64_t> owners;   // one per row
		uint64_t              generation = 0;
		// One map per column; only key columns' maps are filled (Task 3).
		std::vector<std::unordered_map<int64_t, std::vector<uint32_t>>> index;
	};

	schema                  definition;
	std::vector<table_rows> tables;
};

namespace
{

// A flag set is AngelScript uint32; anything outside it could not round-trip.
bool FitsFlags(int64_t v)
{
	return v >= 0 && v <= INT64_C(0xFFFFFFFF);
}

// Negative, zero or positive as the row sorts before, with or after key, over
// the first key.size() order columns. Order columns are never doubles, so
// every compared cell is an integer.
int CompareOrder(std::span<cell const> row, std::span<uint32_t const> order, std::span<cell const> key)
{
	for (size_t k = 0; k < key.size(); ++k)
	{
		int64_t a = row[order[k]].i;
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
	std::span<uint32_t const> order, std::span<cell const> key, bool upper)
{
	size_t first = 0;
	size_t count = rowCount;
	while (count > 0)
	{
		size_t half = count / 2;
		size_t mid  = first + half;
		int    cmp  = CompareOrder(cells.subspan(mid * stride, stride), order, key);
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

relation_store::relation_store(schema&& definition)
	: _storage(std::make_unique<Storage>())
{
	_storage->definition = std::move(definition);
	auto tables = _storage->definition.tables();
	_storage->tables.resize(tables.size());
	for (size_t t = 0; t < tables.size(); ++t)
	{
		_storage->tables[t].index.resize(tables[t].columns.size());
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

std::span<cell const> relation_store::Row(uint32_t table, size_t row) const
{
	auto const& rows   = _storage->tables[table];
	size_t      stride = _storage->definition.tables()[table].columns.size();
	return std::span<cell const>(rows.cells).subspan(row * stride, stride);
}

uint64_t relation_store::OwnerOf(uint32_t table, size_t row) const
{
	return _storage->tables[table].owners[row];
}

uint64_t relation_store::Generation(uint32_t table) const
{
	return _storage->tables[table].generation;
}

tl::expected<void, ErrorContext> relation_store::Apply(row_command const& command)
{
	if (auto const* insert = std::get_if<insert_row>(&command))
	{
		auto defs = _storage->definition.tables();
		if (insert->table >= defs.size())
		{
			return tl::make_unexpected(ErrorContext("insert into table ", insert->table,
				", but the schema has ", defs.size(), " tables"));
		}
		table_def const& def = defs[insert->table];
		if (insert->values.size() != def.columns.size())
		{
			return tl::make_unexpected(ErrorContext("insert into '", def.name, "' has ",
				insert->values.size(), " values, but the table has ", def.columns.size(), " columns"));
		}
		for (size_t c = 0; c < def.columns.size(); ++c)
		{
			if (def.columns[c].type == column_type::Flags && !FitsFlags(insert->values[c].i))
			{
				return tl::make_unexpected(ErrorContext("column '", def.columns[c].name, "' of '", def.name,
					"' is a flag set and holds a uint32; got ", insert->values[c].i));
			}
		}

		auto&  rows   = _storage->tables[insert->table];
		size_t stride = def.columns.size();
		size_t count  = rows.owners.size();
		size_t pos    = count;
		if (!def.order.empty())
		{
			std::vector<cell> key;
			key.reserve(def.order.size());
			for (uint32_t c : def.order)
			{
				key.push_back(insert->values[c]);
			}
			size_t lower = OrderBound(rows.cells, count, stride, def.order, key, false);
			pos          = OrderBound(rows.cells, count, stride, def.order, key, true);
			if (def.unique && lower != pos)
			{
				return tl::make_unexpected(ErrorContext("table '", def.name, "' has a unique order, and owner ",
					rows.owners[lower], " already has a row with this key"));
			}
		}

		rows.cells.insert(rows.cells.begin() + pos * stride, insert->values.begin(), insert->values.end());
		rows.owners.insert(rows.owners.begin() + pos, insert->owner);
		auto row = static_cast<uint32_t>(pos);
		for (size_t c = 0; c < stride; ++c)
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
			auto& list = rows.index[c][insert->values[c].i];
			list.insert(std::lower_bound(list.begin(), list.end(), row), row);
		}
		++rows.generation;
		return {};
	}

	auto const& retract = std::get<retract_owner>(command);
	auto        defs    = _storage->definition.tables();
	for (size_t t = 0; t < defs.size(); ++t)
	{
		auto&  rows   = _storage->tables[t];
		size_t stride = defs[t].columns.size();
		size_t kept   = 0;
		for (size_t r = 0; r < rows.owners.size(); ++r)
		{
			if (rows.owners[r] == retract.owner)
			{
				continue;
			}
			if (kept != r)
			{
				std::copy_n(rows.cells.begin() + r * stride, stride, rows.cells.begin() + kept * stride);
				rows.owners[kept] = rows.owners[r];
			}
			++kept;
		}
		if (kept == rows.owners.size())
		{
			// Nothing of this owner's here: the rows did not change, so the
			// generation must not either.
			continue;
		}
		rows.owners.resize(kept);
		rows.cells.resize(kept * stride);
		// A retract happens on reload, not per frame, so rebuilding the key
		// indexes is cheaper to get right than patching shifted row numbers.
		for (size_t c = 0; c < stride; ++c)
		{
			rows.index[c].clear();
			if (!defs[t].columns[c].key)
			{
				continue;
			}
			for (size_t r = 0; r < kept; ++r)
			{
				rows.index[c][rows.cells[r * stride + c].i].push_back(static_cast<uint32_t>(r));
			}
		}
		++rows.generation;
	}
	return {};
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
```

- [ ] **Step 4: Build and run the tests to verify they pass**

Run: `env -C $WT PWD=$WT scripts/wt-build.sh && $WT/Build/Engine --gtest --gtest_filter='Relation*'`
Expected: 14 `RelationSchema` and 15 `RelationStore` tests PASS.

- [ ] **Step 5: Commit (coordinator), leaves first**

```bash
git -C $WT/Spehleon/Tabular add src/relation.cpp test/relation_store_gtest.cpp
git -C $WT/Spehleon/Tabular commit -m "Relations: store rows by owner; retract and generation"
git -C $WT/Spehleon add Tabular && git -C $WT/Spehleon commit -m "Bump Tabular: relation store"
git -C $WT add Spehleon cmake/sources_tests.cmake && git -C $WT commit -m "Test the Tabular relation store"
```

Each message ends with the session's attribution lines.

---

### Task 3: key lookup, and spawn points end to end

> **Amended 2026-10-15 (owner header review).** The header in Task 1 is authoritative and changes this task's interface: no `row_command` variant (`Insert(insert_row const&) -> tl::expected<void, ErrorContext>` and `Retract(retract_owner const&) -> void`); `insert_row` carries views (`values` for value columns only, `conditions` adopted closures, `commands` copied text); `Row()` is replaced by `Value(table, row, column)`, plus `Holds(...)` and `CommandAt(...)` for Condition and Command columns. Where the code below conflicts with the header, the header wins; the executor ledgers each adaptation as a ruling. Tests for closure adoption (released exactly once on retract, on refusal and on destruction) and for `Holds` passing `this_in` and reporting `error_out` are added here.

**Files:**
- Modify: `Spehleon/Tabular/src/relation.cpp` (replace the `Find` stub)
- Create: `Spehleon/Tabular/test/relation_find_gtest.cpp`
- Modify: `cmake/sources_tests.cmake` (add the new test)

**Interfaces:**
- Consumes: `relation_store::Storage::table_rows::index` as filled by Task 2's `Apply`.
- Produces: `relation_store::Find(uint32_t table, uint32_t column, cell value) const -> tl::expected<std::vector<size_t>, ErrorContext>` and `relation_store::FindOrdered(uint32_t table, std::span<cell const> prefix) const -> tl::expected<row_range, ErrorContext>`. `FindOrdered` uses Task 2's `OrderBound`.

- [ ] **Step 1: Write the failing tests**

`Spehleon/Tabular/test/relation_find_gtest.cpp`:

```cpp
#include <gtest/gtest.h>
#include "tabular/relation.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace tabular;

// The message mentions the text: errors must name what they refuse.
static bool Mentions(std::string const& message, std::string_view text)
{
	return message.find(text) != std::string::npos;
}

namespace
{

relation_store SpawnPoints()
{
	std::vector<schema_command> commands = {
		create_table{"spawn_points"},
		add_column{0, "package", column_type::Hash,   true},
		add_column{0, "section", column_type::Hash,   true},
		add_column{0, "x",       column_type::Double, false},
		add_column{0, "y",       column_type::Double, false},
	};
	auto s = BuildSchema(commands);
	EXPECT_TRUE(s.has_value());
	return relation_store(std::move(*s));
}

insert_row Spawn(uint64_t owner, int64_t package, int64_t section, double x, double y)
{
	return insert_row{0, owner, {cell::Integer(package), cell::Integer(section), cell::Real(x), cell::Real(y)}};
}

}

TEST(RelationFind, FindsEveryRowWithTheKeyInRowOrder)
{
	relation_store store = SpawnPoints();
	ASSERT_TRUE(store.Apply(Spawn(1, 10, 500, 0, 0)).has_value());
	ASSERT_TRUE(store.Apply(Spawn(1, 10, 600, 0, 0)).has_value());
	ASSERT_TRUE(store.Apply(Spawn(2, 20, 500, 0, 0)).has_value());

	auto bySection = store.Find(0, 1, cell::Integer(500));
	ASSERT_TRUE(bySection.has_value()) << bySection.error().message;
	EXPECT_EQ(*bySection, (std::vector<size_t>{0, 2}));

	auto byPackage = store.Find(0, 0, cell::Integer(10));
	ASSERT_TRUE(byPackage.has_value());
	EXPECT_EQ(*byPackage, (std::vector<size_t>{0, 1}));
}

TEST(RelationFind, MissingKeyFindsNothing)
{
	relation_store store = SpawnPoints();
	ASSERT_TRUE(store.Apply(Spawn(1, 10, 500, 0, 0)).has_value());
	auto found = store.Find(0, 1, cell::Integer(999));
	ASSERT_TRUE(found.has_value());
	EXPECT_TRUE(found->empty());
}

TEST(RelationFind, NonKeyColumnIsRefused)
{
	relation_store store = SpawnPoints();
	auto found = store.Find(0, 2, cell::Real(0));
	ASSERT_FALSE(found.has_value());
	EXPECT_TRUE(Mentions(found.error().message, "'x'")) << found.error().message;
}

TEST(RelationFind, OutOfRangeTableOrColumnIsRefused)
{
	relation_store store = SpawnPoints();
	EXPECT_FALSE(store.Find(3, 0, cell::Integer(0)).has_value());
	EXPECT_FALSE(store.Find(0, 9, cell::Integer(0)).has_value());
}

TEST(RelationFind, FindAfterRetractReturnsNewPositions)
{
	relation_store store = SpawnPoints();
	ASSERT_TRUE(store.Apply(Spawn(1, 10, 500, 0, 0)).has_value());
	ASSERT_TRUE(store.Apply(Spawn(2, 20, 500, 0, 0)).has_value());
	ASSERT_TRUE(store.Apply(Spawn(3, 30, 500, 0, 0)).has_value());

	ASSERT_TRUE(store.Apply(retract_owner{1}).has_value());

	auto found = store.Find(0, 1, cell::Integer(500));
	ASSERT_TRUE(found.has_value());
	EXPECT_EQ(*found, (std::vector<size_t>{0, 1}));
	EXPECT_EQ(store.Row(0, (*found)[0])[0].i, 20);
	EXPECT_EQ(store.Row(0, (*found)[1])[0].i, 30);
}

TEST(RelationFind, ExtremeValuesLookUpAndRetract)
{
	relation_store store = SpawnPoints();
	int64_t const highBitHash = INT64_C(0x80000000);   // a CityHash32 with bit 31 set
	int64_t const negative    = -5;
	ASSERT_TRUE(store.Apply(Spawn(UINT64_MAX, highBitHash, negative, 0, 0)).has_value());
	ASSERT_TRUE(store.Apply(Spawn(1, 7, 7, 0, 0)).has_value());

	auto byHash = store.Find(0, 0, cell::Integer(highBitHash));
	ASSERT_TRUE(byHash.has_value());
	EXPECT_EQ(*byHash, (std::vector<size_t>{0}));
	auto byNegative = store.Find(0, 1, cell::Integer(negative));
	ASSERT_TRUE(byNegative.has_value());
	EXPECT_EQ(*byNegative, (std::vector<size_t>{0}));
	EXPECT_EQ(store.OwnerOf(0, 0), UINT64_MAX);

	ASSERT_TRUE(store.Apply(retract_owner{UINT64_MAX}).has_value());
	EXPECT_EQ(store.RowCount(0), 1u);
	auto gone = store.Find(0, 0, cell::Integer(highBitHash));
	ASSERT_TRUE(gone.has_value());
	EXPECT_TRUE(gone->empty());
}

TEST(RelationFind, KeyLookupIsRightAfterAnInsertInTheMiddle)
{
	std::vector<schema_command> commands = {
		create_table{"t"},
		add_column{0, "rank", column_type::Int, false},
		add_column{0, "tag",  column_type::Hash, true},
		order_by{0, {0}, false},
	};
	auto s = BuildSchema(commands);
	ASSERT_TRUE(s.has_value()) << s.error().message;
	relation_store store(std::move(*s));
	ASSERT_TRUE(store.Apply(insert_row{0, 1, {cell::Integer(10), cell::Integer(7)}}).has_value());
	ASSERT_TRUE(store.Apply(insert_row{0, 1, {cell::Integer(30), cell::Integer(7)}}).has_value());
	ASSERT_TRUE(store.Apply(insert_row{0, 1, {cell::Integer(20), cell::Integer(7)}}).has_value());

	auto found = store.Find(0, 1, cell::Integer(7));
	ASSERT_TRUE(found.has_value());
	EXPECT_EQ(*found, (std::vector<size_t>{0, 1, 2}));
	EXPECT_EQ(store.Row(0, 1)[0].i, 20);
}

namespace
{

// spawn points ordered by (section, package)
relation_store OrderedSpawnPoints()
{
	std::vector<schema_command> commands = {
		create_table{"spawn_points"},
		add_column{0, "package", column_type::Hash,   true},
		add_column{0, "section", column_type::Hash,   true},
		add_column{0, "x",       column_type::Double, false},
		add_column{0, "y",       column_type::Double, false},
		order_by{0, {1, 0}, false},
	};
	auto s = BuildSchema(commands);
	EXPECT_TRUE(s.has_value());
	return relation_store(std::move(*s));
}

}

TEST(RelationFind, OrderedPrefixFindsAContiguousRange)
{
	relation_store store = OrderedSpawnPoints();
	ASSERT_TRUE(store.Apply(Spawn(1, 10, 600, 0, 0)).has_value());
	ASSERT_TRUE(store.Apply(Spawn(1, 10, 500, 0, 0)).has_value());
	ASSERT_TRUE(store.Apply(Spawn(2, 20, 500, 0, 0)).has_value());
	ASSERT_TRUE(store.Apply(Spawn(2, 20, 700, 0, 0)).has_value());

	cell meadow[] = {cell::Integer(500)};
	auto inMeadow = store.FindOrdered(0, meadow);
	ASSERT_TRUE(inMeadow.has_value()) << inMeadow.error().message;
	EXPECT_EQ(inMeadow->first, 0u);
	EXPECT_EQ(inMeadow->last, 2u);

	cell exact[] = {cell::Integer(500), cell::Integer(20)};
	auto one = store.FindOrdered(0, exact);
	ASSERT_TRUE(one.has_value());
	EXPECT_EQ(one->last - one->first, 1u);
	EXPECT_EQ(store.Row(0, one->first)[0].i, 20);

	cell nowhere[] = {cell::Integer(550)};
	auto none = store.FindOrdered(0, nowhere);
	ASSERT_TRUE(none.has_value());
	EXPECT_EQ(none->first, none->last);
}

TEST(RelationFind, EmptyPrefixMatchesEveryRow)
{
	relation_store store = OrderedSpawnPoints();
	ASSERT_TRUE(store.Apply(Spawn(1, 10, 600, 0, 0)).has_value());
	ASSERT_TRUE(store.Apply(Spawn(1, 10, 500, 0, 0)).has_value());
	auto all = store.FindOrdered(0, {});
	ASSERT_TRUE(all.has_value());
	EXPECT_EQ(all->first, 0u);
	EXPECT_EQ(all->last, 2u);
}

TEST(RelationFind, BadOrderedLookupsAreRefused)
{
	relation_store unordered = SpawnPoints();
	cell key[] = {cell::Integer(1)};
	auto noOrder = unordered.FindOrdered(0, key);
	ASSERT_FALSE(noOrder.has_value());
	EXPECT_TRUE(Mentions(noOrder.error().message, "no order")) << noOrder.error().message;

	relation_store ordered = OrderedSpawnPoints();
	cell tooLong[] = {cell::Integer(1), cell::Integer(2), cell::Integer(3)};
	auto longer = ordered.FindOrdered(0, tooLong);
	ASSERT_FALSE(longer.has_value());
	EXPECT_TRUE(Mentions(longer.error().message, "3")) << longer.error().message;

	EXPECT_FALSE(ordered.FindOrdered(5, key).has_value());
}

// The owner's example: a package contributes spawn points, is reloaded with
// different ones, and a consumer notices through the generation and re-reads.
TEST(RelationFind, SpawnPointsSurvivePackageReload)
{
	relation_store store = SpawnPoints();
	uint64_t const grazers = 1, plants = 2;
	int64_t const  meadow  = 500;

	ASSERT_TRUE(store.Apply(Spawn(grazers, 10, meadow, 1.0, 2.0)).has_value());
	ASSERT_TRUE(store.Apply(Spawn(grazers, 10, meadow, 3.0, 4.0)).has_value());
	ASSERT_TRUE(store.Apply(Spawn(plants,  20, meadow, 5.0, 6.0)).has_value());
	uint64_t seen = store.Generation(0);

	// Reload the grazer package with one spawn point instead of two.
	ASSERT_TRUE(store.Apply(retract_owner{grazers}).has_value());
	ASSERT_TRUE(store.Apply(Spawn(grazers, 10, meadow, 9.0, 9.5)).has_value());
	EXPECT_NE(store.Generation(0), seen);

	auto inMeadow = store.Find(0, 1, cell::Integer(meadow));
	ASSERT_TRUE(inMeadow.has_value());
	ASSERT_EQ(inMeadow->size(), 2u);
	EXPECT_EQ(store.OwnerOf(0, (*inMeadow)[0]), plants);
	EXPECT_EQ(store.Row(0, (*inMeadow)[0])[2].d, 5.0);
	EXPECT_EQ(store.OwnerOf(0, (*inMeadow)[1]), grazers);
	EXPECT_EQ(store.Row(0, (*inMeadow)[1])[2].d, 9.0);
	EXPECT_EQ(store.Row(0, (*inMeadow)[1])[3].d, 9.5);
}
```

Add `${SPEHLEON_ROOT}/Tabular/test/relation_find_gtest.cpp` to `cmake/sources_tests.cmake` after the store test.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `env -C $WT PWD=$WT scripts/wt-build.sh && $WT/Build/Engine --gtest --gtest_filter='RelationFind.*'`
Expected: the lookup tests FAIL with "relation_store::Find is not implemented yet".

- [ ] **Step 3: Implement `Find`**

Replace both stubs in `Spehleon/Tabular/src/relation.cpp` with:

```cpp
tl::expected<std::vector<size_t>, ErrorContext> relation_store::Find(uint32_t table, uint32_t column, cell value) const
{
	auto defs = _storage->definition.tables();
	if (table >= defs.size())
	{
		return tl::make_unexpected(ErrorContext("no table ", table));
	}
	table_def const& def = defs[table];
	if (column >= def.columns.size())
	{
		return tl::make_unexpected(ErrorContext("table '", def.name, "' has no column ", column));
	}
	if (!def.columns[column].key)
	{
		return tl::make_unexpected(ErrorContext("column '", def.columns[column].name, "' of '", def.name,
			"' is not a key, so it cannot be looked up"));
	}

	std::vector<size_t> out;
	auto const& index = _storage->tables[table].index[column];
	auto        found = index.find(value.i);
	if (found != index.end())
	{
		out.assign(found->second.begin(), found->second.end());
	}
	return out;
}

tl::expected<row_range, ErrorContext> relation_store::FindOrdered(uint32_t table, std::span<cell const> prefix) const
{
	auto defs = _storage->definition.tables();
	if (table >= defs.size())
	{
		return tl::make_unexpected(ErrorContext("no table ", table));
	}
	table_def const& def = defs[table];
	if (def.order.empty())
	{
		return tl::make_unexpected(ErrorContext("table '", def.name, "' has no order to look up by"));
	}
	if (prefix.size() > def.order.size())
	{
		return tl::make_unexpected(ErrorContext("a prefix of ", prefix.size(), " values for table '", def.name,
			"', whose order has ", def.order.size(), " columns"));
	}
	auto const& rows   = _storage->tables[table];
	size_t      stride = def.columns.size();
	size_t      count  = rows.owners.size();
	return row_range{
		OrderBound(rows.cells, count, stride, def.order, prefix, false),
		OrderBound(rows.cells, count, stride, def.order, prefix, true),
	};
}
```

- [ ] **Step 4: Build and run all relation tests**

Run: `env -C $WT PWD=$WT scripts/wt-build.sh && $WT/Build/Engine --gtest --gtest_filter='Relation*'`
Expected: 14 `RelationSchema`, 15 `RelationStore` and 11 `RelationFind` tests PASS, 40 in total.

- [ ] **Step 5: Show that a test can fail**

Temporarily change `++rows.generation;` in the insert path to nothing. Rebuild (touch the file so Ninja recompiles) and run `--gtest_filter='Relation*'`. Expected: `InsertChangesOnlyItsTablesGeneration` and `SpawnPointsSurvivePackageReload` FAIL. Restore the line, `touch` the file, rebuild and confirm all 40 pass again.

- [ ] **Step 6: Run the full suite once**

Run: `env -u GTEST_FILTER $WT/Build/Engine --gtest`
Expected: report the actual pass, fail and skip counts. Compare any failure with `known-master-reds-index.md` and the main tree before attributing it to this branch.

- [ ] **Step 7: Commit (coordinator), leaves first**

```bash
git -C $WT/Spehleon/Tabular add src/relation.cpp test/relation_find_gtest.cpp
git -C $WT/Spehleon/Tabular commit -m "Relations: equality lookup on key columns"
git -C $WT/Spehleon add Tabular && git -C $WT/Spehleon commit -m "Bump Tabular: key lookup"
git -C $WT add Spehleon cmake/sources_tests.cmake && git -C $WT commit -m "Test Tabular key lookup and spawn-point reload"
```

Each message ends with the session's attribution lines.

---

## Out of scope for this plan

- The YAML front end (step 2), the CSV front end and read-only tables (step 3), and machines (step 4).
- RAII contribution handles (definition, Lifetimes) and the budget benchmarks. Both are wanted. Neither is needed to prove the command core, and the budgets need a measured baseline first.
- Removing the old engine `src/Tabular`. It keeps building beside the new namespace.
