# Relations: the column set — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every Tabular column type (adding `string` and `vec2`, and condition cells in read-only tables) can be declared in YAML, filled from CSV, held by a read-only version and read back.

**Architecture:** `vec2` is two floats in the existing 8-byte `cell` union, so it is an ordinary value column. Strings get a fourth per-table slot array of interned `counted_string`, beside cells, conditions and commands, with a content-keyed index for string key columns. Every condition is wrapped in a refcounted `HeldCondition`, for writable and read-only tables alike, so a survivor shared by two versions releases its closure exactly once; `Load` then accepts conditions, and `LoadCsv` builds them through a caller-supplied function.

**Tech Stack:** C++20, `tl::expected`, `ErrorContext`, `counted_ptr` / `counted_string` (Spehleon/lib/Support), the Spehleon YAML walker, `CSV::File`, GoogleTest compiled into the Engine binary.

**Spec:** `Spehleon/Tabular/docs/superpowers/specs/2026-10-16-relations-column-set-design.md`. The contract is `Spehleon/Tabular/docs/definition.md`.

## Global Constraints

- Work in a worktree: `scripts/wt-create.sh tabular-columns` from the superproject root; `WT=/mnt/Passport/Worktrees/tabular-columns`. Never commit on the main tree.
- Build: `env -C $WT PWD=$WT scripts/wt-build.sh > $LOG 2>&1` (the wrapper resolves the tree from `$PWD`). Run tests **from `$WT`**: `cd $WT && $WT/Build/Engine --gtest --gtest_filter='<filter>'` (relative test data paths break from any other directory).
- The tabular filter used throughout: `FILTER='Relation*:RowsCsv.*:SchemaYaml.*'`.
- Never build while a test run or a subagent is live. After restoring a file from a mutation, `touch` it so Ninja rebuilds.
- Commit leaves first: `Spehleon/Tabular`, then `Spehleon` (gitlink), then the superproject (gitlink). Commit messages end with the session's attribution lines.
- Always brace control-flow bodies. Comments say why, not what.
- Errors are returned as `ErrorContext`, never logged; no `LOG_F(ERROR)` / `LOG_F(FATAL)`.
- A `snake_case` type is never `new`ed; a heap type is `PascalCase`. Adopt with `UncountedWrap(new T(...))`, never `counted_ptr<T>(new T())`. Refcounts are atomic.
- Big parameters by `const&` or `&&`; small trivially-copyable values (spans, `cell`, `string_in`) by value.
- Header dependencies: include what a header needs for by-value members and inline code; implementation-only includes go in the `.cpp`.

## Review Focus

1. **A refused reload must not touch survivors.** A load refused for a unique clash releases the closures it carried and leaves the current version's closures alive. Test: `RelationLoadConditions.ARefusedReloadReleasesOnlyItsOwnClosures` (Task 1).
2. **String key indexes after an ordered insert shifts rows.** A writable table ordered by another column, whose inserts land mid-table, must still find each string key at its new row. Test: `RelationColumns.StringKeyFollowsRowsShiftedByAnOrderedInsert` (Task 3).
3. **The empty string.** `string_in("")` is a value like any other: it is stored and found by `Find(column, "")`. Test: `RelationColumns.EmptyStringIsStoredAndFound` (Task 3).
4. **Negative and exponent `vec2` components.** `-1..-0.5` and `1e3..2` are ordinary floats and load; `1 .. 2` (spaces inside) is refused, because only a cell's ends are trimmed. Test: `RowsCsv.Vec2AcceptsSignsAndExponentsAndRefusesInnerSpaces` (Task 2).
5. **A condition function returning a closure with no function** is refused by `Load`, and `LoadCsv` names its line. Test: `RowsCsv.ANullConditionNamesItsLine` (Task 4).

---

## File Structure

| File | Change |
|---|---|
| `Spehleon/Tabular/include/tabular/relation.h` | `column_type::Vec2`, `column_type::String`; `cell::v`, `cell::Vector`; `string_in`; `strings` on `insert_row`; `conditions` and `strings` on `load_rows`; `TableVersion::Holds`, `StringAt`, string `Find`; store `StringAt`, string `Find` |
| `Spehleon/Tabular/src/relation.cpp` | `HeldCondition`; string slots, interning and index; `Load` / `Retract` / `Insert` changes; schema refusals; readers |
| `Spehleon/Tabular/include/tabular/rows_csv.h`, `src/rows_csv.cpp` | `vec2` and string cells; `csv_rows::condition` |
| `Spehleon/Tabular/src/schema_yaml.cpp` | type names `vec2`, `string` |
| `Spehleon/Tabular/test/probe_closure.h` (new) | the counting closure, shared by tests |
| `Spehleon/Tabular/test/relation_load_conditions_gtest.cpp` (new) | Task 1 |
| `Spehleon/Tabular/test/relation_columns_gtest.cpp` (new) | Tasks 2 and 3, store level |
| `Spehleon/Tabular/test/relation_store_gtest.cpp`, `rows_csv_gtest.cpp`, `schema_yaml_gtest.cpp` | probe header; CSV and YAML tests |
| `cmake/sources_tests.cmake` (superproject) | register the two new test files |
| `Spehleon/Tabular/docs/definition.md`, `docs/feature-checklist.md`, the spec | Task 5 |

Aggregate initialisers in existing code and tests are positional (`insert_row{0, 7, values, {}, commands}`, `load_rows{t, owner, 0, {}, {}}`, `load_rows{..., &refusal}`), so **every new field goes at the end of its struct**.

---

### Task 1: Held conditions; `Load` accepts conditions

**Files:**
- Create: `Spehleon/Tabular/test/probe_closure.h`, `Spehleon/Tabular/test/relation_load_conditions_gtest.cpp`
- Modify: `Spehleon/Tabular/include/tabular/relation.h`, `Spehleon/Tabular/src/relation.cpp`, `Spehleon/Tabular/test/relation_store_gtest.cpp`, `cmake/sources_tests.cmake`

**Interfaces:**
- Produces: `load_rows::conditions` (`std::span<closure const>`, after `refusal`); `TableVersion::Holds(size_t row, uint32_t column, void const* this_in) const -> tl::expected<bool, ErrorContext>`; test helper `tabular_test::MakeProbe(int64_t threshold, int* released) -> closure` in `test/probe_closure.h`.

- [ ] **Step 1: Move the probe closure into a shared test header**

Create `test/probe_closure.h`:

```cpp
#ifndef TABULAR_TEST_PROBE_CLOSURE_H
#define TABULAR_TEST_PROBE_CLOSURE_H

#include "tabular/relation.h"

#include <cstdint>

namespace tabular_test
{

// A condition's captured state: the threshold it compares with, and a count
// of releases, so a test can see each closure released exactly once.
struct Probe
{
	int64_t threshold = 0;
	int*    released  = nullptr;
};

// this_in is an int64_t; the row counts when it reaches the threshold. A
// negative this_in is an error the closure reports.
inline bool AtLeast(void* data, void const* this_in, ErrorContext* error_out)
{
	auto const* p     = static_cast<Probe const*>(data);
	int64_t     value = *static_cast<int64_t const*>(this_in);
	if (value < 0)
	{
		*error_out = ErrorContext("negative input ", value);
		return false;
	}
	return value >= p->threshold;
}

inline void ReleaseProbe(void* data)
{
	auto* p = static_cast<Probe*>(data);
	++*p->released;
	delete p;
}

inline tabular::closure MakeProbe(int64_t threshold, int* released)
{
	return tabular::closure{&AtLeast, new Probe{threshold, released}, &ReleaseProbe};
}

}

#endif // TABULAR_TEST_PROBE_CLOSURE_H
```

In `test/relation_store_gtest.cpp`, delete its own `Probe`, `AtLeast`, `ReleaseProbe` and `MakeProbe` (the block under `// ---- Conditions and commands`), add `#include "probe_closure.h"` after `#include "tabular/relation.h"`, and add `using namespace tabular_test;` after `using namespace tabular;`.

- [ ] **Step 2: Write the failing tests**

Create `test/relation_load_conditions_gtest.cpp`:

```cpp
#include <gtest/gtest.h>
#include "tabular/relation.h"
#include "probe_closure.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace tabular;
using namespace tabular_test;

static bool Mentions(std::string const& message, std::string_view text)
{
	return message.find(text) != std::string::npos;
}

namespace
{

// rules(priority int, when condition, then command), read-only, ordered by priority.
relation_store MakeRules(bool unique)
{
	static add_column const columns[] = {
		{"priority", column_type::Int,       false},
		{"when",     column_type::Condition, false},
		{"then",     column_type::Command,   false},
	};
	static uint32_t const order[] = {0};
	order_by const ordering{order, unique};
	create_table const table{"rules", columns, &ordering, true};
	auto s = BuildSchema(std::span<create_table const>(&table, 1));
	EXPECT_TRUE(s.has_value()) << s.error().message;
	return relation_store(std::move(*s));
}

// Loads (priority, condition) rows for owner; every command is "(act)".
tl::expected<void, ErrorContext> LoadRules(relation_store& store, uint64_t owner,
	std::vector<std::pair<int64_t, closure>> const& rows, load_refusal* refusal = nullptr)
{
	std::vector<cell>             values;
	std::vector<closure>          conditions;
	std::vector<std::string_view> commands;
	for (auto const& [priority, when] : rows)
	{
		values.push_back(cell::Integer(priority));
		conditions.push_back(when);
		commands.push_back("(act)");
	}
	return store.Load(load_rows{0, owner, rows.size(), values, commands, refusal, conditions});
}

}

TEST(RelationLoadConditions, ALoadedConditionIsCalledWithThisIn)
{
	int released = 0;
	{
		relation_store store = MakeRules(false);
		ASSERT_TRUE(LoadRules(store, 1, {{10, MakeProbe(5, &released)}}).has_value());
		int64_t low = 4, high = 5;
		EXPECT_FALSE(*store.Holds(0, 0, 1, &low));
		EXPECT_TRUE(*store.Holds(0, 0, 1, &high));
		EXPECT_EQ(released, 0);
	}
	EXPECT_EQ(released, 1);
}

TEST(RelationLoadConditions, ASurvivorSharedByTwoVersionsIsReleasedOnce)
{
	int survivor = 0, replaced = 0, added = 0;
	{
		relation_store store = MakeRules(false);
		ASSERT_TRUE(LoadRules(store, 1, {{10, MakeProbe(1, &survivor)}}).has_value());
		ASSERT_TRUE(LoadRules(store, 2, {{20, MakeProbe(1, &replaced)}}).has_value());
		auto before = store.Version(0);
		ASSERT_TRUE(before.has_value());
		ASSERT_TRUE(LoadRules(store, 2, {{30, MakeProbe(1, &added)}}).has_value());
		EXPECT_EQ(replaced, 0) << "the held version still has owner 2's old row";
		*before = nullptr;
		EXPECT_EQ(replaced, 1);
		EXPECT_EQ(survivor, 0) << "the current version still holds owner 1's row";
	}
	EXPECT_EQ(survivor, 1);
	EXPECT_EQ(added, 1);
}

TEST(RelationLoadConditions, ARefusedLoadReleasesItsClosures)
{
	int released = 0;
	relation_store store = MakeRules(false);
	cell const    values[]     = {cell::Integer(1)};
	closure const conditions[] = {MakeProbe(1, &released), MakeProbe(1, &released)};   // two for one row
	std::string_view const commands[] = {"(act)"};
	auto loaded = store.Load(load_rows{0, 1, 1, values, commands, nullptr, conditions});
	ASSERT_FALSE(loaded.has_value());
	EXPECT_TRUE(Mentions(loaded.error().message, "condition")) << loaded.error().message;
	EXPECT_EQ(released, 2);
	EXPECT_EQ(store.RowCount(0), 0u);
}

TEST(RelationLoadConditions, AConditionWithNoFunctionIsRefusedAndNamesItsRow)
{
	int released = 0;
	relation_store store = MakeRules(false);
	load_refusal refusal;
	auto loaded = LoadRules(store, 1, {{1, MakeProbe(1, &released)}, {2, closure{}}}, &refusal);
	ASSERT_FALSE(loaded.has_value());
	EXPECT_EQ(refusal.row, 1u);
	EXPECT_EQ(released, 1);
}

TEST(RelationLoadConditions, ARefusedReloadReleasesOnlyItsOwnClosures)
{
	int kept = 0, refused = 0;
	relation_store store = MakeRules(true);
	ASSERT_TRUE(LoadRules(store, 1, {{10, MakeProbe(1, &kept)}}).has_value());
	auto clash = LoadRules(store, 2, {{10, MakeProbe(1, &refused)}});
	ASSERT_FALSE(clash.has_value());
	EXPECT_EQ(refused, 1);
	EXPECT_EQ(kept, 0);
	int64_t five = 5;
	EXPECT_TRUE(*store.Holds(0, 0, 1, &five));
}

TEST(RelationLoadConditions, RetractClearsAConditionTable)
{
	int released = 0;
	relation_store store = MakeRules(false);
	ASSERT_TRUE(LoadRules(store, 1, {{10, MakeProbe(1, &released)}}).has_value());
	store.Retract(retract_owner{1});
	EXPECT_EQ(store.RowCount(0), 0u);
	EXPECT_EQ(released, 1);
}

TEST(RelationLoadConditions, AVersionHeldPastItsStoreReleasesWhenDropped)
{
	int released = 0;
	counted_ptr<TableVersion const> held;
	{
		relation_store store = MakeRules(false);
		ASSERT_TRUE(LoadRules(store, 1, {{10, MakeProbe(1, &released)}}).has_value());
		held = *store.Version(0);
	}
	EXPECT_EQ(released, 0);
	int64_t five = 5;
	EXPECT_TRUE(*held->Holds(0, 1, &five));
	held = nullptr;
	EXPECT_EQ(released, 1);
}

TEST(RelationLoadConditions, HoldsOnAVersionReportsTheClosuresError)
{
	int released = 0;
	relation_store store = MakeRules(false);
	ASSERT_TRUE(LoadRules(store, 1, {{10, MakeProbe(1, &released)}}).has_value());
	auto version = *store.Version(0);
	int64_t negative = -1;
	auto holds = version->Holds(0, 1, &negative);
	ASSERT_FALSE(holds.has_value());
	EXPECT_TRUE(Mentions(holds.error().message, "negative")) << holds.error().message;
	EXPECT_FALSE(version->Holds(0, 0, &negative).has_value()) << "column 0 is not a condition";
}
```

Register it in the superproject's `cmake/sources_tests.cmake`, after the `relation_load_gtest.cpp` line:

```cmake
	${SPEHLEON_ROOT}/Tabular/test/relation_load_conditions_gtest.cpp
```

- [ ] **Step 3: Build and watch it fail**

Run: `env -C $WT PWD=$WT scripts/wt-build.sh > $WT/build.log 2>&1; grep -E 'error:' $WT/build.log | head`
Expected: compile errors in `relation_load_conditions_gtest.cpp`: `load_rows` has no initialiser for a seventh field, and `TableVersion` has no member `Holds`.

- [ ] **Step 4: Header changes**

In `include/tabular/relation.h`, replace the `load_rows` comment and struct:

```cpp
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
```

In `class TableVersion`, after `CommandAt`, add:

```cpp
	tl::expected<bool, ErrorContext>                Holds(size_t row, uint32_t column, void const* this_in) const;
```

Update the `closure` comment's last sentence to: `release frees data and is called once, when no table version or writable table holds the row any longer, or when the row is refused.`

- [ ] **Step 5: `HeldCondition` and the row storage**

In `src/relation.cpp`, add `#include <atomic>` and `#include <iterator>` to the includes. In the second anonymous namespace, rename the free `Release(closure const&)` to `ReleaseClosure` (and its use in `ReleaseAll`), then add after `ReleaseAll`:

```cpp
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
```

In `struct table_rows`, replace `std::vector<closure> closures;   // Condition columns, row-major` with:

```cpp
	std::vector<held_condition> conditions; // Condition columns, row-major
```

Delete `Storage::~Storage()` entirely: holders release their closures when the rows are destroyed.

- [ ] **Step 6: `Insert` and writable `Retract`**

In `Insert`, replace the `rows.closures.insert(...)` statement with:

```cpp
	std::vector<held_condition> held;
	held.reserve(command.conditions.size());
	for (closure const& c : command.conditions)
	{
		held.push_back(Hold(c));
	}
	rows.conditions.insert(rows.conditions.begin() + pos * rows.conditionCount,
		std::make_move_iterator(held.begin()), std::make_move_iterator(held.end()));
```

In `Retract`, replace the read-only branch with:

```cpp
		if (_storage->versions[t])
		{
			// The version without the owner's rows, put in place like any load.
			// Removing rows cannot clash or carry a bad cell, so this load is
			// never refused, and there is nothing to report.
			auto removed = Load(load_rows{t, command.owner, 0, {}, {}});
			(void)removed;
			continue;
		}
```

In the writable compaction loop, delete the `ReleaseAll(...)` line inside `if (rows.owners[r] == command.owner)` (leave the `continue;`), replace the `std::copy_n(rows.closures...)` statement with:

```cpp
				std::move(rows.conditions.begin() + r * rows.conditionCount,
					rows.conditions.begin() + (r + 1) * rows.conditionCount,
					rows.conditions.begin() + kept * rows.conditionCount);
```

and replace `rows.closures.resize(kept * rows.conditionCount);` with `rows.conditions.resize(kept * rows.conditionCount);` (dropping the moved-from tail releases the retracted rows' holders).

- [ ] **Step 7: `Load` accepts conditions**

At the top of `relation_store::Load`, before the table-number check, add:

```cpp
	// Every closure the load carries is ours from here on: a refusal releases
	// them when held goes out of scope, and a success moves them into rows.
	std::vector<held_condition> held;
	held.reserve(command.conditions.size());
	for (closure const& c : command.conditions)
	{
		held.push_back(Hold(c));
	}
```

Delete the `if (current.conditionCount != 0) { ... "a load carries no conditions" ... }` block. After the commands size check, add:

```cpp
	if (!spans(command.conditions.size(), current.conditionCount))
	{
		return tl::make_unexpected(ErrorContext("load into '", def.name, "' of ", command.count, " rows has ",
			command.conditions.size(), " conditions, but the table has ", current.conditionCount, " condition columns"));
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
```

In the loop that builds `next`, after the `next.cells.insert(...)` line, add:

```cpp
		for (uint32_t k = 0; k < next.conditionCount; ++k)
		{
			next.conditions.push_back(s.loaded ? held[s.row * next.conditionCount + k]
			                                   : current.conditions[s.row * next.conditionCount + k]);
		}
```

and add `next.conditions.reserve(order.size() * next.conditionCount);` beside the other `reserve` calls.

- [ ] **Step 8: Shared `Holds` reader**

In the readers' anonymous namespace, add after `ReadCommand`:

```cpp
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
```

Replace the body of `relation_store::Holds` after its table check with `return ReadHolds(_storage->definition.tables()[table], _storage->RowsOf(table), row, column, this_in);`, and add:

```cpp
tl::expected<bool, ErrorContext> TableVersion::Holds(size_t row, uint32_t column, void const* this_in) const
{
	return ReadHolds(_data->def, _data->rows, row, column, this_in);
}
```

- [ ] **Step 9: Build and run**

Run: `env -C $WT PWD=$WT scripts/wt-build.sh > $WT/build.log 2>&1; grep -E 'error:' $WT/build.log | head; cd $WT && $WT/Build/Engine --gtest --gtest_filter="$FILTER" 2>&1 | grep -E '^\[  (FAILED|PASSED) '`
Expected: build clean; `[  PASSED  ]` for every test; no `FAILED`.

- [ ] **Step 10: Prove a test bites**

Mutation: in `Load`, change `held[s.row * next.conditionCount + k]` to `Hold(command.conditions[s.row * next.conditionCount + k])` (a second holder of the same closure). Rebuild and run `--gtest_filter='RelationLoadConditions.*'`.
Expected: a crash or `released` counted 2 (double release) in `ALoadedConditionIsCalledWithThisIn`. Restore the line, `touch src/relation.cpp`, rebuild, rerun: all pass.

- [ ] **Step 11: Commit**

```bash
cd $WT/Spehleon/Tabular && git add include src test && git commit -m "Conditions: held by refcount, so read-only tables load them and survivors release once"
cd $WT/Spehleon && git add Tabular && git commit -m "Bump Tabular: conditions in read-only tables"
cd $WT && git add Spehleon cmake/sources_tests.cmake && git commit -m "Bump Spehleon: conditions in read-only tables; register their tests"
```

---

### Task 2: The `vec2` column

**Files:**
- Create: `Spehleon/Tabular/test/relation_columns_gtest.cpp`
- Modify: `include/tabular/relation.h`, `src/relation.cpp`, `src/schema_yaml.cpp`, `src/rows_csv.cpp`, `include/tabular/rows_csv.h`, `test/rows_csv_gtest.cpp`, `test/schema_yaml_gtest.cpp`, `cmake/sources_tests.cmake`

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces: `column_type::Vec2` (after `Command`); `cell::v` (`float[2]`) and `cell::Vector(float x, float y) -> cell`; YAML type name `vec2`; CSV `a..b`.

- [ ] **Step 1: Write the failing tests**

Create `test/relation_columns_gtest.cpp`:

```cpp
#include <gtest/gtest.h>
#include "tabular/relation.h"

#include <string>
#include <string_view>
#include <vector>

using namespace tabular;

static bool Mentions(std::string const& message, std::string_view text)
{
	return message.find(text) != std::string::npos;
}

namespace
{

tl::expected<schema, ErrorContext> OneTable(std::span<add_column const> columns, order_by const* order,
	bool read_only)
{
	create_table const table{"t", columns, order, read_only};
	return BuildSchema(std::span<create_table const>(&table, 1));
}

}

TEST(RelationColumns, Vec2HoldsTwoFloatsInOneCell)
{
	static_assert(sizeof(cell) == 8);
	cell c = cell::Vector(1.5f, -2.25f);
	EXPECT_EQ(c.v[0], 1.5f);
	EXPECT_EQ(c.v[1], -2.25f);
}

TEST(RelationColumns, Vec2RoundTripsThroughLoad)
{
	add_column const columns[] = {{"id", column_type::Int, false}, {"at", column_type::Vec2, false}};
	auto s = OneTable(columns, nullptr, true);
	ASSERT_TRUE(s.has_value()) << s.error().message;
	relation_store store(std::move(*s));
	cell const values[] = {cell::Integer(1), cell::Vector(0.25f, 0.5f)};
	ASSERT_TRUE(store.Load(load_rows{0, 1, 1, values, {}}).has_value());
	auto at = store.Value(0, 0, 1);
	ASSERT_TRUE(at.has_value());
	EXPECT_EQ(at->v[0], 0.25f);
	EXPECT_EQ(at->v[1], 0.5f);
}

TEST(RelationColumns, Vec2IsNeitherAKeyNorInAnOrder)
{
	add_column const keyed[] = {{"at", column_type::Vec2, true}};
	auto asKey = OneTable(keyed, nullptr, false);
	ASSERT_FALSE(asKey.has_value());
	EXPECT_TRUE(Mentions(asKey.error().message, "vec2")) << asKey.error().message;

	add_column const plain[] = {{"at", column_type::Vec2, false}};
	uint32_t const   first[] = {0};
	order_by const   order{first, false};
	auto inOrder = OneTable(plain, &order, false);
	ASSERT_FALSE(inOrder.has_value());
	EXPECT_TRUE(Mentions(inOrder.error().message, "cannot be in an order")) << inOrder.error().message;
}
```

Register it in `cmake/sources_tests.cmake` after the `relation_load_conditions_gtest.cpp` line:

```cmake
	${SPEHLEON_ROOT}/Tabular/test/relation_columns_gtest.cpp
```

Append to `test/schema_yaml_gtest.cpp`:

```cpp
TEST(SchemaYaml, ReadsTheVec2Type)
{
	auto s = SchemaFromYaml("tables:\n  - name: t\n    columns:\n      - {name: at, type: vec2}\n", "t.yaml");
	ASSERT_TRUE(s.has_value()) << s.error().format();
	EXPECT_EQ(s->tables()[0].columns[0].type, column_type::Vec2);
}
```

Append to `test/rows_csv_gtest.cpp`:

```cpp
namespace
{

constexpr std::string_view kVecSchema = R"(
tables:
  - name: marks
    read_only: true
    columns:
      - {name: id, type: int}
      - {name: at, type: vec2}
)";

relation_store VecStore()
{
	auto s = SchemaFromYaml(kVecSchema, "marks.yaml");
	EXPECT_TRUE(s.has_value()) << s.error().format();
	return relation_store(std::move(*s));
}

std::string LoadMarks(relation_store& store, std::string_view text)
{
	auto loaded = LoadCsv(store, csv_rows{0, 1, text, "marks.csv"});
	return loaded ? std::string() : loaded.error().format();
}

}

TEST(RowsCsv, Vec2IsWrittenAsARange)
{
	relation_store store = VecStore();
	ASSERT_EQ(LoadMarks(store, "id,at\n1,0.25..0.5\n"), "");
	EXPECT_EQ(store.Value(0, 0, 1)->v[0], 0.25f);
	EXPECT_EQ(store.Value(0, 0, 1)->v[1], 0.5f);
}

TEST(RowsCsv, MalformedVec2IsRefusedNamingTheColumn)
{
	for (std::string_view bad : {"1..", "..2", "1..2..3", "1;2", "a..b"})
	{
		relation_store store = VecStore();
		std::string error = LoadMarks(store, std::string("id,at\n1,") + std::string(bad) + "\n");
		EXPECT_TRUE(Mentions(error, "'at'")) << bad << ": " << error;
		EXPECT_TRUE(Mentions(error, "marks.csv:2")) << bad << ": " << error;
	}
}

TEST(RowsCsv, Vec2AcceptsSignsAndExponentsAndRefusesInnerSpaces)
{
	relation_store store = VecStore();
	ASSERT_EQ(LoadMarks(store, "id,at\n1,-1..-0.5\n2,1e3..2\n"), "");
	EXPECT_EQ(store.Value(0, 0, 1)->v[1], -0.5f);
	EXPECT_EQ(store.Value(0, 1, 1)->v[0], 1000.0f);
	relation_store spaced = VecStore();
	EXPECT_TRUE(Mentions(LoadMarks(spaced, "id,at\n1,1 .. 2\n"), "'at'"));
}
```

- [ ] **Step 2: Build and watch it fail**

Run: `env -C $WT PWD=$WT scripts/wt-build.sh > $WT/build.log 2>&1; grep -E 'error:' $WT/build.log | head`
Expected: `column_type` has no member `Vec2`; `cell` has no member `Vector` / `v`.

- [ ] **Step 3: Header**

In `relation.h`, add `Vec2,` after `Command,` in `column_type`, and update the comment above the enum to read:

```cpp
// The value types are the only values Tabular knows; a host converts its own
// values to these, and a flag set (AngelScript uint32) and a hash (CityHash32)
// travel as int64_t, differing only in how a condition reads them. A vec2 is
// two floats; it is the only vector, and a band a..b is one.
```

In `union cell`, add the member and maker:

```cpp
	float   v[2];

	static cell Vector(float x, float y) { cell c; c.v[0] = x; c.v[1] = y; return c; }
```

- [ ] **Step 4: Store, YAML, CSV**

In `src/relation.cpp` `TypeName`, add `case column_type::Vec2: return "vec2";`. (Keys and orders already refuse it: `ComparesByEquality` admits only hash, flags and int.)

In `src/schema_yaml.cpp`:

```cpp
char const* const kTypeNames[] = {"hash", "flags", "int", "double", "condition", "command", "vec2"};
YamlEnum const    kTypes{kTypeNames, 7};
```

In `src/rows_csv.cpp` `ReadCell`, before `default:`, add:

```cpp
	case column_type::Vec2:
	{
		size_t const dots = text.find("..");
		float        x    = 0;
		float        y    = 0;
		if (dots == std::string_view::npos || !ParseWhole(text.substr(0, dots), x) || !ParseWhole(text.substr(dots + 2), y))
		{
			return tl::make_unexpected(ErrorContext("column '", column.name, "' is a vec2, written a..b; got '", text, "'"));
		}
		return cell::Vector(x, y);
	}
```

In `include/tabular/rows_csv.h`, in the comment on `csv_rows`, after "a double as from_chars reads it," insert "a vec2 as `a..b`, two floats,".

- [ ] **Step 5: Build and run**

Run: `env -C $WT PWD=$WT scripts/wt-build.sh > $WT/build.log 2>&1; grep -E 'error:' $WT/build.log | head; cd $WT && $WT/Build/Engine --gtest --gtest_filter="$FILTER" 2>&1 | grep -E '^\[  (FAILED|PASSED) '`
Expected: all pass.

- [ ] **Step 6: Commit**

```bash
cd $WT/Spehleon/Tabular && git add include src test && git commit -m "Columns: vec2, two floats in one cell; YAML and CSV (a..b)"
cd $WT/Spehleon && git add Tabular && git commit -m "Bump Tabular: vec2 column"
cd $WT && git add Spehleon cmake/sources_tests.cmake && git commit -m "Bump Spehleon: vec2 column; register column tests"
```

---

### Task 3: The string column

**Files:**
- Modify: `include/tabular/relation.h`, `src/relation.cpp`, `src/schema_yaml.cpp`, `src/rows_csv.cpp`, `include/tabular/rows_csv.h`, `test/relation_columns_gtest.cpp`, `test/rows_csv_gtest.cpp`, `test/schema_yaml_gtest.cpp`

**Interfaces:**
- Consumes: `relation_columns_gtest.cpp`'s `OneTable` and `Mentions` (Task 2); `load_rows::conditions` position (Task 1), since `strings` goes after it.
- Produces: `column_type::String` (after `Vec2`); `struct string_in { string_in(); string_in(std::string_view); string_in(counted_string const&); std::string_view text; }`; `insert_row::strings` and `load_rows::strings` (`std::span<string_in const>`, last field); `StringAt(table, row, column)` / `TableVersion::StringAt(row, column) -> tl::expected<counted_string, ErrorContext>`; `Find(table, column, std::string_view)` / `TableVersion::Find(column, std::string_view) -> tl::expected<std::vector<size_t>, ErrorContext>`; YAML type name `string`.

- [ ] **Step 1: Write the failing tests**

Append to `test/relation_columns_gtest.cpp` (add `#include "Support/counted_string.h"` at the top):

```cpp
namespace
{

// places(name string key, priority int), ordered by priority; read_only as given.
relation_store Places(bool read_only)
{
	static add_column const columns[] = {{"name", column_type::String, true}, {"priority", column_type::Int, false}};
	static uint32_t const   order[]   = {1};
	static order_by const   ordering{order, false};
	auto s = OneTable(columns, &ordering, read_only);
	EXPECT_TRUE(s.has_value()) << s.error().message;
	return relation_store(std::move(*s));
}

tl::expected<void, ErrorContext> InsertPlace(relation_store& store, uint64_t owner, string_in name, int64_t priority)
{
	cell const      values[]  = {cell::Integer(priority)};
	string_in const strings[] = {name};
	return store.Insert(insert_row{0, owner, values, {}, {}, strings});
}

}

TEST(RelationColumns, AStringOutlivesTheTextItWasLoadedFrom)
{
	relation_store store = Places(true);
	std::string text = "forest";
	cell const      values[]  = {cell::Integer(1)};
	string_in const strings[] = {std::string_view(text)};
	ASSERT_TRUE(store.Load(load_rows{0, 1, 1, values, {}, nullptr, {}, strings}).has_value());
	text.assign("XXXXXX");
	auto name = store.StringAt(0, 0, 0);
	ASSERT_TRUE(name.has_value()) << name.error().message;
	EXPECT_EQ(std::string_view(*name), "forest");
	auto version = *store.Version(0);
	EXPECT_EQ(std::string_view(*version->StringAt(0, 0)), "forest");
}

TEST(RelationColumns, AStringKeyIsFoundByContent)
{
	relation_store store = Places(true);
	cell const      values[]  = {cell::Integer(1), cell::Integer(2)};
	string_in const strings[] = {std::string_view("meadow"), std::string_view("moor")};
	ASSERT_TRUE(store.Load(load_rows{0, 1, 2, values, {}, nullptr, {}, strings}).has_value());
	std::string probe = std::string("mo") + "or";
	EXPECT_EQ(*store.Find(0, 0, std::string_view(probe)), std::vector<size_t>{1});
	EXPECT_EQ(*(*store.Version(0))->Find(0, std::string_view("meadow")), std::vector<size_t>{0});
	EXPECT_TRUE(store.Find(0, 0, std::string_view("marsh"))->empty());
}

TEST(RelationColumns, AnInternedCountedStringIsStoredAsTheSameHandle)
{
	relation_store store = Places(false);
	counted_string const shared = counted_string::MakeShared(std::string_view("meadow"));
	ASSERT_TRUE(InsertPlace(store, 1, shared, 1).has_value());
	EXPECT_EQ(store.StringAt(0, 0, 0)->ptr_compare(shared), 0);

	counted_string const unique = counted_string::MakeUnique(std::string_view("moor"));
	ASSERT_TRUE(InsertPlace(store, 1, unique, 2).has_value());
	EXPECT_NE(store.StringAt(0, 1, 0)->ptr_compare(unique), 0) << "a unique string is interned by content";
	EXPECT_EQ(*store.Find(0, 0, std::string_view(unique)), std::vector<size_t>{1});
}

TEST(RelationColumns, StringKeyFollowsRowsShiftedByAnOrderedInsert)
{
	relation_store store = Places(false);
	ASSERT_TRUE(InsertPlace(store, 1, std::string_view("b"), 2).has_value());
	ASSERT_TRUE(InsertPlace(store, 1, std::string_view("a"), 1).has_value());   // lands before "b"
	EXPECT_EQ(*store.Find(0, 0, std::string_view("a")), std::vector<size_t>{0});
	EXPECT_EQ(*store.Find(0, 0, std::string_view("b")), std::vector<size_t>{1});
}

TEST(RelationColumns, WritableStringsSurviveRetractingAnotherOwner)
{
	relation_store store = Places(false);
	ASSERT_TRUE(InsertPlace(store, 1, std::string_view("kept"), 1).has_value());
	ASSERT_TRUE(InsertPlace(store, 2, std::string_view("gone"), 2).has_value());
	store.Retract(retract_owner{2});
	ASSERT_EQ(store.RowCount(0), 1u);
	EXPECT_EQ(std::string_view(*store.StringAt(0, 0, 0)), "kept");
	EXPECT_EQ(*store.Find(0, 0, std::string_view("kept")), std::vector<size_t>{0});
	EXPECT_TRUE(store.Find(0, 0, std::string_view("gone"))->empty());
}

TEST(RelationColumns, EmptyStringIsStoredAndFound)
{
	relation_store store = Places(false);
	ASSERT_TRUE(InsertPlace(store, 1, std::string_view(""), 1).has_value());
	EXPECT_EQ(std::string_view(*store.StringAt(0, 0, 0)), "");
	EXPECT_EQ(*store.Find(0, 0, std::string_view("")), std::vector<size_t>{0});
}

TEST(RelationColumns, AStringIsNotInAnOrderAndNotAValue)
{
	add_column const columns[] = {{"name", column_type::String, false}};
	uint32_t const   first[]   = {0};
	order_by const   order{first, false};
	auto inOrder = OneTable(columns, &order, false);
	ASSERT_FALSE(inOrder.has_value());
	EXPECT_TRUE(Mentions(inOrder.error().message, "cannot be in an order")) << inOrder.error().message;

	relation_store store = Places(false);
	ASSERT_TRUE(InsertPlace(store, 1, std::string_view("a"), 1).has_value());
	EXPECT_FALSE(store.Value(0, 0, 0).has_value()) << "a string column is read with StringAt";
	EXPECT_FALSE(store.Find(0, 0, cell::Integer(0)).has_value()) << "a string key is looked up by text";
	EXPECT_FALSE(store.StringAt(0, 0, 1).has_value()) << "column 1 is an int";
}
```

Append to `test/schema_yaml_gtest.cpp`:

```cpp
TEST(SchemaYaml, ReadsTheStringType)
{
	auto s = SchemaFromYaml("tables:\n  - name: t\n    columns:\n      - {name: n, type: string, key: true}\n", "t.yaml");
	ASSERT_TRUE(s.has_value()) << s.error().format();
	EXPECT_EQ(s->tables()[0].columns[0].type, column_type::String);
}
```

Append to `test/rows_csv_gtest.cpp`:

```cpp
TEST(RowsCsv, AStringCellIsItsTrimmedText)
{
	auto s = SchemaFromYaml(
		"tables:\n  - name: places\n    read_only: true\n    columns:\n"
		"      - {name: name, type: string, key: true}\n      - {name: id, type: int}\n", "places.yaml");
	ASSERT_TRUE(s.has_value()) << s.error().format();
	relation_store store(std::move(*s));
	auto loaded = LoadCsv(store, csv_rows{0, 1, "id,name\n1,  north meadow \n", "places.csv"});
	ASSERT_TRUE(loaded.has_value()) << loaded.error().format();
	EXPECT_EQ(std::string_view(*store.StringAt(0, 0, 0)), "north meadow");
	EXPECT_EQ(*store.Find(0, 0, std::string_view("north meadow")), std::vector<size_t>{0});
}
```

- [ ] **Step 2: Build and watch it fail**

Run: `env -C $WT PWD=$WT scripts/wt-build.sh > $WT/build.log 2>&1; grep -E 'error:' $WT/build.log | head`
Expected: `column_type` has no member `String`; `string_in` is undeclared; no `StringAt`.

- [ ] **Step 3: Header**

In `relation.h`, add `#include "Support/counted_string.h"` beside the other Support include. Add `String,` after `Vec2,` in `column_type`. Extend the comment after the enum's `Condition and Command columns hold no value.` sentence with: `A String column holds an interned counted_string in a slot array of its own.`

Before `struct insert_row`, add:

```cpp
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
```

Append to `insert_row`: `std::span<string_in const> strings;   // one per String column, in column order`. Append to `load_rows` (after `conditions`): `std::span<string_in const> strings;    // count * String columns`.

In `TableVersion`, add:

```cpp
	tl::expected<counted_string, ErrorContext>      StringAt(size_t row, uint32_t column) const;
	tl::expected<std::vector<size_t>, ErrorContext> Find(uint32_t column, std::string_view text) const;
```

In `relation_store`, add after `CommandAt`:

```cpp
	tl::expected<counted_string, ErrorContext>   StringAt(uint32_t table, size_t row, uint32_t column) const;
```

and after the cell `Find`:

```cpp
	// Rows whose String key column holds text, in row order.
	tl::expected<std::vector<size_t>, ErrorContext> Find(uint32_t table, uint32_t column, std::string_view text) const;
```

Change the `Value` comment to: `A value column's cell. Condition, Command and String columns are read with Holds, CommandAt and StringAt instead.`

- [ ] **Step 4: Schema rules**

In `src/relation.cpp`: `TypeName` gets `case column_type::String: return "string";`. Add after `ComparesByEquality`:

```cpp
// A key needs equality only. Interned strings have it, so a string can be a
// key; it has no order worth sorting by, so it cannot be in one.
bool CanBeKey(column_type type)
{
	return ComparesByEquality(type) || type == column_type::String;
}
```

and in `BuildColumns` use `CanBeKey(in.type)` in place of `ComparesByEquality(in.type)`. `BuildOrder` keeps `ComparesByEquality`. Change `IsValueColumn` to:

```cpp
bool IsValueColumn(column_type type)
{
	return type != column_type::Condition && type != column_type::Command && type != column_type::String;
}
```

- [ ] **Step 5: Row storage and the index**

In `struct table_rows`, add the members:

```cpp
	std::vector<counted_string> strings;   // String columns, row-major, interned
	uint32_t                    stringCount = 0;
	// Per column, text -> rows in row order; filled for String key columns only.
	// The views point into strings' handles, which do not move.
	std::vector<std::unordered_map<std::string_view, std::vector<uint32_t>>> stringIndex;
```

In its constructor, add `stringIndex.resize(def.columns.size());` and the case `case column_type::String: slot[c] = stringCount++; break;`. Replace `RebuildIndex` with:

```cpp
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
```

Add to the second anonymous namespace (before `table_rows`):

```cpp
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
```

- [ ] **Step 6: `Insert`, `Retract`, `Load`**

In `Insert`, after the commands size check, add:

```cpp
	if (command.strings.size() != rows.stringCount)
	{
		return refuse(ErrorContext("insert into '", def.name, "' has ", command.strings.size(),
			" strings, but the table has ", rows.stringCount, " string columns"));
	}
```

After the `rows.commands.insert(...)` statement, add:

```cpp
	std::vector<counted_string> interned;
	interned.reserve(command.strings.size());
	for (string_in s : command.strings)
	{
		interned.push_back(Intern(s));
	}
	rows.strings.insert(rows.strings.begin() + pos * rows.stringCount,
		std::make_move_iterator(interned.begin()), std::make_move_iterator(interned.end()));
```

Replace the key-index loop at the end of `Insert` with:

```cpp
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
```

In the writable `Retract` compaction, beside the commands `std::move`, add:

```cpp
				std::move(rows.strings.begin() + r * rows.stringCount,
					rows.strings.begin() + (r + 1) * rows.stringCount,
					rows.strings.begin() + kept * rows.stringCount);
```

and beside the commands `resize`, add `rows.strings.resize(kept * rows.stringCount);`.

In `Load`, after the conditions size check (Task 1), add:

```cpp
	if (!spans(command.strings.size(), current.stringCount))
	{
		return tl::make_unexpected(ErrorContext("load into '", def.name, "' of ", command.count, " rows has ",
			command.strings.size(), " strings, but the table has ", current.stringCount, " string columns"));
	}
```

In the loop building `next`, after the conditions loop, add:

```cpp
		for (uint32_t k = 0; k < next.stringCount; ++k)
		{
			next.strings.push_back(s.loaded ? Intern(command.strings[s.row * next.stringCount + k])
			                                : current.strings[s.row * next.stringCount + k]);
		}
```

and `next.strings.reserve(order.size() * next.stringCount);` beside the other `reserve` calls.

- [ ] **Step 7: Readers**

In the readers' anonymous namespace, add:

```cpp
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
```

In `FindKey`, after the `!def.columns[column].key` check, add:

```cpp
	if (def.columns[column].type == column_type::String)
	{
		return tl::make_unexpected(ErrorContext("column '", def.columns[column].name, "' of '", def.name,
			"' is a string; look it up by text"));
	}
```

Add the public readers:

```cpp
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
```

- [ ] **Step 8: YAML and CSV**

In `src/schema_yaml.cpp`:

```cpp
char const* const kTypeNames[] = {"hash", "flags", "int", "double", "condition", "command", "vec2", "string"};
YamlEnum const    kTypes{kTypeNames, 8};
```

In `src/rows_csv.cpp` `LoadCsv`, replace the slot loop with one that numbers every kind:

```cpp
	std::vector<uint32_t> slot(def.columns.size());
	uint32_t valueCount     = 0;
	uint32_t commandCount   = 0;
	uint32_t stringCount    = 0;
	uint32_t conditionCount = 0;
	for (uint32_t c = 0; c < def.columns.size(); ++c)
	{
		switch (def.columns[c].type)
		{
		case column_type::Command:   slot[c] = commandCount++;   break;
		case column_type::String:    slot[c] = stringCount++;    break;
		case column_type::Condition: slot[c] = conditionCount++; break;
		default:                     slot[c] = valueCount++;     break;
		}
	}
```

(`conditionCount` is used in Task 4; until then mark it `[[maybe_unused]]` or leave the warning.) Beside `std::vector<std::string_view> commands(...)`, add `std::vector<string_in> strings(count * stringCount);`. In the cell loop, after the Command branch, add:

```cpp
			if (column.type == column_type::String)
			{
				strings[r * stringCount + slot[columnOf[h]]] = text;
				continue;
			}
```

and pass the strings to `Load`: `load_rows{command.table, command.owner, count, values, commands, &refusal, {}, strings}`.

In `rows_csv.h`'s comment, after "a command verbatim", insert ", a string as its trimmed text".

- [ ] **Step 9: Build and run**

Run: `env -C $WT PWD=$WT scripts/wt-build.sh > $WT/build.log 2>&1; grep -E 'error:' $WT/build.log | head; cd $WT && $WT/Build/Engine --gtest --gtest_filter="$FILTER" 2>&1 | grep -E '^\[  (FAILED|PASSED) '`
Expected: all pass.

- [ ] **Step 10: Prove a test bites**

Mutation: in `Insert`, delete the line `ShiftRows(rows.stringIndex[c], row);`. Rebuild, run `--gtest_filter='RelationColumns.StringKeyFollowsRowsShiftedByAnOrderedInsert'`.
Expected: FAIL (`"b"` found at row 0). Restore, `touch src/relation.cpp`, rebuild, rerun: pass.

- [ ] **Step 11: Commit**

```bash
cd $WT/Spehleon/Tabular && git add include src test && git commit -m "Columns: interned strings in their own slots; string keys found by text; YAML and CSV"
cd $WT/Spehleon && git add Tabular && git commit -m "Bump Tabular: string column"
cd $WT && git add Spehleon && git commit -m "Bump Spehleon: string column"
```

---

### Task 4: Condition cells from CSV

**Files:**
- Modify: `include/tabular/rows_csv.h`, `src/rows_csv.cpp`, `test/rows_csv_gtest.cpp`

**Interfaces:**
- Consumes: `load_rows::conditions` (Task 1); `tabular_test::MakeProbe` (Task 1); the slot loop with `conditionCount` (Task 3).
- Produces: `csv_rows::condition` (`std::function<tl::expected<closure, ErrorContext>(std::string_view)>`, last field).

- [ ] **Step 1: Write the failing tests**

Append to `test/rows_csv_gtest.cpp` (add `#include "probe_closure.h"` and `using namespace tabular_test;` at the top, and `#include <vector>`):

```cpp
namespace
{

constexpr std::string_view kRuleSchema = R"(
tables:
  - name: rules
    read_only: true
    columns:
      - {name: priority, type: int}
      - {name: when, type: condition}
)";

relation_store RuleStore()
{
	auto s = SchemaFromYaml(kRuleSchema, "rules.yaml");
	EXPECT_TRUE(s.has_value()) << s.error().format();
	return relation_store(std::move(*s));
}

}

TEST(RowsCsv, ConditionCellsGoThroughTheFunction)
{
	int released = 0;
	std::vector<std::string> seen;
	{
		relation_store store = RuleStore();
		csv_rows rows{0, 1, "priority,when\n1, hp < 5 \n2,always\n", "rules.csv"};
		rows.condition = [&](std::string_view text) -> tl::expected<closure, ErrorContext>
		{
			seen.emplace_back(text);
			return MakeProbe(static_cast<int64_t>(seen.size()) * 10, &released);
		};
		auto loaded = LoadCsv(store, rows);
		ASSERT_TRUE(loaded.has_value()) << loaded.error().format();
		int64_t fifteen = 15;
		EXPECT_TRUE(*store.Holds(0, 0, 1, &fifteen));
		EXPECT_FALSE(*store.Holds(0, 1, 1, &fifteen));
	}
	EXPECT_EQ(seen, (std::vector<std::string>{"hp < 5", "always"}));
	EXPECT_EQ(released, 2);
}

TEST(RowsCsv, AFailingConditionNamesTheLineAndReleasesEarlierCells)
{
	int released = 0;
	relation_store store = RuleStore();
	csv_rows rows{0, 1, "priority,when\n1,fine\n2,broken\n", "rules.csv"};
	rows.condition = [&](std::string_view text) -> tl::expected<closure, ErrorContext>
	{
		if (text == "broken")
		{
			return tl::make_unexpected(ErrorContext("does not compile"));
		}
		return MakeProbe(1, &released);
	};
	auto loaded = LoadCsv(store, rows);
	ASSERT_FALSE(loaded.has_value());
	std::string const error = loaded.error().format();
	EXPECT_TRUE(Mentions(error, "rules.csv:3")) << error;
	EXPECT_TRUE(Mentions(error, "does not compile")) << error;
	EXPECT_EQ(released, 1);
	EXPECT_EQ(store.RowCount(0), 0u);
}

TEST(RowsCsv, AConditionTableWithNoFunctionIsRefused)
{
	relation_store store = RuleStore();
	auto loaded = LoadCsv(store, csv_rows{0, 1, "priority,when\n1,fine\n", "rules.csv"});
	ASSERT_FALSE(loaded.has_value());
	EXPECT_TRUE(Mentions(loaded.error().format(), "condition function")) << loaded.error().format();
}

TEST(RowsCsv, ANullConditionNamesItsLine)
{
	int released = 0;
	relation_store store = RuleStore();
	csv_rows rows{0, 1, "priority,when\n1,fine\n2,empty\n", "rules.csv"};
	rows.condition = [&](std::string_view text) -> tl::expected<closure, ErrorContext>
	{
		return text == "empty" ? closure{} : MakeProbe(1, &released);
	};
	auto loaded = LoadCsv(store, rows);
	ASSERT_FALSE(loaded.has_value());
	EXPECT_TRUE(Mentions(loaded.error().format(), "rules.csv:3")) << loaded.error().format();
	EXPECT_EQ(released, 1);
}
```

Also find the existing test that expects a CSV into a condition table to be refused (search `rows_csv_gtest.cpp` for `condition`; it loads the `rules` table of `kSchema`). Change its expectation from "CSV cannot fill" to `Mentions(error, "condition function")`, since that is now why it is refused.

- [ ] **Step 2: Build and watch it fail**

Run: `env -C $WT PWD=$WT scripts/wt-build.sh > $WT/build.log 2>&1; grep -E 'error:' $WT/build.log | head`
Expected: `csv_rows` has no member `condition`.

- [ ] **Step 3: Header**

In `include/tabular/rows_csv.h`, add `#include <functional>`. Replace the sentence about columns in the `csv_rows` comment's first line with: `every column appears exactly once.` Add before `struct csv_rows` a paragraph:

```cpp
// A condition cell's text is handed to condition, which returns the compiled
// closure or an error; that is the whole of Tabular's part in compiling
// conditions. A table with condition columns needs one.
```

Append to `csv_rows`:

```cpp
	std::function<tl::expected<closure, ErrorContext>(std::string_view)> condition;
```

- [ ] **Step 4: `LoadCsv`**

In `src/rows_csv.cpp`, add to the anonymous namespace:

```cpp
// Closures built for the cells read so far. Load adopts them all, refused or
// not; until it is called they are ours, so an early return releases them.
struct pending_conditions
{
	std::vector<closure> list;
	bool                 handedOver = false;

	explicit pending_conditions(size_t count) : list(count) {}
	pending_conditions(pending_conditions const&)            = delete;
	pending_conditions& operator=(pending_conditions const&) = delete;
	~pending_conditions()
	{
		if (handedOver)
		{
			return;
		}
		for (closure const& c : list)
		{
			if (c.release)
			{
				c.release(c.data);
			}
		}
	}
};
```

Replace the loop that refuses condition columns with:

```cpp
	if (!command.condition)
	{
		for (column_def const& column : def.columns)
		{
			if (column.type == column_type::Condition)
			{
				return tl::make_unexpected(ErrorContext("table '", def.name, "' has condition column '", column.name,
					"', and no condition function was given").with(std::string(command.label)));
			}
		}
	}
```

After `strings` is declared, add `pending_conditions conditions(count * conditionCount);`. In the cell loop, after the String branch, add:

```cpp
			if (column.type == column_type::Condition)
			{
				auto built = command.condition(text);
				if (!built)
				{
					return tl::make_unexpected(std::move(built.error())
						.with("column '", column.name, "'").with(label(line)));
				}
				conditions.list[r * conditionCount + slot[columnOf[h]]] = *built;
				continue;
			}
```

Replace the `Load` call with:

```cpp
	load_refusal refusal;
	conditions.handedOver = true;   // Load adopts them, whatever it decides
	auto loaded = store.Load(load_rows{command.table, command.owner, count, values, commands, &refusal,
		conditions.list, strings});
```

- [ ] **Step 5: Build and run**

Run: `env -C $WT PWD=$WT scripts/wt-build.sh > $WT/build.log 2>&1; grep -E 'error:|warning:.*rows_csv' $WT/build.log | head; cd $WT && $WT/Build/Engine --gtest --gtest_filter="$FILTER" 2>&1 | grep -E '^\[  (FAILED|PASSED) '`
Expected: all pass; no warning from `rows_csv.cpp`.

- [ ] **Step 6: Prove a test bites**

Mutation: in `pending_conditions::~pending_conditions`, change `if (handedOver)` to `if (true)`. Rebuild, run `--gtest_filter='RowsCsv.AFailingConditionNamesTheLineAndReleasesEarlierCells'`.
Expected: FAIL (`released` is 0). Restore, `touch src/rows_csv.cpp`, rebuild, rerun: pass.

- [ ] **Step 7: Commit**

```bash
cd $WT/Spehleon/Tabular && git add include src test && git commit -m "CSV: condition cells through the caller's condition function"
cd $WT/Spehleon && git add Tabular && git commit -m "Bump Tabular: CSV condition cells"
cd $WT && git add Spehleon && git commit -m "Bump Spehleon: CSV condition cells"
```

---

### Task 5: Documents, and the whole suite

**Files:**
- Modify: `Spehleon/Tabular/docs/definition.md`, `Spehleon/Tabular/docs/feature-checklist.md`, `Spehleon/Tabular/docs/superpowers/specs/2026-10-16-relations-column-set-design.md`

- [ ] **Step 1: `definition.md`**

In "Order of work", item 3, replace the sentence beginning `A condition cell is handed, as text, to a function the caller passes to the loader, \`std::function<condition(std::string_view)>\`,` with `A condition cell is handed, as text, to a function the caller passes to the loader (\`csv_rows::condition\`, returning \`tl::expected<closure, ErrorContext>\`),`. After item 3, add:

```markdown
   **Column set** (built: string, `vec2` and condition columns, in `tabular/relation.h`): a string
   cell is an interned `counted_string` (a caller may pass one it holds, or a view), and a string
   key column is found by text; a `vec2` is two floats, written `a..b` in CSV; read-only tables
   hold conditions, each released once when the last version holding its row lets go.
```

- [ ] **Step 2: `feature-checklist.md`**

Tick these three items `[x]`: the `vec2` column; the string column; "CSV filling condition columns". Then the consequence line "Conditions are `{function, data, release}` closures, released with their row" stays `[x]`.

- [ ] **Step 3: Spec, as built**

In the spec's "Cell layout" section, replace `readers that hand a \`vec2\` out return \`glm::vec2\`` with `readers hand it out as a \`cell\`, read through \`v\``. In its CSV "Ownership across a failure" bullet, replace the sentence with: `Closures built for earlier cells are held by a guard that releases them on any early return; \`Load\` adopts them all once called.`

- [ ] **Step 4: Full suite**

Run: `cd $WT && env -u GTEST_FILTER $WT/Build/Engine --gtest > $WT/suite.log 2>&1; grep -E '^\[  (FAILED|PASSED|SKIPPED) ' $WT/suite.log | sort -u`
Expected: every test passes, apart from known load-sensitive reds (`SimCoordinatorDeterminism.AsymmetricWindIsWorkerCountInvariant`, `ScriptExpectedComponents.MtaCabinRefusalsPreserveModeOffsetAndDimensions`); any such red is rerun alone (`--gtest_filter=<name>`) and must pass three times.

- [ ] **Step 5: Commit**

```bash
cd $WT/Spehleon/Tabular && git add docs && git commit -m "Definition: the column set is built"
cd $WT/Spehleon && git add Tabular && git commit -m "Bump Tabular: column set documented"
cd $WT && git add Spehleon && git commit -m "Bump Spehleon: Tabular column set documented"
```
