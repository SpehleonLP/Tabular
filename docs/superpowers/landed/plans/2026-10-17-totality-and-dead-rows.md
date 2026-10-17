# Totality and Dead Rows Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A best table's load is refused unless every key run always picks a row and every row in it can be picked; band inputs declare a range that picks clamp to; CSV hash cells refuse 0 and same-file collisions.

**Architecture:** Two new load-time checks run inside `relation_store::Load` for `best` tables, over the key runs a load adds rows to, using one internal coverage helper (`src/coverage.h`) that finds a point a set of half-open boxes misses. A null-function `closure` becomes "no script condition", which is what makes a row quiescent. Ranges live on the input definitions; picks clamp in `RowHolds`. Hash checks live in the CSV front ends' shared cell reader.

**Tech Stack:** C++20, tl::expected, gtest compiled into `Build/Engine`, Spehleon/lib (`Errors`, `Support`, `Utility/load_csv.h`, `Yaml`), CityHash32.

**Spec:** `Spehleon/Tabular/docs/superpowers/landed/specs/2026-10-17-totality-and-dead-rows-design.md`. The contract is `Spehleon/Tabular/docs/definition.md`.

## Global Constraints

- Worktree: `env -C /mnt/Passport/Kreatures-Engine PWD=/mnt/Passport/Kreatures-Engine scripts/wt-create.sh tabular-totality`; `WT=/mnt/Passport/Worktrees/tabular-totality`. Never edit, build or run git in `/mnt/Passport/Kreatures-Engine`.
- Build: `env -C $WT PWD=$WT scripts/wt-build.sh` (foreground; queueing is normal). Never call cmake/ninja directly.
- Tests, from `$WT`: `cd $WT && GTEST_FILTER="$FILTER" Build/Engine --gtest`, with `FILTER='Relation*:RowsCsv.*:SchemaYaml.*:Pick*:PivotCsv.*:Grid*:Coverage.*:HashNames.*'`. Never an empty `GTEST_FILTER`.
- Commit leaves first: `Spehleon/Tabular`, then `Spehleon` (gitlink bump plus any `Spehleon/cmake` change), then the superproject (gitlink bump plus any `cmake/sources_tests.cmake` change). End each message with the session's `Co-Authored-By` / `Claude-Session` lines. Never push, merge or rebase.
- Brace every control-flow body. Errors are returned (`tl::expected<…, ErrorContext>`), never logged; no `LOG_F(ERROR/FATAL)`. Asserts are live in release: add none.
- snake_case types are never `new`ed. Big parameters by `const&` or `&&`.
- New aggregate fields go at the **end** of their struct (`add_input`, `input_def`, `load_rows`, `load_refusal` are built positionally).
- `cc(...)` and `ErrorContext(...)` refuse floats (`Support/strings.h`): format a float with `detail::FloatText` (Task 3).
- Comments say why, not what.
- After restoring any deliberate mutation, `touch` the file so the build sees it.
- `git grep` skips submodules: grep inside `Spehleon/Tabular` directly. Never grep or find rooted at `/`.

### Spec amendments made while planning (owner to see in the final report)

1. **A range is three fields, not `std::optional<std::array<float, 2>>`**: `bool ranged = false; float lo = 0; float hi = 0;` at the end of `add_input` and `input_def`, so `relation.h` gains no `<optional>`/`<array>` include and existing positional `add_input{name, type}` initialisers keep compiling.
2. **A dead row's covering row goes in a new `load_refusal::covered_by`**, not in `earlier`: the CSV front ends print `earlier` as "same key as line N", which is wrong for a dead row. They print `covered_by` as "covered by line N".
3. **Messages keep "no row with no flags"** for ranked tables (`... has no row with no flags and no script condition`), so the existing refusals' wording survives; an unranked table says `... has no row with no script condition`.
4. **A band whose start is not below its end is refused** with the out-of-range band (it holds for no input once ranged), and a row whose band columns on one input do not overlap is a dead row with no covering rows ("its bands hold for no input").
5. **A survivor made dead by a load** (a loaded row with a lower pivot column ahead of it in tie order) refuses the load; `refusal.row` is then the loaded covering row.
6. **Two private helpers are tested directly**: `test/coverage_gtest.cpp` includes `../src/coverage.h` (it has no public surface), and `test/csv_hash_gtest.cpp` includes `../src/csv_cells.h` to test hash 0, because no text hashing to 0 is known.

## Review Focus

1. A ranged input at exactly `hi`, above `hi`, below `lo`, and NaN: `hi` and above pick the top band, below picks the bottom band, NaN holds no band (and with no quiescent band-free row picks nothing, never crashes). → Task 3, `PickBest.ARangedInputIsClampedAndNanHoldsNothing`.
2. A reload of one owner that removes the only quiescent row of a run another owner extends: the next load of that other owner is refused, but the reload that removed it (no rows of its own in the run) is not. → Task 4, `PickBest.ARunTheLoadDoesNotTouchIsNotRechecked`.
3. Two band columns on one input, partly overlapping: a row's box is their intersection, so totality and dead rows see the intersection, not either band. → Task 5, `PickBest.TwoBandsOnOneInputIntersect`.
4. A pivoted lookup line with two headers spelling the same flags (`Crouch|Injured`, `Injured|Crouch`) both filled: the right one is a dead row, refused naming the line. → Task 5, `PivotCsv.EqualFlagsOnOneLineAreADeadRow`.
5. A hash collision between a key in one row and a hash value cell in another row of the same file: refused (one namespace per file, not per column). → Task 2, `RowsCsv.AHashCollisionAcrossColumnsIsRefused`.

---

### Task 1: A row with no script condition

**Files:**
- Modify: `Spehleon/Tabular/include/tabular/relation.h` (the `closure` comment)
- Modify: `Spehleon/Tabular/src/relation.cpp` (`Insert`, `Load`, `ReadHolds`)
- Modify: `Spehleon/Tabular/src/rows_csv.cpp` (condition cells)
- Modify: `Spehleon/Tabular/include/tabular/rows_csv.h` (comment)
- Test: `Spehleon/Tabular/test/relation_load_conditions_gtest.cpp`, `Spehleon/Tabular/test/rows_csv_gtest.cpp`

**Interfaces:**
- Produces: `closure{}` (null `function`) is accepted by `Load` and `Insert` and means "no script condition"; `Holds` on it returns `true` without calling anything. `closure{nullptr, data, release}` with `data` or `release` set is refused. A blank CSV condition cell becomes `closure{}` without calling `csv_rows::condition`.

- [ ] **Step 1: Write the failing tests**

In `test/relation_load_conditions_gtest.cpp`, **replace** `TEST(RelationLoadConditions, AConditionWithNoFunctionIsRefusedAndNamesItsRow)` with:

```cpp
TEST(RelationLoadConditions, ANullFunctionIsNoConditionAndHolds)
{
	int released = 0;
	relation_store store = MakeRules(false);
	ASSERT_TRUE(LoadRules(store, 1, {{1, MakeProbe(5, &released)}, {2, closure{}}}).has_value());
	int64_t one = 1;
	auto first = store.Holds(0, 0, 1, &one);
	ASSERT_TRUE(first.has_value()) << first.error().format();
	EXPECT_FALSE(*first);
	auto second = store.Holds(0, 1, 1, &one);
	ASSERT_TRUE(second.has_value()) << second.error().format();
	EXPECT_TRUE(*second) << "a row with no script condition holds";
}

TEST(RelationLoadConditions, ANullFunctionWithDataIsRefusedAndNamesItsRow)
{
	int released = 0;
	relation_store store = MakeRules(false);
	load_refusal refusal;
	auto loaded = LoadRules(store, 1, {{1, MakeProbe(1, &released)}, {2, closure{nullptr, &released, nullptr}}}, &refusal);
	ASSERT_FALSE(loaded.has_value());
	EXPECT_TRUE(Mentions(loaded.error().format(), "no function")) << loaded.error().format();
	EXPECT_EQ(refusal.row, 1u);
	EXPECT_EQ(released, 1);
}
```

In `test/relation_store_gtest.cpp`, find the writable rules helper `InsertRule` (it inserts `(priority, closure, command)`); add after the last test using it:

```cpp
TEST(RelationStore, InsertTakesANullFunctionAsNoCondition)
{
	relation_store store = RulesStore();
	ASSERT_TRUE(InsertRule(store, 1, 10, closure{}, "(a)").has_value());
	int64_t one = 1;
	auto holds = store.Holds(0, 0, 0, &one);
	ASSERT_TRUE(holds.has_value()) << holds.error().format();
	EXPECT_TRUE(*holds);
	EXPECT_FALSE(InsertRule(store, 1, 11, closure{nullptr, nullptr, [](void*) {}}, "(a)").has_value())
		<< "a release with no function is refused";
}
```

(Read `relation_store_gtest.cpp` first: use the actual name of the store-building helper the `InsertRule` tests use in place of `RulesStore()`, and its condition column number in place of the `0` passed as `Holds`' column. Keep the test's assertions.)

In `test/rows_csv_gtest.cpp`, **replace** `TEST(RowsCsv, ANullConditionNamesItsLine)` with:

```cpp
TEST(RowsCsv, ABlankConditionCellIsNoConditionAndNeverReachesTheFunction)
{
	int released = 0;
	std::vector<std::string> seen;
	relation_store store = RuleStore();
	csv_rows rows{0, 1, "priority,when\n1,fine\n2,\n3,   \n", "rules.csv"};
	rows.condition = [&](std::string_view text) -> tl::expected<closure, ErrorContext>
	{
		seen.emplace_back(text);
		return MakeProbe(100, &released);
	};
	auto loaded = LoadCsv(store, rows);
	ASSERT_TRUE(loaded.has_value()) << loaded.error().format();
	EXPECT_EQ(seen, (std::vector<std::string>{"fine"}));
	int64_t one = 1;
	EXPECT_FALSE(*store.Holds(0, 0, 1, &one));
	EXPECT_TRUE(*store.Holds(0, 1, 1, &one));
	EXPECT_TRUE(*store.Holds(0, 2, 1, &one)) << "cells are trimmed, so spaces are blank";
}

TEST(RowsCsv, AHostClosureWithReleaseButNoFunctionNamesItsLine)
{
	int released = 0;
	relation_store store = RuleStore();
	csv_rows rows{0, 1, "priority,when\n1,fine\n2,odd\n", "rules.csv"};
	rows.condition = [&](std::string_view text) -> tl::expected<closure, ErrorContext>
	{
		return text == "odd" ? closure{nullptr, nullptr, [](void*) {}} : MakeProbe(1, &released);
	};
	auto loaded = LoadCsv(store, rows);
	ASSERT_FALSE(loaded.has_value());
	EXPECT_TRUE(Mentions(loaded.error().format(), "rules.csv:3")) << loaded.error().format();
	EXPECT_EQ(released, 1);
}
```

- [ ] **Step 2: Build and watch it fail**

Run the build, then `$FILTER`. Expected: the four new tests fail (a null function is refused today; a blank cell reaches the function).

- [ ] **Step 3: Implement**

`include/tabular/relation.h`, the comment above `struct closure` becomes:

```cpp
// A condition the host compiled. function returns whether the row counts for
// this_in; when it cannot answer it fills error_out. release frees data and is
// called once, when no table version or writable table holds the row any longer, or when the row is refused.
// A null function is no condition: the row counts, and data and release are null too.
```

`src/relation.cpp`, add in the anonymous namespace near `ReleaseAll`:

```cpp
// A null function is "no script condition"; with data or a release it is a
// closure the host half-built, whose data would leak.
bool IsMalformed(closure const& c)
{
	return c.function == nullptr && (c.data != nullptr || c.release != nullptr);
}
```

In `relation_store::Insert`, replace

```cpp
		if (column.type == column_type::Condition && command.conditions[rows.slot[c]].function == nullptr)
		{
			return refuse(ErrorContext("column '", column.name, "' of '", def.name, "' got a condition with no function"));
		}
```

with

```cpp
		if (column.type == column_type::Condition && IsMalformed(command.conditions[rows.slot[c]]))
		{
			return refuse(ErrorContext("column '", column.name, "' of '", def.name,
				"' got a condition with no function but data or a release"));
		}
```

In `relation_store::Load`, in the loop over `command.conditions`, replace the test `command.conditions[i].function == nullptr` with `IsMalformed(command.conditions[i])`, and the message with `"load into '", def.name, "' got a condition with no function but data or a release"`.

In `ReadHolds`, after `closure const& c = ...;` add:

```cpp
	if (c.function == nullptr)
	{
		return true;   // no script condition
	}
```

`src/rows_csv.cpp`, in the condition branch of the row loop, before `auto built = command.condition(text);`:

```cpp
				// The one blank cell CSV accepts: "no condition", which the
				// loader must see to prove a table total.
				if (text.empty())
				{
					conditions.list[r * conditionCount + slot[columnOf[h]]] = closure{};
					continue;
				}
```

`include/tabular/rows_csv.h`, in the comment above `struct csv_rows`, after "A table with condition columns needs one." add: `A blank condition cell is no condition: it is not handed to condition.`

- [ ] **Step 4: Build and run** `$FILTER`. Expected: all pass.

- [ ] **Step 5: Mutation check**

In `ReadHolds`, change `return true;   // no script condition` to `return false;`. Rebuild, run `$FILTER`: `ANullFunctionIsNoConditionAndHolds` and `ABlankConditionCellIs...` must fail cleanly (FAILED, not an abort). Restore, `touch src/relation.cpp`.

- [ ] **Step 6: Commit**

```bash
git -C $WT/Spehleon/Tabular add -A && git -C $WT/Spehleon/Tabular commit -m "A null-function closure is no script condition; a blank CSV condition cell is one"
git -C $WT/Spehleon add Tabular && git -C $WT/Spehleon commit -m "Bump Tabular: rows with no script condition"
git -C $WT add Spehleon && git -C $WT commit -m "Bump Spehleon: Tabular rows with no script condition"
```

(Each message ends with the two attribution lines.)

---

### Task 2: Hash 0 and same-file collisions

**Files:**
- Modify: `Spehleon/Tabular/src/csv_cells.h`
- Modify: `Spehleon/Tabular/src/rows_csv.cpp` (`hash_names`; row cells)
- Modify: `Spehleon/Tabular/src/pivot_csv.cpp` (keys, hash cells)
- Modify: `Spehleon/Tabular/src/grid_csv.cpp` (axes, hash cells)
- Modify: `Spehleon/Tabular/include/tabular/rows_csv.h` (comments)
- Test: `Spehleon/Tabular/test/rows_csv_gtest.cpp`, `Spehleon/Tabular/test/pivot_csv_gtest.cpp`, `Spehleon/Tabular/test/grid_gtest.cpp`
- Create: `Spehleon/Tabular/test/csv_hash_gtest.cpp`; modify superproject `cmake/sources_tests.cmake`

**Interfaces:**
- Produces (in `csv_cells.h`, namespace `tabular::detail`):

```cpp
// Every text one CSV load hashed, in any column, so two names with one hash,
// or a name hashing to 0, are refused: a script looking either name up would
// get the other, or a "no hash".
class hash_names
{
public:
	tl::expected<void, ErrorContext> Note(std::string_view text, int64_t hash, uint32_t line);

private:
	std::unordered_map<int64_t, std::pair<std::string, uint32_t>> _seen;
};

// ReadCell, recording a hash cell's text in names.
tl::expected<cell, ErrorContext> ReadCell(column_def const& column, std::string_view text, hash_names& names,
	uint32_t line);
```

The pinned collision: `CityHash32("n26963") == CityHash32("n50336") == 0x67943049` (found by brute force over `n0, n1, …`; the test asserts it so a changed hash function shows up as a broken pin, not a silently passing test).

- [ ] **Step 1: Write the failing tests**

`test/rows_csv_gtest.cpp`, append:

```cpp
TEST(RowsCsv, TwoNamesWithOneHashAreRefusedNamingBothLines)
{
	ASSERT_EQ(Hash("n26963"), Hash("n50336")) << "the pinned collision no longer collides";
	relation_store store = Store();
	std::string error = Load(store,
		"package,section,mask,x,on_spawn\nn26963,1,0,0,(a)\nother,2,0,0,(b)\nn50336,3,0,0,(c)\n");
	EXPECT_TRUE(Mentions(error, "'n50336'")) << error;
	EXPECT_TRUE(Mentions(error, "'n26963'")) << error;
	EXPECT_TRUE(Mentions(error, "line 2")) << error;
	EXPECT_TRUE(Mentions(error, "spawn.csv:4")) << error;
	EXPECT_EQ(store.RowCount(0), 0u);
}

TEST(RowsCsv, TheSameNameOnManyLinesIsNoCollision)
{
	relation_store store = Store();
	EXPECT_EQ(Load(store, "package,section,mask,x,on_spawn\nbase,1,0,0,(a)\nbase,2,0,0,(b)\n"), "");
}

TEST(RowsCsv, AHashCollisionAcrossColumnsIsRefused)
{
	auto s = SchemaFromYaml(R"(
tables:
  - name: links
    read_only: true
    columns:
      - {name: from, type: hash}
      - {name: to, type: hash}
)", "links.yaml");
	ASSERT_TRUE(s.has_value()) << s.error().format();
	relation_store store(std::move(*s));
	auto loaded = LoadCsv(store, csv_rows{0, 1, "from,to\nn26963,a\nb,n50336\n", "links.csv"});
	ASSERT_FALSE(loaded.has_value());
	EXPECT_TRUE(Mentions(loaded.error().format(), "links.csv:3")) << loaded.error().format();
}
```

`test/pivot_csv_gtest.cpp`, append (a lookup keyed by a hash):

```cpp
TEST(PivotCsv, KeysThatCollideAreRefused)
{
	relation_store store = StoreFrom(R"(
tables:
  - name: lookup
    read_only: true
    pick: best
    inputs:
      - {name: mode, type: flags}
    columns:
      - {name: label, type: hash, key: true}
      - {name: req, type: flags, input: mode}
      - {name: clip, type: string}
    order_by: {columns: [label]}
    pivot: {headers: req, cells: clip}
)");
	std::string error = Error(LoadPivot(store, "label,None,Injured\nn26963,a,b\nn50336,c,d\n"));
	EXPECT_TRUE(Mentions(error, "'n26963'")) << error;
	EXPECT_TRUE(Mentions(error, "lookup.csv:3")) << error;
}
```

(`Error(...)` is the file's existing helper returning `""` on success; if it is named differently, use that name.)

`test/grid_gtest.cpp`, append (use the file's grid-from-YAML helper the `GridCsv` tests use; the grid below has hash axes and int cells):

```cpp
TEST(GridCsv, AxisKeysThatCollideAreRefused)
{
	auto s = SchemaFromYaml(R"(
tables:
  - name: g
    grid:
      rows: {name: r, type: hash}
      columns: {name: c, type: hash}
      cells: int
)", "g.yaml");
	ASSERT_TRUE(s.has_value()) << s.error().format();
	relation_store store(std::move(*s));
	auto across = LoadGridCsv(store, csv_grid{0, 1, ",n26963,x\nn50336,1,2\n", "g.csv"});
	ASSERT_FALSE(across.has_value()) << "a row key colliding with a column key";
	EXPECT_TRUE(across.error().format().find("g.csv:2") != std::string::npos) << across.error().format();
}
```

No text hashing to 0 is known (a search of `n0` … `n5000000000` found none), so 0 is tested on the helper directly. Create `test/csv_hash_gtest.cpp` (register it in superproject `cmake/sources_tests.cmake` after `rows_csv_gtest.cpp`):

```cpp
#include <gtest/gtest.h>
#include "../src/csv_cells.h"

#include <string>

using tabular::detail::hash_names;

TEST(HashNames, ZeroIsRefusedAndRepeatsAreNot)
{
	hash_names names;
	auto zero = names.Note("anything", 0, 3);
	ASSERT_FALSE(zero.has_value());
	EXPECT_TRUE(zero.error().format().find("hashes to 0") != std::string::npos) << zero.error().format();
	EXPECT_TRUE(names.Note("a", 7, 4).has_value());
	EXPECT_TRUE(names.Note("a", 7, 9).has_value()) << "the same text again is a key, not a collision";
	auto clash = names.Note("b", 7, 10);
	ASSERT_FALSE(clash.has_value());
	EXPECT_TRUE(clash.error().format().find("'a' on line 4") != std::string::npos) << clash.error().format();
}
```

Add `HashNames.*` to `$FILTER` from here on.

- [ ] **Step 2: Build and watch it fail.** Expected: `hash_names` missing (compile error); once declared, the three CSV refusal tests fail (loads accepted); `TheSameNameOnManyLinesIsNoCollision` passes.

- [ ] **Step 3: Implement**

`src/csv_cells.h`: add `#include <string>`, `#include <unordered_map>`, `#include <utility>`, and the `hash_names` class and `ReadCell` overload from **Interfaces** above, after the existing `ReadCell` declaration.

`src/rows_csv.cpp`, in `namespace detail`, after the existing `ReadCell`:

```cpp
tl::expected<void, ErrorContext> hash_names::Note(std::string_view text, int64_t hash, uint32_t line)
{
	if (hash == 0)
	{
		return tl::make_unexpected(ErrorContext("'", text, "' hashes to 0, which no name may"));
	}
	auto [it, added] = _seen.try_emplace(hash, std::string(text), line);
	if (!added && it->second.first != text)
	{
		return tl::make_unexpected(ErrorContext("'", text, "' has the same hash as '", it->second.first,
			"' on line ", it->second.second));
	}
	return {};
}

tl::expected<cell, ErrorContext> ReadCell(column_def const& column, std::string_view text, hash_names& names,
	uint32_t line)
{
	auto value = ReadCell(column, text);
	if (value && column.type == column_type::Hash)
	{
		auto noted = names.Note(text, value->i, line);
		if (!noted)
		{
			return tl::make_unexpected(std::move(noted.error()));
		}
	}
	return value;
}
```

In `LoadCsv`, declare `detail::hash_names names;` before the row loop, and change `auto value = detail::ReadCell(column, text);` to `auto value = detail::ReadCell(column, text, names, line);`.

`src/pivot_csv.cpp`, in `LoadPivotedCsv`: declare `hash_names names;` before the row loop; the key read becomes `ReadCell(def.columns[def.order[k]], file.cell(k, r), names, line)`; the cells read becomes `ReadCell(column_def{(*headers)[h].name, cells.type}, cellText, names, line)`.

`src/grid_csv.cpp`, in `LoadGridCsv`: declare `detail::hash_names names;` before the column-key loop; the column-key read becomes `detail::ReadCell(def.grid_columns, file.cell(h, 0), names, headerLine)`, the row-key read `detail::ReadCell(def.grid_rows, file.cell(0, r), names, line)`, and the value-cell read `detail::ReadCell(column, cellText, names, line)`.

`include/tabular/rows_csv.h`: in the `csv_rows` comment, after "a hash is CityHash32 of the text," add "and within one load two texts with one hash, or a text hashing to 0, are refused, naming both lines,"; in the `csv_grid` comment after "a hash is CityHash32 of the text" add "(collisions refused as in LoadCsv)".

- [ ] **Step 4: Build and run** `$FILTER`. Expected: all pass.

- [ ] **Step 5: Mutation check.** In `hash_names::Note`, change `it->second.first != text` to `false`. Rebuild, run: the three collision tests fail cleanly. Restore, `touch src/rows_csv.cpp`.

- [ ] **Step 6: Commit** (Tabular; Spehleon bump; superproject bump with `cmake/sources_tests.cmake`): `CSV: two names with one hash, or a name hashing to 0, are refused`.

---

### Task 3: Input ranges and clamping

**Files:**
- Modify: `Spehleon/Tabular/include/tabular/relation.h` (`add_input`, `input_def`)
- Modify: `Spehleon/Tabular/src/relation_internal.h` (`FloatText`)
- Modify: `Spehleon/Tabular/src/relation.cpp` (`BuildInputs`, `BuildPick`, `Load`)
- Modify: `Spehleon/Tabular/src/pick.cpp` (`RowHolds`)
- Modify: `Spehleon/Tabular/src/schema_yaml.cpp` (`range`)
- Test: `Spehleon/Tabular/test/pick_schema_gtest.cpp`, `Spehleon/Tabular/test/pick_gtest.cpp`, `Spehleon/Tabular/test/schema_yaml_gtest.cpp`

**Interfaces:**
- Produces:

```cpp
struct add_input
{
	std::string_view name;
	column_type      type   = column_type::Double;   // Double or Flags
	bool             ranged = false;                 // Double only: lo <= x < hi; picks clamp to it
	float            lo     = 0;
	float            hi     = 0;
};

struct input_def
{
	std::string name;
	column_type type   = column_type::Double;
	bool        ranged = false;
	float       lo     = 0;
	float       hi     = 0;
};
```

and in `relation_internal.h`, namespace `tabular::detail`:

```cpp
// cc refuses floats; messages spell them the shortest way that reads back.
inline std::string FloatText(float v)
{
	char buffer[32];
	auto r = std::to_chars(buffer, buffer + sizeof buffer, v);
	return std::string(buffer, r.ptr);
}
```

YAML: an input may carry `range: lo..hi`.

- [ ] **Step 1: Write the failing tests**

`test/pick_schema_gtest.cpp`: change `kInputs` to

```cpp
add_input const kInputs[] = {{"mode", column_type::Flags}, {"t", column_type::Double, true, 0.0f, 1.0f}};
```

and append:

```cpp
TEST(PickSchema, ABestTablesBandInputDeclaresARange)
{
	add_input const unranged[] = {{"mode", column_type::Flags}, {"t", column_type::Double}};
	EXPECT_TRUE(Mentions(Refusal(Build(Columns(), unranged)), "declares no range")) << Refusal(Build(Columns(), unranged));
	EXPECT_EQ(Refusal(Build(Columns(), unranged, pick_policy::None)), "") << "only a best table needs one";
}

TEST(PickSchema, ARangeIsOnADoubleAndRunsLowToHigh)
{
	add_input const onFlags[] = {{"mode", column_type::Flags, true, 0.0f, 1.0f}, {"t", column_type::Double, true, 0.0f, 1.0f}};
	EXPECT_TRUE(Mentions(Refusal(Build(Columns(), onFlags)), "only a double has a range"));
	add_input const backwards[] = {{"mode", column_type::Flags}, {"t", column_type::Double, true, 1.0f, 0.0f}};
	EXPECT_TRUE(Mentions(Refusal(Build(Columns(), backwards)), "lo < hi"));
	add_input const infinite[] = {{"mode", column_type::Flags}, {"t", column_type::Double, true, 0.0f, INFINITY}};
	EXPECT_TRUE(Mentions(Refusal(Build(Columns(), infinite)), "lo < hi"));
}
```

(add `#include <cmath>` for `INFINITY`.)

`test/pick_gtest.cpp`: in `kGuarded`, change the input line to `      - {name: t, type: double, range: 0..1}`. Append:

```cpp
TEST(PickBest, ARangedInputIsClampedAndNanHoldsNothing)
{
	relation_store store = StoreFrom(kGuarded);
	ASSERT_TRUE(LoadGuarded(store, {{1, 0.0f, 0.5f, closure{}, 10}, {1, 0.5f, 1.0f, closure{}, 11}}).has_value());
	EXPECT_EQ(GuardedClip(store, 1, -3.0, 0), 10) << "below lo reads as lo";
	EXPECT_EQ(GuardedClip(store, 1, 1.0, 0), 11) << "hi reads as the largest float below it";
	EXPECT_EQ(GuardedClip(store, 1, 7.0, 0), 11);
	EXPECT_EQ(GuardedClip(store, 1, std::nan(""), 0), -1) << "NaN is not clamped and holds no band";
}

TEST(PickBest, ABandOutsideItsInputsRangeIsRefused)
{
	relation_store store = StoreFrom(kGuarded);
	load_refusal refusal;
	std::vector<cell> values = {cell::Integer(1), cell::Vector(0.0f, 1.0f), cell::Integer(10),
	                            cell::Integer(1), cell::Vector(0.5f, 1.5f), cell::Integer(11)};
	std::vector<closure> conditions = {closure{}, closure{}};
	auto loaded = store.Load(load_rows{0, 1, 2, values, {}, &refusal, conditions});
	ASSERT_FALSE(loaded.has_value());
	EXPECT_TRUE(Mentions(loaded.error().format(), "0.5..1.5")) << loaded.error().format();
	EXPECT_TRUE(Mentions(loaded.error().format(), "range 0..1")) << loaded.error().format();
	EXPECT_EQ(refusal.row, 1u);

	std::vector<cell> backwards = {cell::Integer(1), cell::Vector(0.5f, 0.25f), cell::Integer(10)};
	std::vector<closure> one = {closure{}};
	EXPECT_FALSE(store.Load(load_rows{0, 1, 1, backwards, {}, nullptr, one}).has_value());
}
```

`test/schema_yaml_gtest.cpp`, append:

```cpp
TEST(SchemaYaml, AnInputReadsItsRange)
{
	auto s = SchemaFromYaml(R"(
tables:
  - name: phase
    read_only: true
    pick: or
    output: bits
    inputs:
      - {name: t, type: double, range: -0.5..2}
    columns:
      - {name: clip, type: int, key: true}
      - {name: bits, type: flags}
      - {name: window, type: band, input: t}
    order_by: {columns: [clip]}
)", "phase.yaml");
	ASSERT_TRUE(s.has_value()) << s.error().format();
	input_def const& t = s->tables()[0].inputs[0];
	EXPECT_TRUE(t.ranged);
	EXPECT_EQ(t.lo, -0.5f);
	EXPECT_EQ(t.hi, 2.0f);

	auto bad = SchemaFromYaml(R"(
tables:
  - name: phase
    read_only: true
    inputs:
      - {name: t, type: double, range: "0-1"}
    columns:
      - {name: clip, type: int}
)", "phase.yaml");
	ASSERT_FALSE(bad.has_value());
	EXPECT_TRUE(bad.error().format().find("written lo..hi") != std::string::npos) << bad.error().format();
}
```

- [ ] **Step 2: Build and watch it fail.** Expected: compile errors on the new `add_input` fields, then (once the struct exists) the refusals and clamps fail.

- [ ] **Step 3: Implement**

`relation.h`: the `add_input` and `input_def` from **Interfaces**.

`relation_internal.h`: add `#include <charconv>` and `FloatText` (Interfaces) in `namespace detail`.

`relation.cpp`, `BuildInputs`: after the type check, before `inputs[i] = ...`:

```cpp
		if (in.ranged && in.type != column_type::Double)
		{
			return tl::make_unexpected(ErrorContext("input '", in.name, "' of table '", c.name, "' is a ",
				TypeName(in.type), "; only a double has a range"));
		}
		if (in.ranged && !(std::isfinite(in.lo) && std::isfinite(in.hi) && in.lo < in.hi))
		{
			return tl::make_unexpected(ErrorContext("input '", in.name, "' of table '", c.name, "' has range ",
				FloatText(in.lo), "..", FloatText(in.hi), "; a range is lo < hi, both finite"));
		}
		inputs[i] = input_def{std::string(in.name), in.type, in.ranged, in.lo, in.hi};
```

(replacing the old `inputs[i] = ...` line; add `#include <cmath>`; `FloatText` is `detail::FloatText`, so add `using detail::FloatText;` beside the file's other `using` lines, or qualify it.)

`BuildPick`: after `if (!out.read_only) {...}`, add:

```cpp
	// Totality covers a best table's band inputs, so each needs a finite domain.
	if (c.pick == pick_policy::Best)
	{
		for (column_def const& col : out.columns)
		{
			if (col.type == column_type::Band && !out.inputs[col.input].ranged)
			{
				return tl::make_unexpected(ErrorContext("band '", col.name, "' of best table '", c.name,
					"' reads input '", out.inputs[col.input].name, "', which declares no range"));
			}
		}
	}
```

`Load`: in the per-row loop that calls `Check32`, add after the `IsValueColumn` block (inside the column loop):

```cpp
			if (def.columns[c].type == column_type::Band && def.inputs[def.columns[c].input].ranged)
			{
				input_def const& in   = def.inputs[def.columns[c].input];
				cell const       band = command.values[r * current.valueCount + current.slot[c]];
				// Clamped inputs never reach past the range, so a band that does
				// is partly dead, and one running backwards holds nowhere.
				if (!(in.lo <= band.v[0] && band.v[0] < band.v[1] && band.v[1] <= in.hi))
				{
					if (command.refusal)
					{
						command.refusal->row = r;
					}
					return tl::make_unexpected(ErrorContext("band '", def.columns[c].name, "' of '", def.name, "' is ",
						FloatText(band.v[0]), "..", FloatText(band.v[1]), "; it runs low to high inside input '",
						in.name, "''s range ", FloatText(in.lo), "..", FloatText(in.hi)).with("row ", r));
				}
			}
```

`pick.cpp`, `RowHolds`: replace `float const x = static_cast<float>(query.inputs[def.columns[c].input].d);` with:

```cpp
		input_def const& in = def.inputs[def.columns[c].input];
		float            x  = static_cast<float>(query.inputs[def.columns[c].input].d);
		// Clamped in float, as bands compare; hi is excluded, so the top value
		// lands in the top band. NaN fails both tests and stays NaN.
		if (in.ranged)
		{
			if (x < in.lo)
			{
				x = in.lo;
			}
			else if (x >= in.hi)
			{
				x = std::nextafter(in.hi, -INFINITY);
			}
		}
```

(add `#include <cmath>`.)

`schema_yaml.cpp`: `input_yaml` gains `std::string range;`; `kInputFields` gains `{"range", FieldType::String, offsetof(input_yaml, range)},`; add `#include "csv_cells.h"`. In the input loop, replace `inputs[t].push_back(add_input{i.name, static_cast<column_type>(i.type)});` with a loop over indices:

```cpp
		LF::Yaml::Node const* inputNodes = tableNodes->as_sequence().items[t]->get("inputs");
		for (uint32_t ii = 0; ii < in.inputs.size(); ++ii)
		{
			input_yaml const& i = in.inputs[ii];
			add_input         a{i.name, static_cast<column_type>(i.type)};
			if (inputNodes->as_sequence().items[ii]->get("range") != nullptr)
			{
				size_t const dots = i.range.find("..");
				a.ranged = dots != std::string::npos && i.range.find("...") == std::string::npos
					&& detail::ParseWhole(std::string_view(i.range).substr(0, dots), a.lo)
					&& detail::ParseWhole(std::string_view(i.range).substr(dots + 2), a.hi);
				if (!a.ranged)
				{
					return tl::make_unexpected(ErrorContext("input '", i.name, "' of table '", in.name, "' has range '",
						i.range, "'; a range is written lo..hi").with(std::string(label)));
				}
			}
			inputs[t].push_back(a);
		}
```

This sits where the old input loop was, so `tableNodes` is already declared above it; if it is declared later in the function, move its declaration up.

Fixtures that now fail with "declares no range": give their band input `range: 0..1` (all existing best-table bands lie in 0..1). Search: `grep -n "pick: best" -A6 test/*.cpp | grep "type: double"`.

- [ ] **Step 4: Build and run** `$FILTER`. Expected: all pass.

- [ ] **Step 5: Mutation check.** In `RowHolds`, change `x = std::nextafter(in.hi, -INFINITY);` to `x = in.hi;`. Rebuild, run: `ARangedInputIsClampedAndNanHoldsNothing` fails cleanly. Restore, `touch src/pick.cpp`.

- [ ] **Step 6: Commit**: `Inputs declare a range; picks clamp to it; a band outside it is refused`.

---

### Task 4: Coverage and totality

**Files:**
- Create: `Spehleon/Tabular/src/coverage.h`, `Spehleon/Tabular/src/coverage.cpp`
- Modify: `Spehleon/cmake/sources_tabular.cmake` (add `${SPEHLEON_ROOT}/Tabular/src/coverage.cpp` after `pick.cpp`)
- Modify: `Spehleon/Tabular/src/relation.cpp` (`Load`: the run checks)
- Create: `Spehleon/Tabular/test/coverage_gtest.cpp`
- Modify: superproject `cmake/sources_tests.cmake` (add `${SPEHLEON_ROOT}/Tabular/test/coverage_gtest.cpp` after `pick_gtest.cpp`)
- Test: `Spehleon/Tabular/test/pick_gtest.cpp`

**Interfaces:**
- Produces (`src/coverage.h`, namespace `tabular::detail`):

```cpp
struct interval
{
	float lo = 0;
	float hi = 0;   // half-open: lo <= x < hi
};

// The lowest point of domain (one interval per dimension, dimension 0 most
// significant) that none of count boxes contains, or none. boxes holds count
// boxes of domain.size() intervals each, one after another. Exact: every
// dimension is cut at every box edge inside the domain, so a cell lies wholly
// inside or outside each box, and its low corner speaks for it. With no
// dimensions the domain is one point, covered by any box.
std::optional<std::vector<float>> Uncovered(std::span<interval const> domain, std::span<interval const> boxes,
	size_t count);
```

- Produces (in `relation.cpp`'s anonymous namespace, used by Task 5): the run walker below (`run_rows`, `BandAxes`, `BoxOf`).

- [ ] **Step 1: Write the failing tests**

`test/coverage_gtest.cpp`:

```cpp
#include <gtest/gtest.h>
#include "../src/coverage.h"

#include <vector>

using tabular::detail::interval;
using tabular::detail::Uncovered;

TEST(Coverage, OneDimensionFindsTheGap)
{
	interval const domain[] = {{0.0f, 1.0f}};
	interval const boxes[]  = {{0.0f, 0.2f}, {0.4f, 1.0f}};
	auto hole = Uncovered(domain, boxes, 2);
	ASSERT_TRUE(hole.has_value());
	EXPECT_EQ(*hole, (std::vector<float>{0.2f}));
}

TEST(Coverage, TouchingBandsLeaveNoSeam)
{
	interval const domain[] = {{0.0f, 1.0f}};
	interval const boxes[]  = {{0.5f, 1.0f}, {0.0f, 0.5f}};
	EXPECT_FALSE(Uncovered(domain, boxes, 2).has_value());
}

TEST(Coverage, BoxesReachingPastTheDomainCoverIt)
{
	interval const domain[] = {{0.25f, 0.75f}};
	interval const boxes[]  = {{0.0f, 1.0f}};
	EXPECT_FALSE(Uncovered(domain, boxes, 1).has_value());
}

TEST(Coverage, TwoDimensionsFindTheCornerOfAnLShapedHole)
{
	interval const domain[] = {{0.0f, 1.0f}, {0.0f, 1.0f}};
	// Everything but [0.5, 1) x [0.5, 1).
	interval const boxes[] = {{0.0f, 0.5f}, {0.0f, 1.0f},
	                          {0.5f, 1.0f}, {0.0f, 0.5f}};
	auto hole = Uncovered(domain, boxes, 2);
	ASSERT_TRUE(hole.has_value());
	EXPECT_EQ(*hole, (std::vector<float>{0.5f, 0.5f}));
}

TEST(Coverage, NoDimensionsIsCoveredByAnyBox)
{
	EXPECT_TRUE(Uncovered({}, {}, 0).has_value());
	EXPECT_FALSE(Uncovered({}, {}, 1).has_value());
}

TEST(Coverage, AnEmptyDomainIsCovered)
{
	interval const domain[] = {{0.5f, 0.5f}};
	EXPECT_FALSE(Uncovered(domain, {}, 0).has_value());
}
```

`test/pick_gtest.cpp`: first make the existing guarded tests total (each needs a quiescent row covering 0..1; their assertions stay):

- `WithoutRankingTakesTheFirstRowThatHolds`: both rows' `MakeProbe(0, &released)` become `closure{}`.
- `FallsThroughARowWhoseScriptConditionFails`: the second row's `MakeProbe(0, &released)` becomes `closure{}`.
- `BandsAreTestedBeforeTheScript`: the second row's `MakeProbe(0, &released)` becomes `closure{}`, and a third row `{1, 0.0f, 0.5f, closure{}, 12}` is appended.
- `AFailingScriptConditionFailsThePick`: append a second row `{1, 0.0f, 1.0f, closure{}, 11}`.

Remove any `released` variable left unused. Then append, after the helpers namespace (a ranked table with a band, used here and in Task 5):

```cpp
namespace
{

// label (key), req (ranks by mode), window (band over t in 0..1), when (script), clip.
constexpr std::string_view kRankedBand = R"(
tables:
  - name: lookup
    read_only: true
    pick: best
    inputs:
      - {name: mode, type: flags}
      - {name: t, type: double, range: 0..1}
    columns:
      - {name: label, type: int, key: true}
      - {name: req, type: flags, input: mode}
      - {name: window, type: band, input: t}
      - {name: when, type: condition}
      - {name: clip, type: int}
    order_by: {columns: [label]}
)";

struct band_row
{
	int64_t  label;
	int64_t  req;
	float    lo;
	float    hi;
	closure  when;
	int64_t  clip;
	uint16_t pivot = 0;
};

tl::expected<void, ErrorContext> LoadBand(relation_store& store, std::vector<band_row> const& rows,
	load_refusal* refusal = nullptr, uint64_t owner = 1)
{
	std::vector<cell>     values;
	std::vector<closure>  conditions;
	std::vector<uint16_t> pivot;
	for (band_row const& r : rows)
	{
		values.push_back(cell::Integer(r.label));
		values.push_back(cell::Integer(r.req));
		values.push_back(cell::Vector(r.lo, r.hi));
		values.push_back(cell::Integer(r.clip));
		conditions.push_back(r.when);
		pivot.push_back(r.pivot);
	}
	return store.Load(load_rows{0, owner, rows.size(), values, {}, refusal, conditions, {}, pivot});
}

int64_t BandClip(relation_store const& store, int64_t label, int64_t mode, double t)
{
	cell const keys[]   = {cell::Integer(label)};
	cell const inputs[] = {cell::Integer(mode), cell::Real(t)};
	int64_t    thisIn   = 0;
	auto picked = PickBest(store, 0, pick_query{keys, inputs, &thisIn});
	EXPECT_TRUE(picked.has_value()) << RefusalOf(picked);
	if (!picked || !*picked)
	{
		return -1;
	}
	auto clip = store.Value(0, **picked, 3);
	EXPECT_TRUE(clip.has_value()) << RefusalOf(clip);
	return clip ? clip->i : -1;
}

closure Never()
{
	return closure{[](void*, void const*, ErrorContext*) { return false; }, nullptr, nullptr};
}

}

TEST(PickBest, TheReactionTableLoads)
{
	relation_store store = StoreFrom(kGuarded);
	ASSERT_EQ(RefusalOf(LoadGuarded(store, {{1, 0.0f, 0.2f, closure{}, 10},
	                                        {1, 0.2f, 1.0f, Never(), 11},
	                                        {1, 0.2f, 1.0f, closure{}, 12}})), "(accepted)");
	EXPECT_EQ(GuardedClip(store, 1, 0.5, 0), 12);
}

TEST(PickBest, AGapInTheQuiescentRowsIsRefusedNamingAValue)
{
	relation_store store = StoreFrom(kGuarded);
	load_refusal refusal;
	std::vector<cell> values = {cell::Integer(1), cell::Vector(0.0f, 0.2f), cell::Integer(10),
	                            cell::Integer(1), cell::Vector(0.2f, 1.0f), cell::Integer(11)};
	std::vector<closure> conditions = {closure{}, Never()};
	auto loaded = store.Load(load_rows{0, 1, 2, values, {}, &refusal, conditions});
	ASSERT_FALSE(loaded.has_value());
	std::string const error = loaded.error().format();
	EXPECT_TRUE(Mentions(error, "key (1): t 0.2 has no row with no script condition")) << error;
	EXPECT_EQ(refusal.row, 0u) << "the run's first loaded row";
	EXPECT_EQ(store.RowCount(0), 0u);
}

TEST(PickBest, ADefaultWithAScriptConditionIsNotADefault)
{
	relation_store store = StoreFrom(kRankedBand);
	load_refusal refusal;
	auto loaded = LoadBand(store, {{1, 0, 0.0f, 1.0f, Never(), 10}, {1, 1, 0.0f, 1.0f, closure{}, 11}}, &refusal);
	ASSERT_FALSE(loaded.has_value());
	EXPECT_TRUE(Mentions(loaded.error().format(), "no row with no flags and no script condition"))
		<< loaded.error().format();
}

TEST(PickBest, AnUnrankedTableOfScriptedRowsIsRefused)
{
	auto s = SchemaFromYaml(R"(
tables:
  - name: rules
    read_only: true
    pick: best
    columns:
      - {name: label, type: int, key: true}
      - {name: when, type: condition}
      - {name: clip, type: int}
    order_by: {columns: [label]}
)", "rules.yaml");
	ASSERT_TRUE(s.has_value()) << s.error().format();
	relation_store store(std::move(*s));
	cell const values[] = {cell::Integer(1), cell::Integer(10)};
	closure const scripted[] = {Never()};
	auto loaded = store.Load(load_rows{0, 1, 1, values, {}, nullptr, scripted});
	ASSERT_FALSE(loaded.has_value());
	EXPECT_TRUE(Mentions(loaded.error().format(), "key (1) has no row with no script condition"))
		<< loaded.error().format();
	closure const plain[] = {closure{}};
	EXPECT_TRUE(store.Load(load_rows{0, 1, 1, values, {}, nullptr, plain}).has_value());
}

TEST(PickBest, ARunTheLoadDoesNotTouchIsNotRechecked)
{
	relation_store store = StoreFrom(kRankedBand);
	ASSERT_EQ(RefusalOf(LoadBand(store, {{1, 0, 0.0f, 1.0f, closure{}, 10}}, nullptr, 1)), "(accepted)");
	ASSERT_EQ(RefusalOf(LoadBand(store, {{1, 1, 0.0f, 1.0f, closure{}, 11}}, nullptr, 2)), "(accepted)");
	EXPECT_EQ(RefusalOf(LoadBand(store, {{2, 0, 0.0f, 1.0f, closure{}, 20}}, nullptr, 1)), "(accepted)")
		<< "owner 1 leaves run 1; the reload adds nothing to it, so it is not refused";
	EXPECT_EQ(BandClip(store, 1, 0, 0.5), -1) << "run 1 has no default now, and picks nothing";
	EXPECT_NE(RefusalOf(LoadBand(store, {{1, 2, 0.0f, 1.0f, closure{}, 12}}, nullptr, 3)), "(accepted)")
		<< "a load adding to run 1 is checked, and run 1 has no default";
}
```

Also in `AKeyRunWithoutADefaultIsRefusedAtLoad` nothing changes (its substring "no row with no flags" is kept by amendment 3).

- [ ] **Step 2: Build and watch it fail.** Expected: `coverage.h` missing (compile error). After Step 3's coverage files exist but before the Load change: `TheReactionTableLoads` passes, the four refusal tests fail.

- [ ] **Step 3: Implement coverage**

`src/coverage.h`: the header from **Interfaces**, with include guard `TABULAR_COVERAGE_H`, `#include <cstddef>`, `<optional>`, `<span>`, `<vector>`. A comment at the top: `// Internal to Tabular: the load checks' one geometric question. Not a public header.`

`src/coverage.cpp`:

```cpp
#include "coverage.h"

#include <algorithm>

namespace tabular::detail
{

std::optional<std::vector<float>> Uncovered(std::span<interval const> domain, std::span<interval const> boxes,
	size_t count)
{
	size_t const dims = domain.size();
	// Cuts per dimension: the domain's ends and every box edge strictly inside.
	std::vector<std::vector<float>> cuts(dims);
	for (size_t d = 0; d < dims; ++d)
	{
		if (!(domain[d].lo < domain[d].hi))
		{
			return std::nullopt;   // an empty domain has no point to miss
		}
		cuts[d] = {domain[d].lo, domain[d].hi};
		for (size_t b = 0; b < count; ++b)
		{
			for (float edge : {boxes[b * dims + d].lo, boxes[b * dims + d].hi})
			{
				if (edge > domain[d].lo && edge < domain[d].hi)
				{
					cuts[d].push_back(edge);
				}
			}
		}
		std::sort(cuts[d].begin(), cuts[d].end());
		cuts[d].erase(std::unique(cuts[d].begin(), cuts[d].end()), cuts[d].end());
	}
	// Odometer over cells, dimension 0 most significant, so the first miss is
	// the lowest and a refusal names the same point every time.
	std::vector<size_t> cell(dims, 0);
	std::vector<float>  point(dims);
	while (true)
	{
		for (size_t d = 0; d < dims; ++d)
		{
			point[d] = cuts[d][cell[d]];
		}
		bool covered = false;
		for (size_t b = 0; b < count && !covered; ++b)
		{
			covered = true;
			for (size_t d = 0; d < dims && covered; ++d)
			{
				interval const& in = boxes[b * dims + d];
				covered = in.lo <= point[d] && point[d] < in.hi;
			}
		}
		if (!covered)
		{
			return point;
		}
		size_t d = dims;
		while (d > 0)
		{
			--d;
			if (++cell[d] + 1 < cuts[d].size())
			{
				break;
			}
			cell[d] = 0;
			if (d == 0)
			{
				return std::nullopt;
			}
		}
		if (dims == 0)
		{
			return std::nullopt;
		}
	}
}

}
```

Check: with `dims == 0` and `count == 0`, the loop finds `covered == false` and returns the empty point; with `count > 0`, `covered` is true for the first box and the `dims == 0` exit returns none.

- [ ] **Step 4: Implement totality in `Load`**

In `relation.cpp` add `#include "coverage.h"`. **Replace** the whole block that begins `// A ranked best table answers "nothing matched" with its R == 0 row` (through its closing brace) with the code below. It keeps the run walk and generalises the check; Task 5 adds dead rows inside the same per-run body at the marked place.

```cpp
	// A best table always picks a row (totality) and every row in it can be
	// picked (no dead rows). Checked for the key runs this load adds rows to;
	// runs it does not touch are not re-checked, so removing rows is never
	// refused (Retract relies on it).
	if (def.pick == pick_policy::Best)
	{
		// One axis per ranged input a band reads, in input order.
		std::vector<uint32_t> axes;
		for (uint32_t i = 0; i < def.inputs.size(); ++i)
		{
			for (column_def const& col : def.columns)
			{
				if (col.type == column_type::Band && col.input == i)
				{
					axes.push_back(i);
					break;
				}
			}
		}
		std::vector<detail::interval> domain;
		for (uint32_t i : axes)
		{
			domain.push_back({def.inputs[i].lo, def.inputs[i].hi});
		}
		// A row's box: its input's range, cut down by every band on that input.
		auto boxOf = [&](source s, std::vector<detail::interval>& out)
		{
			std::span<cell const> cells = cellsOf(s);
			for (size_t a = 0; a < axes.size(); ++a)
			{
				detail::interval box = domain[a];
				for (uint32_t c = 0; c < def.columns.size(); ++c)
				{
					if (def.columns[c].type == column_type::Band && def.columns[c].input == axes[a])
					{
						box.lo = std::max(box.lo, cells[current.slot[c]].v[0]);
						box.hi = std::min(box.hi, cells[current.slot[c]].v[1]);
					}
				}
				out.push_back(box);
			}
		};
		uint32_t const conditionSlot = current.conditionCount == 0 ? UINT32_MAX : 0;
		auto scripted = [&](source s)
		{
			if (conditionSlot == UINT32_MAX)
			{
				return false;
			}
			closure const& c = s.loaded ? command.conditions[s.row * current.conditionCount]
			                            : current.conditions[s.row * current.conditionCount]->get();
			return c.function != nullptr;
		};
		auto flagsOf = [&](source s) -> uint32_t
		{
			return def.ranking == UINT32_MAX ? 0u : static_cast<uint32_t>(cellsOf(s)[current.slot[def.ranking]].i);
		};
		auto pivotOf = [&](source s) -> uint16_t
		{
			return s.loaded ? (command.pivot.empty() ? uint16_t(0) : command.pivot[s.row]) : current.pivots[s.row];
		};
		auto sameKey = [&](source a, source b)
		{
			for (uint32_t k = 0; k < def.key_count; ++k)
			{
				uint32_t const s = current.orderSlots[k];
				if (cellsOf(a)[s].i != cellsOf(b)[s].i)
				{
					return false;
				}
			}
			return true;
		};
		auto keyText = [&](source s)
		{
			std::string key;
			for (uint32_t k = 0; k < def.key_count; ++k)
			{
				key = cc(key, key.empty() ? "" : ", ", cellsOf(s)[current.orderSlots[k]].i);
			}
			return key;
		};
		char const* const noDefault = def.ranking == UINT32_MAX ? "no row with no script condition"
		                                                        : "no row with no flags and no script condition";

		std::vector<detail::interval> boxes;
		size_t start = 0;
		while (start < order.size())
		{
			size_t end = start + 1;
			while (end < order.size() && sameKey(order[start], order[end]))
			{
				++end;
			}
			size_t firstLoaded = SIZE_MAX;
			for (size_t i = start; i < end; ++i)
			{
				if (order[i].loaded && firstLoaded == SIZE_MAX)
				{
					firstLoaded = order[i].row;
				}
			}
			if (firstLoaded == SIZE_MAX)
			{
				start = end;
				continue;
			}

			// Totality: the quiescent rows cover every input. A ranked row
			// never helps: an input with no flags skips every one.
			boxes.clear();
			size_t quiescent = 0;
			for (size_t i = start; i < end; ++i)
			{
				if (!scripted(order[i]) && flagsOf(order[i]) == 0)
				{
					boxOf(order[i], boxes);
					++quiescent;
				}
			}
			auto hole = detail::Uncovered(domain, boxes, quiescent);
			if (hole)
			{
				if (command.refusal)
				{
					command.refusal->row = firstLoaded;
				}
				std::string where;
				for (size_t a = 0; a < axes.size(); ++a)
				{
					where = cc(where, a == 0 ? "" : ", ", def.inputs[axes[a]].name, " ", FloatText((*hole)[a]));
				}
				return tl::make_unexpected(axes.empty()
					? ErrorContext("table '", def.name, "' key (", keyText(order[start]), ") has ", noDefault)
					: ErrorContext("table '", def.name, "' key (", keyText(order[start]), "): ", where, " has ", noDefault));
			}

			// (Task 5: dead rows go here.)

			start = end;
		}
	}
```

`std::max`/`std::min` need `<algorithm>` (check the file's includes).

- [ ] **Step 5: Build and run** `$FILTER`. Expected: all pass, including the existing default-row tests (`AKeyRunWithoutADefaultIsRefusedAtLoad`, `AKeyRunIsEveryKeyColumnAndNoMore`, `PivotCsv.ALabelWithoutNoneIsRefusedNamingItsLine`, `PivotCsv.ARefusedLoadLeavesTheFlagNames`).

`PickBest.EqualRanksGoToPivotColumnThenRowOrder` and `PivotCsv.EqualRanksFollowHeaderOrder` still pass here; Task 5 changes them.

- [ ] **Step 6: Mutation checks**

1. In the totality loop, change `!scripted(order[i]) && flagsOf(order[i]) == 0` to `flagsOf(order[i]) == 0`. Rebuild, run: `ADefaultWithAScriptConditionIsNotADefault`, `AGapInTheQuiescentRowsIsRefusedNamingAValue` and `AnUnrankedTableOfScriptedRowsIsRefused` fail cleanly. Restore, `touch src/relation.cpp`.
2. In `Uncovered`, change `point[d] < in.hi` to `point[d] <= in.hi`. Rebuild, run: `Coverage.OneDimensionFindsTheGap` fails. Restore, `touch src/coverage.cpp`.

- [ ] **Step 7: Commit** (Tabular; Spehleon with the gitlink and `cmake/sources_tabular.cmake`; superproject with the gitlink and `cmake/sources_tests.cmake`): `Totality: a best table's quiescent rows cover every input in each key run a load touches`.

---

### Task 5: Dead rows

**Files:**
- Modify: `Spehleon/Tabular/include/tabular/relation.h` (`load_refusal::covered_by`)
- Modify: `Spehleon/Tabular/src/relation.cpp` (`Load`: the marked place)
- Modify: `Spehleon/Tabular/src/rows_csv.cpp`, `Spehleon/Tabular/src/pivot_csv.cpp` (print `covered_by`)
- Test: `Spehleon/Tabular/test/pick_gtest.cpp`, `Spehleon/Tabular/test/pivot_csv_gtest.cpp`, `Spehleon/Tabular/test/rows_csv_gtest.cpp`

**Interfaces:**
- Consumes: Task 4's `scripted`, `flagsOf`, `pivotOf`, `boxOf`, `domain`, `axes`, `keyText`, `detail::Uncovered`.
- Produces:

```cpp
struct load_refusal
{
	size_t row        = SIZE_MAX;
	size_t earlier    = SIZE_MAX;
	size_t covered_by = SIZE_MAX;   // a dead row: the loaded row tried just before it that covers it
};
```

- [ ] **Step 1: Write the failing tests**

`test/pick_gtest.cpp`: **replace** `TEST(PickBest, EqualRanksGoToPivotColumnThenRowOrder)` with (ties are only visible when the earlier row can fail, so these use bands):

```cpp
TEST(PickBest, EqualRanksGoToPivotColumnThenRowOrder)
{
	relation_store store = StoreFrom(kRankedBand);
	ASSERT_EQ(RefusalOf(LoadBand(store, {{1, 0, 0.0f, 1.0f, closure{}, 10},
	                                     {1, 1, 0.0f, 1.0f, closure{}, 11, 2},
	                                     {1, 1, 0.0f, 0.5f, closure{}, 12, 1},
	                                     {2, 0, 0.0f, 1.0f, closure{}, 20},
	                                     {2, 1, 0.0f, 0.5f, closure{}, 21},
	                                     {2, 1, 0.0f, 1.0f, closure{}, 22}})), "(accepted)");
	EXPECT_EQ(BandClip(store, 1, 1, 0.25), 12) << "the lower pivot column wins";
	EXPECT_EQ(BandClip(store, 1, 1, 0.75), 11);
	EXPECT_EQ(BandClip(store, 2, 1, 0.25), 21) << "equal pivot columns: row order";
	EXPECT_EQ(BandClip(store, 2, 1, 0.75), 22);
}
```

Append:

```cpp
TEST(PickBest, ARowEarlierRowsCoverIsRefusedNamingThem)
{
	relation_store store = StoreFrom(kGuarded);
	load_refusal refusal;
	auto loaded = LoadGuardedWith(store, {{1, 0.0f, 0.6f, closure{}, 10},
	                                      {1, 0.4f, 1.0f, closure{}, 11},
	                                      {1, 0.2f, 0.8f, closure{}, 12}}, &refusal);
	ASSERT_FALSE(loaded.has_value());
	std::string const error = loaded.error().format();
	EXPECT_TRUE(Mentions(error, "loaded row 2 is never picked")) << error;
	EXPECT_TRUE(Mentions(error, "loaded rows 0, 1 cover it")) << error;
	EXPECT_EQ(refusal.row, 2u);
	EXPECT_EQ(refusal.covered_by, 1u);
}

TEST(PickBest, AScriptedEarlierRowShadowsNothing)
{
	relation_store store = StoreFrom(kGuarded);
	EXPECT_EQ(RefusalOf(LoadGuarded(store, {{1, 0.0f, 1.0f, Never(), 10},
	                                        {1, 0.0f, 1.0f, closure{}, 11}})), "(accepted)");
}

TEST(PickBest, APartialOverlapIsPriorityNotDeath)
{
	relation_store store = StoreFrom(kGuarded);
	ASSERT_EQ(RefusalOf(LoadGuarded(store, {{1, 0.0f, 0.6f, closure{}, 10},
	                                        {1, 0.4f, 1.0f, closure{}, 11}})), "(accepted)");
	EXPECT_EQ(GuardedClip(store, 1, 0.5, 0), 10);
	EXPECT_EQ(GuardedClip(store, 1, 0.7, 0), 11);
}

TEST(PickBest, DifferentFlagsNeverShadow)
{
	relation_store store = StoreFrom(kRankedBand);
	EXPECT_EQ(RefusalOf(LoadBand(store, {{1, 0, 0.0f, 1.0f, closure{}, 10},
	                                     {1, 1, 0.0f, 1.0f, closure{}, 11},
	                                     {1, 3, 0.0f, 1.0f, closure{}, 13},
	                                     {1, 2, 0.0f, 1.0f, closure{}, 12}})), "(accepted)");
	EXPECT_EQ(BandClip(store, 1, 1, 0.5), 11);
	EXPECT_EQ(BandClip(store, 1, 3, 0.5), 13);
	EXPECT_EQ(BandClip(store, 1, 2, 0.5), 12);
}

TEST(PickBest, SameFlagsShadowInTieOrder)
{
	relation_store store = StoreFrom(kRankedBand);
	load_refusal refusal;
	auto loaded = LoadBand(store, {{1, 0, 0.0f, 1.0f, closure{}, 10},
	                               {1, 1, 0.0f, 0.5f, closure{}, 11, 2},
	                               {1, 1, 0.0f, 1.0f, closure{}, 12, 1}}, &refusal);
	ASSERT_FALSE(loaded.has_value()) << "pivot 1 is tried first and covers pivot 2's band";
	EXPECT_EQ(refusal.row, 1u);
	EXPECT_EQ(refusal.covered_by, 2u);
}

TEST(PickBest, TwoBandsOnOneInputIntersect)
{
	relation_store store = StoreFrom(R"(
tables:
  - name: twice
    read_only: true
    pick: best
    inputs:
      - {name: t, type: double, range: 0..1}
    columns:
      - {name: label, type: int, key: true}
      - {name: a, type: band, input: t}
      - {name: b, type: band, input: t}
      - {name: clip, type: int}
    order_by: {columns: [label]}
)");
	auto load = [&](std::vector<std::array<float, 4>> const& rows)
	{
		std::vector<cell> values;
		for (auto const& r : rows)
		{
			values.push_back(cell::Integer(1));
			values.push_back(cell::Vector(r[0], r[1]));
			values.push_back(cell::Vector(r[2], r[3]));
			values.push_back(cell::Integer(0));
		}
		return store.Load(load_rows{0, 1, rows.size(), values});
	};
	EXPECT_EQ(RefusalOf(load({{0.0f, 1.0f, 0.0f, 0.6f}, {0.4f, 1.0f, 0.0f, 1.0f}})), "(accepted)")
		<< "row 0 holds on 0..0.6 only, so row 1 still owns 0.6..1";
	EXPECT_TRUE(Mentions(RefusalOf(load({{0.0f, 0.5f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.6f, 1.0f}})), "0.5"))
		<< "row 0 covers 0..0.5 and row 1 0.6..1: 0.5 has no row";
	EXPECT_TRUE(Mentions(RefusalOf(load({{0.0f, 1.0f, 0.0f, 1.0f}, {0.0f, 0.4f, 0.6f, 1.0f}})), "hold for no input"))
		<< "row 1's bands do not overlap";
}
```

Add to the helpers namespace (beside `LoadGuarded`; it only adds a refusal):

```cpp
tl::expected<void, ErrorContext> LoadGuardedWith(relation_store& store, std::vector<guarded_row> const& rows,
	load_refusal* refusal)
{
	std::vector<cell>    values;
	std::vector<closure> conditions;
	for (guarded_row const& r : rows)
	{
		values.push_back(cell::Integer(r.label));
		values.push_back(cell::Vector(r.lo, r.hi));
		values.push_back(cell::Integer(r.clip));
		conditions.push_back(r.when);
	}
	return store.Load(load_rows{0, 1, rows.size(), values, {}, refusal, conditions});
}
```

(`Never()` and `kRankedBand` come from Task 4; if `LoadGuardedWith` is declared after its first use, move it up. Add `#include <array>` if missing.)

`test/pivot_csv_gtest.cpp`: **replace** `TEST(PivotCsv, EqualRanksFollowHeaderOrder)` with:

```cpp
TEST(PivotCsv, EqualFlagsOnOneLineAreADeadRow)
{
	relation_store store = StoreFrom(kLookup);
	// Two names for the same flags; the left one always wins, so the right is dead.
	std::string error = Error(LoadPivot(store, "label,None,Injured,Crouch,Crouch|Injured,Injured|Crouch\n1,n,,,left,right\n"));
	EXPECT_TRUE(Mentions(error, "never picked")) << error;
	EXPECT_TRUE(Mentions(error, "lookup.csv:2")) << error;
	EXPECT_TRUE(Mentions(error, "covered by line 2")) << error;
}
```

`test/rows_csv_gtest.cpp`, append:

```cpp
TEST(RowsCsv, ADeadRowNamesTheLineThatCoversIt)
{
	auto s = SchemaFromYaml(R"(
tables:
  - name: reaction
    read_only: true
    pick: best
    inputs:
      - {name: energy, type: double, range: 0..1}
    columns:
      - {name: stimulus, type: hash, key: true}
      - {name: energy, type: band, input: energy}
      - {name: reaction, type: string}
    order_by: {columns: [stimulus]}
)", "reaction.yaml");
	ASSERT_TRUE(s.has_value()) << s.error().format();
	relation_store store(std::move(*s));
	auto loaded = LoadCsv(store, csv_rows{0, 1,
		"stimulus,energy,reaction\npredator,0..1,flee\npredator,0.2..1,fight\n", "reaction.csv"});
	ASSERT_FALSE(loaded.has_value());
	std::string const error = loaded.error().format();
	EXPECT_TRUE(Mentions(error, "reaction.csv:3")) << error;
	EXPECT_TRUE(Mentions(error, "covered by line 2")) << error;
	EXPECT_FALSE(Mentions(error, "same key as")) << error;
}
```

- [ ] **Step 2: Build and watch it fail.** Expected: `covered_by` missing (compile error); then the dead-row refusals fail.

- [ ] **Step 3: Implement**

`relation.h`: `load_refusal` from **Interfaces**.

`relation.cpp`, replace `// (Task 5: dead rows go here.)` with:

```cpp
			// Dead rows: B is never picked when the rows tried before it for
			// every input, among those with no script condition, cover its box.
			// Without a ranking column that is row order. With one, only the
			// same flags can: at input F = R_B, B is a full match with hit R_B,
			// and any other full match has R a strict subset of R_B, so a
			// smaller hit. Same flags tie by pivot column, then row; the R == 0
			// rows come last, in row order, after every ranked row.
			for (size_t b = start; b < end; ++b)
			{
				source const   B  = order[b];
				uint32_t const RB = flagsOf(B);
				auto triedBefore = [&](size_t a)
				{
					if (a == b || scripted(order[a]) || flagsOf(order[a]) != RB)
					{
						return false;
					}
					if (RB == 0)
					{
						return a < b;
					}
					uint16_t const pa = pivotOf(order[a]);
					uint16_t const pb = pivotOf(B);
					return pa < pb || (pa == pb && a < b);
				};
				// The rows tried before B, in try order, so the last is the one
				// tried just before it.
				std::vector<size_t> before;
				for (size_t a = start; a < end; ++a)
				{
					if (triedBefore(a))
					{
						before.push_back(a);
					}
				}
				std::stable_sort(before.begin(), before.end(), [&](size_t x, size_t y)
				{
					return RB != 0 && pivotOf(order[x]) < pivotOf(order[y]);
				});
				std::vector<detail::interval> own;
				boxOf(B, own);
				boxes.clear();
				for (size_t a : before)
				{
					boxOf(order[a], boxes);
				}
				if (detail::Uncovered(own, boxes, before.size()))
				{
					continue;   // some input reaches B
				}
				// B is dead. A survivor can be made dead only by a loaded row
				// ahead of it in tie order, so one of before is loaded.
				auto nameOf = [&](source s)
				{
					return s.loaded ? cc(std::string("loaded row "), s.row) : cc(std::string("owner "), current.owners[s.row], "'s row");
				};
				size_t lastLoaded = SIZE_MAX;
				for (size_t a : before)
				{
					if (order[a].loaded)
					{
						lastLoaded = order[a].row;
					}
				}
				if (command.refusal)
				{
					command.refusal->row        = B.loaded ? B.row : lastLoaded;
					command.refusal->covered_by = B.loaded ? lastLoaded : SIZE_MAX;
				}
				bool emptyBox = false;
				for (detail::interval const& in : own)
				{
					emptyBox = emptyBox || !(in.lo < in.hi);
				}
				if (emptyBox)
				{
					return tl::make_unexpected(ErrorContext("table '", def.name, "' key (", keyText(B), "): ",
						nameOf(B), " is never picked; its bands hold for no input"));
				}
				bool allLoaded = true;
				for (size_t a : before)
				{
					allLoaded = allLoaded && order[a].loaded;
				}
				std::string covering;
				for (size_t a : before)
				{
					covering = cc(covering, covering.empty() ? "" : ", ",
						allLoaded ? cc(std::string(), order[a].row) : nameOf(order[a]));
				}
				return tl::make_unexpected(ErrorContext("table '", def.name, "' key (", keyText(B), "): ",
					nameOf(B), " is never picked; ", allLoaded ? "loaded rows " : "", covering, " cover it"));
			}
```

The covering list names every row tried before B, which is a superset of a minimal cover; that is deliberate (it names exactly the rows that take B's place).

In `rows_csv.cpp` and `pivot_csv.cpp`, in the refusal mapping after `store.Load` fails, add after the `refusal.earlier` block:

```cpp
		if (refusal.covered_by < count)   // pivot_csv.cpp: < lineOf.size()
		{
			error = std::move(error).with("covered by line ", file.line_of_row(refusal.covered_by + 1));
			// pivot_csv.cpp: lineOf[refusal.covered_by]
		}
```

- [ ] **Step 4: Build and run** `$FILTER`. Expected: all pass.

- [ ] **Step 5: Mutation checks**

1. In `triedBefore`, change `flagsOf(order[a]) != RB` to `false`. Rebuild, run: `DifferentFlagsNeverShadow` fails cleanly. Restore, `touch src/relation.cpp`.
2. Change `return pa < pb || (pa == pb && a < b);` to `return a < b;`. Rebuild, run: `SameFlagsShadowInTieOrder` fails. Restore, `touch src/relation.cpp`.
3. Change `scripted(order[a]) ||` in `triedBefore` to nothing (delete it). Rebuild, run: `AScriptedEarlierRowShadowsNothing` fails. Restore, `touch src/relation.cpp`.

- [ ] **Step 6: Commit**: `Dead rows: a best-table row the rows tried before it always cover is refused`.

---

### Task 6: Documents and the full suite

**Files:**
- Modify: `Spehleon/Tabular/docs/definition.md`
- Modify: `Spehleon/Tabular/docs/feature-checklist.md`
- Move: the spec and this plan to `Spehleon/Tabular/docs/superpowers/landed/specs/` and `.../landed/plans/`

- [ ] **Step 1: `definition.md`**

- Under "What it promises", item 1, append: `Built for best tables (2026-10-17): in every key run a load adds rows to, the rows with no script condition (and, when the table ranks, no flags) cover every value of the band inputs' declared ranges; a row the rows tried before it always cover is refused as dead. Hash key domains are closed by a machine, which relates its keys to each other, so a key with no run is not checked until machines exist. An all or an or table may answer nothing.`
- In "A row", after "**At most one script condition per row;**" sentence group, add: `A row with no script condition carries a closure with no function; a blank condition cell in CSV is one.`
- In "Flag matching", replace "The row with `R == 0` is tried last, after every ranked row, and **must exist** in every key run a load adds rows to: it is the clear default." with "The row with `R == 0` is tried last, after every ranked row, and in every key run a load adds rows to the `R == 0` rows with no script condition must cover every input: they are the clear default (see promise 1)."
- In the "Column types" paragraph or after "Picking", add: `**Input ranges (owner, 2026-10-17).** A double input may declare range: lo..hi, half-open; a best table's band inputs must. A pick clamps the input into the range (the top end to the largest float below hi, so the top value is in the top band; NaN is left alone and holds no band), and a band reaching outside its input's range is refused.`
- In order-of-work item 3, after the CSV description, add: `Within one load, two texts with one hash, or a text hashing to 0, are refused, naming both lines.`

- [ ] **Step 2: `feature-checklist.md`**

- §1: `Refuse hash 0 and same-file hash collisions` → `[x]`, appending `(CSV front ends; within one load, any column)`.
- §4: `Totality over finite key domains…` → `[~] … (built per key run for best tables; key domains wait for machines, which close them)`. `Shadowed / unreachable rows refused` → `[x]` appending `(best tables; partial overlaps are priority)`.
- Consequences: `A missing default (R == 0) row is refused` → `[x]`, rewrite its trailing note to `checked for key runs a load adds rows to, as totality; a run left without one by removing rows picks nothing`. `Band gaps and overlaps are caught` → `[x]`, append `a gap is a totality refusal, a full overlap a dead row, a partial overlap priority`.

- [ ] **Step 3: Land the documents.** `git mv` the spec into `docs/superpowers/landed/specs/` and this plan into `docs/superpowers/landed/plans/`; set the spec's **Status** to `landed 2026-10-17 (approved by the owner the same day). See **As built** at the end for where the plan amended this spec.` and append an `## As built` section listing the plan's six amendments and anything the implementation changed further. Fix any reference to the old paths (`grep -rn "2026-10-17-totality" docs/`).

- [ ] **Step 4: Full suite and the dependency gate**

```bash
cd $WT && env -u GTEST_FILTER Build/Engine --gtest 2>&1 | tee $WT/../tabular-totality-suite.log | grep -E '^\[  (FAILED|PASSED|SKIPPED) |tests from .* ran'
env -C $WT PWD=$WT scripts/test-compile-dependencies.sh
```

Expected: 0 failed (the known flake `ScriptExpectedConnections.RollBackTouchedWorkingPortals` may fail once; re-run it alone and report both results); the dependency script exits 0. `relation.h` gained no includes; `relation_internal.h` gained `<charconv>`; `csv_cells.h` gained `<string>`, `<unordered_map>`, `<utility>`. If the gate flags one, follow `docs/compile-dependency-guards.json` rules in CLAUDE.md and report it rather than suppressing.

- [ ] **Step 5: Commit**: `Definition and checklist: totality and dead rows are built; spec and plan landed`.
