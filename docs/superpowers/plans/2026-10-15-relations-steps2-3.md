# Relations steps 2–3: the old tabular out, YAML and CSV loaders in

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove the first tabular (built up piece by piece in `src/Tabular`). Then give the relations core a YAML front end for table definitions and a CSV front end that loads rows into read-only tables.

**Architecture:** Each front end is its own file pair in `Spehleon/Tabular` and turns text into the step-1 command structs, so the core never sees a parser. The CSV loader needs one new core verb: `relation_store::Load`. It replaces one owner's rows in one table in a single step, sorts once and rebuilds the index once. A table marked `read_only` refuses `Insert`, so its rows come only from loads.

**Tech Stack:**
- C++20 and gtest, compiled into the Engine binary.
- `Spehleon/lib/Yaml` (`ParseTree` plus the `yaml_markup` `YamlSet` walker).
- `Spehleon/lib/Utility/load_csv.h` (`CSV::File`).
- `ThirdParty/cityhash` (`CityHash32`).

**Spec:** `Spehleon/Tabular/docs/definition.md`, "Order of work" items 2 and 3. Step 1's plan, `2026-10-15-relations-step1.md`, defines the core this plan extends.

## Decisions this plan makes (owner: confirm or strike)

1. **The old tabular is `src/Tabular` in the superproject, not `Spehleon/lib`.**
   - Size: 52 files, about 15k lines.
   - Hooks: Packages (two resource types and the watcher's cook destination), AngelScript registration, `engine.cpp`'s tick, a world `SlotMap`, and the asmjit build, whose only user is the rules JIT.
   - Old content: the `tabular_lamp` e2e package and the `bit_value` fixture.
   - All of it goes. Exception: the `ThirdParty/asmjit` submodule stays checked out but unbuilt. Machines may compile to machine code later (definition, "A machine is a SELECT"), and the build block comes back with them.
   - Shared AngelScript pieces it introduced stay; they have users of their own: `CompiledFrame`, `NameHash`, hashed paths. Only their comments citing the tabular specs are reworded.
2. **A read-only table is replaced, never updated (owner, 2026-10-15).** `create_table::read_only` marks it. Its rows live in an immutable, reference-counted version.
   - A reload builds the next version: the other owners' rows plus the reloading owner's new rows, sorted, indexed and fully checked (types, ranges, unique key).
   - Only a version that passes everything replaces the current one, by swapping a pointer. A refused load leaves the current version untouched.
   - The old version is never modified. A reader holding it keeps a consistent view (row numbers and lookups stay valid), and it's freed when the last holder lets go.
   - `Insert` is refused, and `Load` refuses a writable table: CSV-loaded tables and mutable tables are disjoint (owner, 2026-10-15; machines compile from the immutable ones). `Retract` on unload builds the version without that owner's rows and swaps it in the same way.
   - The store's readers (`Value`, `Find` and the rest) read the current version. `relation_store::Version(table)` hands out a `counted_ptr` to it for a reader that must hold one across a reload.
3. **`Load` is scoped to one table and builds a whole new version** (decision 2), so a failed load changes nothing. It fixes step 1's deferred I2 for bulk loads (sort once, index once) and M6 on this path (built aside, then swapped).
4. **CSV can't fill a table with a Condition column yet.** A condition is a host-compiled closure, and how the host compiles cell text is undecided. Such a table is refused by name.
5. **CSV cell text by type:**
   - Hash: `CityHash32` of the text, the same hash `NameHash` produces.
   - Flags: unsigned decimal or `0x` hex below 2^32.
   - Int: signed decimal.
   - Double: `std::from_chars`.
   - Command: the text verbatim.
   - The whole cell must parse, and an empty cell is refused.
   - `CSV::File` has no quoting, so a command cell can't contain a comma yet.
6. **The CSV header names columns, in any order.**
   - Every non-condition column appears exactly once.
   - An unknown, repeated or missing column is refused.
7. **YAML layout:** a top-level `tables:` sequence. Each entry has:
   - `name`
   - `columns`: a sequence of `{name, type, key}`, where `type` is one of `hash | flags | int | double | condition | command`
   - optional `order_by: {columns: [names], unique}`
   - optional `read_only`

   Order columns are given **by name** and resolved to numbers by the loader. Any diagnostic the YAML walker records, including an unknown key, fails the load, because a misspelled `uniqe:` that is silently ignored makes a non-unique table.

## Global Constraints

- Build with `env -C $WT PWD=$WT scripts/wt-build.sh`; test with `$WT/Build/Engine --gtest --gtest_filter=...`. Never use the main tree's binary.
- Brace every control-flow body. `snake_case` types are never `new`ed. Pass big objects as `const&` or `&&`.
- Return errors as `tl::expected<T, ErrorContext>`, never log them.
- New files go in `Spehleon/cmake/sources_tabular.cmake` (sources) and `cmake/sources_tests.cmake` (tests), never globbed.
- Commit leaves first: `Spehleon/Tabular`, then `Spehleon`, then the superproject (also `game/Lifaundi` for Task 1).
- `game/Lifaundi` carries unrelated dirt from main. Stage only the paths named here.
- Header dependency review: the new public headers include `tabular/relation.h` and `<string_view>`, nothing else. Parser headers stay in the `.cpp` files.

## Review Focus

1. A failed `Load` midway (a unique clash on the 900th row) must leave rows, index and generation untouched. Pinned in Task 2, `LoadIsAllOrNothing`.
2. A `Load` that changes nothing (the owner had no rows and loads none) must not bump the generation. Pinned in Task 2.
3. Two rows of one load with the same unique key: refuse, and name the owner. Pinned in Task 2.
4. CSV numbers with trailing junk (`12abc`, `1.5.2`), or a flag at exactly 2^32: refuse, and name the line. Pinned in Task 4.
5. A ragged CSV row (too few or too many cells) must give the author's line number, not the row index. Pinned in Task 4.

---

### Task 1: Remove the old tabular

**Files:**
- Delete: `src/Tabular/` (whole directory, including `notes.md` and `test/`)
- Delete: `scripts/test-tabular.sh`, `scripts/tabular_trace_gate.py`, `tests/test_tabular_trace_gate.py`, `tests/scripts/entry_tabular.as`, `tests/fixtures/tabular/`
- Delete (in `game/Lifaundi`): `Development/e2e-tests/tabular_lamp/`
- Modify: `cmake/sources_engine.cmake` (the 15 `src/Tabular` lines), `cmake/sources_tests.cmake` (the 17 `src/Tabular/test` lines)
- Modify: `CMakeLists.txt` (the asmjit block at about line 276–286, and the three `asmjit::asmjit` link lines)
- Modify: `src/AngelScript/registerscripts.cpp`, `src/AngelScript/typeid_list.h` (and `typeid_list.cpp` if it names `Tabular`), `src/engine.cpp`, `src/handle_list.h`, `src/SimulationState/worlddescription.h`
- Modify: `src/Packages/package_resourcetype.h`, `package_resourcetype.cpp`, `package_asset.cpp`, `package_bundle.cpp`, `package_fswatch.cpp`
- Modify docs:
  - `docs/project-notes/script-concurrency-ledger.tsv` (Tabular rows)
  - `docs/compile-dependency-guards.json` and `docs/project-notes/compile-dependency-cleanup.md` (any `src/Tabular` entries)
  - `docs/technical-debt.md` (drop `WorldDescription::tabulars` and the `Tabular` GC sentence from the open SlotMap row)
  - `CONTEXT.md` (terms defined only by the old tabular)
- Move to the superseded tree:
  - `docs/arcs/tables-as-queries.md` → `docs/superpowers/superseded/arcs/`
  - the three landed tabular specs (`2026-10-07-tabular-cook-and-lookup-parity`, `2026-10-08-tabular-attach`, `2026-10-09-tabular-state-and-rules`) → `superseded/specs/`
  - their three plans → `superseded/plans/`
  - Then repair every link to them.

**Interfaces:** consumes nothing; produces a tree with no `src/Tabular`, no `ResourceType::Tabular`/`RawTabularFile`, and no asmjit link.

`ResourceType` is safe to shorten: bundles store the type as its text tag (`package_bundle.cpp:143`), and `Tabular` is the last value before `BEGIN_RAW_FILES`.

- [ ] **Step 1: Delete the directory and the source-list lines.** Then build, and use the compile errors as the list of every hook that remains.

Run: `git rm -r src/Tabular scripts/test-tabular.sh scripts/tabular_trace_gate.py tests/test_tabular_trace_gate.py tests/scripts/entry_tabular.as tests/fixtures/tabular`, then remove the `src/Tabular` lines from both cmake lists, then `env -C $WT PWD=$WT scripts/wt-build.sh`.
Expected: FAIL. The errors are in the hook files listed above.

- [ ] **Step 2: Remove each hook.**
  - The `#include "Tabular/..."` lines, `asTypeId.Tabular = Tabular::asRegister(reg);`, `Tabular::OnTick(dt);`, `TabularHandle`, the `tabulars` SlotMap and its forward declaration, and the `int Tabular{}` typeid slot.
  - The two `ResourceType` enumerators and every case, name, extension, flag and mask line naming them.
  - In `package_fswatch.cpp`: `AddTabularDestination`, and the `|| type == ResourceType::Tabular` clause at about line 399.

  Where a removed case sat inside a `switch` that has a `default:`, delete the line. Never add a new `default:` to silence a warning.

- [ ] **Step 3: Remove the asmjit build.** Delete the `# Rules tables compile to machine code…` block through its `add_subdirectory`, and the `asmjit::asmjit` entries in the three `target_link_libraries` calls (with the comment above the first). Leave the `ThirdParty/asmjit` submodule as it is.

- [ ] **Step 4: Sweep the leftover references.**

Run: `git grep -n -i -E 'tabular' -- src cmake CMakeLists.txt scripts tests` and `grep -rn -i tabular game/Lifaundi --include=*.as --include=*.yaml --include=*.manifest`
Expected: only comments in `src/AngelScript` (`compiled_frame.h`, `name_hash.h`, `asregistration.h`, `Path/path_compiledpath.cpp` and the test files) that cite the old specs. Reword each to state the invariant without the citation; for example, `name_hash.h`: "CityHash32 of its UTF-8 bytes; tables store it, so it never changes without a format bump." Any code reference left is a hook Step 2 missed: remove it. Delete `game/Lifaundi/Development/e2e-tests/tabular_lamp/`, and remove any manifest or e2e list entry naming it.

- [ ] **Step 5: Build and run every gate the removal touches.**

Run:
- `env -C $WT PWD=$WT scripts/wt-build.sh`
- `env -u GTEST_FILTER $WT/Build/Engine --gtest > $LOG 2>&1; tail -5 $LOG`
- `scripts/test-concurrency-tags.sh $WT/Build/Engine`
- `scripts/test-compile-dependencies.sh`

Expected: the build is green. The suite has 0 failures; the count drops by the old tabular gtests, and the 50 `Relation*` tests still run. Concurrency tags are green after the ledger rows go. Compile dependencies are green after the guard entries go.

- [ ] **Step 6: Move the docs and repair the links.**

Run `git mv` for the seven docs into `docs/superpowers/superseded/{arcs,specs,plans}/`. Then `git grep -n -E 'tables-as-queries|tabular-cook|tabular-attach|tabular-state-and-rules' -- docs CONTEXT.md` and point every hit at the new path. Edit the technical-debt row and `CONTEXT.md` so they no longer describe code that is gone.
Expected: the grep shows only links into `superseded/`.

- [ ] **Step 7: Commit** (leaves first).

```bash
git -C game/Lifaundi add -A Development/e2e-tests/tabular_lamp
git -C game/Lifaundi commit -m "Remove the tabular_lamp e2e package: the first tabular is gone"
git add -A src cmake CMakeLists.txt scripts tests docs CONTEXT.md game/Lifaundi
git commit -m "Remove the first tabular (src/Tabular), its resource types, asmjit build and docs"
```

---

### Task 2: Read-only tables and the atomic `Load`

**Files:**
- Modify: `Spehleon/Tabular/include/tabular/relation.h`, `Spehleon/Tabular/src/relation.cpp`
- Test: `Spehleon/Tabular/test/relation_load_gtest.cpp` (new; add it to `cmake/sources_tests.cmake` after `relation_find_gtest.cpp`)

**Interfaces:**
- Produces, in `relation.h`:

```cpp
struct create_table
{
	std::string_view              name;
	std::span<add_column const>   columns;
	order_by const*               ordering  = nullptr;   // none: insertion order
	bool                          read_only = false;     // rows come only from Load
};

struct table_def
{
	std::string                  name;
	immutable_array<column_def>  columns;
	immutable_array<uint32_t>    order;    // empty: insertion order
	bool                         unique    = false;
	bool                         read_only = false;
};

// Replaces every row the owner has in one table with count new rows, in one
// step: a file reloaded is one load. Values and commands are row-major, one
// row's value columns (or Command columns) after another. A table with a
// Condition column cannot be loaded. On refusal the table is unchanged.
struct load_rows
{
	uint32_t                          table  = 0;
	uint64_t                          owner  = 0;
	size_t                            count  = 0;
	std::span<cell const>             values;     // count * value columns
	std::span<std::string_view const> commands;   // count * Command columns
};

// One immutable version of a read-only table. Built whole by Load or Retract,
// checked, then swapped in for the version before it; never modified after.
// Row numbers are valid for as long as the version is held.
class TableVersion
{
public:
	size_t   RowCount() const;
	uint64_t OwnerOf(size_t row) const;
	uint64_t Generation() const;
	tl::expected<cell, ErrorContext>                Value(size_t row, uint32_t column) const;
	tl::expected<std::string_view, ErrorContext>    CommandAt(size_t row, uint32_t column) const;
	tl::expected<std::vector<size_t>, ErrorContext> Find(uint32_t column, cell value) const;
	tl::expected<row_range, ErrorContext>           FindOrdered(std::span<cell const> prefix) const;

	void AddRef() const;
	void Release() const;
	// rows (the same row storage writable tables use), def, atomic refcount: private
};

// in relation_store:
	tl::expected<void, ErrorContext> Load(load_rows const& command);
	// The current version of a read-only table, for a reader that holds it
	// across reloads; refused for a writable table.
	tl::expected<counted_ptr<TableVersion const>, ErrorContext> Version(uint32_t table) const;
```

The store keeps one `counted_ptr<TableVersion const>` per read-only table, where it would otherwise keep that table's mutable rows. The store's own readers forward to the current version.

`Load` and `Retract` build the next `TableVersion` from the current one's surviving rows plus the new rows, run every check on it, and only then assign the slot. The version a reader holds is never touched.

Publishing the slot while another thread acquires from it is the acquire race on a reassignable slot (CLAUDE.md, `counted_ptr`). For this step, loads and readers run on one thread, and the header says so. Making the swap safe across threads is a later step, and it goes in `Spehleon/lib/Support`, not here.

- `Insert` on a `read_only` table is refused, and its closures are released (the existing `refuse` lambda).

- [ ] **Step 1: Write the failing tests** in `relation_load_gtest.cpp`.

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

// spawn_points(package hash key, section int, x double, note command), ordered by (package, section).
add_column const kColumns[] = {
	{"package", column_type::Hash,    true},
	{"section", column_type::Int,     false},
	{"x",       column_type::Double,  false},
	{"note",    column_type::Command, false},
};
uint32_t const kOrder[] = {0, 1};

relation_store Store(bool unique, bool read_only)
{
	static order_by orders[2] = {{kOrder, false}, {kOrder, true}};
	create_table const table{"spawn_points", kColumns, &orders[unique ? 1 : 0], read_only};
	auto s = BuildSchema(std::span<create_table const>(&table, 1));
	EXPECT_TRUE(s.has_value()) << s.error().message;
	return relation_store(std::move(*s));
}

// Rows of (package, section, x, note); returns the load command over the given buffers.
load_rows Rows(uint64_t owner, std::vector<cell>& values, std::vector<std::string_view>& commands,
	std::initializer_list<std::tuple<int64_t, int64_t, double, std::string_view>> rows)
{
	for (auto const& [package, section, x, note] : rows)
	{
		values.push_back(cell::Integer(package));
		values.push_back(cell::Integer(section));
		values.push_back(cell::Real(x));
		commands.push_back(note);
	}
	return load_rows{0, owner, rows.size(), values, commands};
}

std::vector<int64_t> Sections(relation_store const& store)
{
	std::vector<int64_t> out;
	for (size_t r = 0; r < store.RowCount(0); ++r)
	{
		out.push_back(store.Value(0, r, 1)->i);
	}
	return out;
}

}

TEST(RelationLoad, SchemaRecordsReadOnly)
{
	relation_store store = Store(false, true);
	EXPECT_TRUE(store.definition().tables()[0].read_only);
	EXPECT_FALSE(Store(false, false).definition().tables()[0].read_only);
}

TEST(RelationLoad, ReadOnlyTableRefusesInsert)
{
	relation_store store = Store(false, true);
	cell const values[] = {cell::Integer(1), cell::Integer(2), cell::Real(0.5)};
	std::string_view const commands[] = {"(none)"};
	auto inserted = store.Insert(insert_row{0, 7, values, {}, commands});
	ASSERT_FALSE(inserted.has_value());
	EXPECT_TRUE(Mentions(inserted.error().message, "read-only")) << inserted.error().message;
	EXPECT_EQ(store.RowCount(0), 0u);
}

TEST(RelationLoad, LoadSortsByTheOrderAndKeepsTiesInFileOrder)
{
	relation_store store = Store(false, true);
	std::vector<cell> v;
	std::vector<std::string_view> c;
	auto loaded = store.Load(Rows(1, v, c, {{5, 3, 0.0, "a"}, {5, 1, 0.0, "b"}, {2, 9, 0.0, "c"}, {5, 1, 0.0, "d"}}));
	ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
	EXPECT_EQ(Sections(store), (std::vector<int64_t>{9, 1, 1, 3}));
	EXPECT_EQ(*store.CommandAt(0, 1, 3), "b");
	EXPECT_EQ(*store.CommandAt(0, 2, 3), "d");
}

TEST(RelationLoad, ReloadReplacesOnlyThatOwnersRows)
{
	relation_store store = Store(false, true);
	std::vector<cell> v1, v2, v3;
	std::vector<std::string_view> c1, c2, c3;
	ASSERT_TRUE(store.Load(Rows(1, v1, c1, {{1, 1, 0.0, "one"}, {1, 2, 0.0, "one"}})).has_value());
	ASSERT_TRUE(store.Load(Rows(2, v2, c2, {{1, 3, 0.0, "two"}})).has_value());
	ASSERT_TRUE(store.Load(Rows(1, v3, c3, {{1, 4, 0.0, "one again"}})).has_value());
	EXPECT_EQ(Sections(store), (std::vector<int64_t>{3, 4}));
	EXPECT_EQ(store.OwnerOf(0, 0), 2u);
	EXPECT_EQ(store.OwnerOf(0, 1), 1u);
}

TEST(RelationLoad, LoadRebuildsTheKeyIndex)
{
	relation_store store = Store(false, true);
	std::vector<cell> v1, v2;
	std::vector<std::string_view> c1, c2;
	ASSERT_TRUE(store.Load(Rows(1, v1, c1, {{7, 1, 0.0, ""}, {8, 2, 0.0, ""}})).has_value());
	ASSERT_TRUE(store.Load(Rows(1, v2, c2, {{8, 5, 0.0, ""}, {7, 6, 0.0, ""}})).has_value());
	auto found = store.Find(0, 0, cell::Integer(8));
	ASSERT_TRUE(found.has_value()) << found.error().message;
	ASSERT_EQ(found->size(), 1u);
	EXPECT_EQ(store.Value(0, (*found)[0], 1)->i, 5);
}

TEST(RelationLoad, GenerationChangesOncePerLoadAndNotForAnEmptyOne)
{
	relation_store store = Store(false, true);
	uint64_t before = store.Generation(0);
	std::vector<cell> v, none;
	std::vector<std::string_view> c, noCommands;
	ASSERT_TRUE(store.Load(Rows(1, none, noCommands, {})).has_value());
	EXPECT_EQ(store.Generation(0), before);
	ASSERT_TRUE(store.Load(Rows(1, v, c, {{1, 1, 0.0, ""}, {1, 2, 0.0, ""}})).has_value());
	EXPECT_EQ(store.Generation(0), before + 1);
}

TEST(RelationLoad, LoadIsAllOrNothing)
{
	relation_store store = Store(true, true);
	std::vector<cell> v1, v2;
	std::vector<std::string_view> c1, c2;
	ASSERT_TRUE(store.Load(Rows(1, v1, c1, {{1, 1, 0.0, "kept"}})).has_value());
	uint64_t before = store.Generation(0);
	// Owner 2's second row clashes with owner 1's row: nothing of owner 2's lands.
	auto loaded = store.Load(Rows(2, v2, c2, {{1, 5, 0.0, ""}, {1, 1, 0.0, ""}}));
	ASSERT_FALSE(loaded.has_value());
	EXPECT_TRUE(Mentions(loaded.error().message, "owner 1")) << loaded.error().message;
	EXPECT_EQ(store.RowCount(0), 1u);
	EXPECT_EQ(store.Generation(0), before);
	EXPECT_EQ(store.Find(0, 0, cell::Integer(1))->size(), 1u);
}

TEST(RelationLoad, DuplicateKeyWithinOneLoadIsRefused)
{
	relation_store store = Store(true, true);
	std::vector<cell> v;
	std::vector<std::string_view> c;
	auto loaded = store.Load(Rows(3, v, c, {{4, 4, 0.0, ""}, {4, 4, 1.0, ""}}));
	ASSERT_FALSE(loaded.has_value());
	EXPECT_TRUE(Mentions(loaded.error().message, "owner 3")) << loaded.error().message;
	EXPECT_EQ(store.RowCount(0), 0u);
}

TEST(RelationLoad, ReloadingTheSameKeyUnderTheSameOwnerIsNotAClash)
{
	relation_store store = Store(true, true);
	std::vector<cell> v1, v2;
	std::vector<std::string_view> c1, c2;
	ASSERT_TRUE(store.Load(Rows(1, v1, c1, {{4, 4, 0.0, "old"}})).has_value());
	ASSERT_TRUE(store.Load(Rows(1, v2, c2, {{4, 4, 0.0, "new"}})).has_value());
	EXPECT_EQ(*store.CommandAt(0, 0, 3), "new");
}

TEST(RelationLoad, BadLoadsAreRefused)
{
	relation_store store = Store(false, true);
	cell const values[] = {cell::Integer(1), cell::Integer(1)};
	std::string_view const commands[] = {""};
	EXPECT_FALSE(store.Load(load_rows{0, 1, 1, values, commands}).has_value());   // short a value
	EXPECT_FALSE(store.Load(load_rows{9, 1, 0, {}, {}}).has_value());             // no such table

	cell const negativeHash[] = {cell::Integer(-1), cell::Integer(1), cell::Real(0)};
	auto hash = store.Load(load_rows{0, 1, 1, negativeHash, commands});
	ASSERT_FALSE(hash.has_value());
	EXPECT_TRUE(Mentions(hash.error().message, "'package'")) << hash.error().message;

	add_column const rules[] = {{"when", column_type::Condition, false}};
	create_table const table{"rules", rules, nullptr, true};
	relation_store withCondition(std::move(*BuildSchema(std::span<create_table const>(&table, 1))));
	auto conditions = withCondition.Load(load_rows{0, 1, 0, {}, {}});
	ASSERT_FALSE(conditions.has_value());
	EXPECT_TRUE(Mentions(conditions.error().message, "'when'")) << conditions.error().message;
}

TEST(RelationLoad, AHeldVersionIsNeverModified)
{
	relation_store store = Store(false, true);
	std::vector<cell> v1, v2;
	std::vector<std::string_view> c1, c2;
	ASSERT_TRUE(store.Load(Rows(1, v1, c1, {{1, 1, 0.0, "old"}, {1, 2, 0.0, "old"}})).has_value());
	auto held = store.Version(0);
	ASSERT_TRUE(held.has_value()) << held.error().message;

	ASSERT_TRUE(store.Load(Rows(1, v2, c2, {{1, 9, 0.0, "new"}})).has_value());
	store.Retract(retract_owner{1});

	// The reload and the retract made new versions; the held one still reads as loaded.
	ASSERT_EQ((*held)->RowCount(), 2u);
	EXPECT_EQ((*held)->Value(1, 1)->i, 2);
	EXPECT_EQ(*(*held)->CommandAt(0, 3), "old");
	EXPECT_EQ((*held)->Find(0, cell::Integer(1))->size(), 2u);
	EXPECT_EQ(store.RowCount(0), 0u);
}

TEST(RelationLoad, AWritableTableHasNoVersionAndRefusesLoad)
{
	relation_store store = Store(false, false);
	EXPECT_FALSE(store.Version(0).has_value());
	std::vector<cell> v;
	std::vector<std::string_view> c;
	auto loaded = store.Load(Rows(1, v, c, {{1, 1, 0.0, ""}}));
	ASSERT_FALSE(loaded.has_value());
	EXPECT_TRUE(Mentions(loaded.error().message, "not read-only")) << loaded.error().message;
}

TEST(RelationLoad, RetractStillRemovesLoadedRows)
{
	relation_store store = Store(false, true);
	std::vector<cell> v;
	std::vector<std::string_view> c;
	ASSERT_TRUE(store.Load(Rows(1, v, c, {{1, 1, 0.0, ""}})).has_value());
	store.Retract(retract_owner{1});
	EXPECT_EQ(store.RowCount(0), 0u);
}
```

- [ ] **Step 2: Run them to watch them fail.**

Run: build, then `$WT/Build/Engine --gtest --gtest_filter='RelationLoad.*'`
Expected: FAIL to compile, because `create_table` has no fourth member and `load_rows`/`Load` don't exist.

- [ ] **Step 3: Implement.**
  - Add the header members above.
  - In `BuildTable`: `table_def out{std::string(c.name), std::move(*columns), {}, false, c.read_only};`.
  - In `Insert`, right after the `def`/`rows` locals:

```cpp
	if (def.read_only)
	{
		return refuse(ErrorContext("table '", def.name, "' is read-only; its rows come only from Load"));
	}
```

  The code below shows `Load`'s checks and build over a table's rows. In the final shape, the build writes a new `TableVersion`'s rows instead of the three local vectors, and the closing swap is the slot assignment `rows.version = UncountedWrap(next)`. It is not three vector moves. Survivors' command strings are **copied** from the current version, never moved, because the current version may be held. `Retract` gets a read-only branch that builds a version the same way.

  `Load`, in `relation.cpp` after `Retract`:

```cpp
tl::expected<void, ErrorContext> relation_store::Load(load_rows const& command)
{
	auto defs = _storage->definition.tables();
	if (command.table >= defs.size())
	{
		return tl::make_unexpected(ErrorContext("load into table ", command.table, ", but the schema has ", defs.size(), " tables"));
	}
	table_def const& def  = defs[command.table];
	auto&            rows = _storage->tables[command.table];
	if (!def.read_only)
	{
		return tl::make_unexpected(ErrorContext("table '", def.name, "' is not read-only; its rows come from Insert"));
	}
	if (rows.conditionCount != 0)
	{
		for (column_def const& column : def.columns)
		{
			if (column.type == column_type::Condition)
			{
				return tl::make_unexpected(ErrorContext("table '", def.name, "' has condition column '", column.name,
					"', and a load carries no conditions"));
			}
		}
	}
	if (command.values.size() != command.count * rows.valueCount)
	{
		return tl::make_unexpected(ErrorContext("load into '", def.name, "' of ", command.count, " rows has ",
			command.values.size(), " values, but needs ", command.count * rows.valueCount));
	}
	if (command.commands.size() != command.count * rows.commandCount)
	{
		return tl::make_unexpected(ErrorContext("load into '", def.name, "' of ", command.count, " rows has ",
			command.commands.size(), " commands, but needs ", command.count * rows.commandCount));
	}
	for (size_t r = 0; r < command.count; ++r)
	{
		for (uint32_t c = 0; c < def.columns.size(); ++c)
		{
			if (IsValueColumn(def.columns[c].type))
			{
				auto fits = Check32(def.columns[c], def, command.values[r * rows.valueCount + rows.slot[c]].i);
				if (!fits)
				{
					return tl::make_unexpected(std::move(fits.error()).with("row ", r));
				}
			}
		}
	}

	// Every row the table will hold, as (from the load?, row): survivors first,
	// so a survivor sorts before a loaded row with the same key, as an earlier
	// insert would.
	struct source
	{
		bool   loaded;
		size_t row;
	};
	std::vector<source> order;
	order.reserve(rows.owners.size() + command.count);
	for (size_t r = 0; r < rows.owners.size(); ++r)
	{
		if (rows.owners[r] != command.owner)
		{
			order.push_back({false, r});
		}
	}
	size_t survivors = order.size();
	if (survivors == rows.owners.size() && command.count == 0)
	{
		return {};   // nothing of this owner's here, nothing new: no change
	}
	for (size_t r = 0; r < command.count; ++r)
	{
		order.push_back({true, r});
	}
	auto cellsOf = [&](source s) -> std::span<cell const>
	{
		return s.loaded ? command.values.subspan(s.row * rows.valueCount, rows.valueCount)
		                : std::span<cell const>(rows.cells).subspan(s.row * rows.valueCount, rows.valueCount);
	};
	auto ownerOf = [&](source s) { return s.loaded ? command.owner : rows.owners[s.row]; };
	if (!rows.orderSlots.empty())
	{
		auto keyCompare = [&](source a, source b) -> int
		{
			std::span<cell const> ac = cellsOf(a);
			std::span<cell const> bc = cellsOf(b);
			for (uint32_t s : rows.orderSlots)
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
					return tl::make_unexpected(ErrorContext("table '", def.name, "' has a unique order, and owner ",
						ownerOf(order[i - 1]), " already has a row with the key of owner ", ownerOf(order[i]), "'s"));
				}
			}
		}
	}

	// Built aside and swapped in, so a throw part-way leaves the table whole.
	std::vector<cell>        cells;
	std::vector<std::string> commands;
	std::vector<uint64_t>    owners;
	cells.reserve(order.size() * rows.valueCount);
	commands.reserve(order.size() * rows.commandCount);
	owners.reserve(order.size());
	for (source s : order)
	{
		std::span<cell const> from = cellsOf(s);
		cells.insert(cells.end(), from.begin(), from.end());
		for (uint32_t k = 0; k < rows.commandCount; ++k)
		{
			if (s.loaded)
			{
				commands.emplace_back(command.commands[s.row * rows.commandCount + k]);
			}
			else
			{
				commands.push_back(std::move(rows.commands[s.row * rows.commandCount + k]));
			}
		}
		owners.push_back(ownerOf(s));
	}
	rows.cells    = std::move(cells);
	rows.commands = std::move(commands);
	rows.owners   = std::move(owners);
	rows.RebuildIndex(def);
	++rows.generation;
	return {};
}
```

  Every refusal and every throw happens before the slot is assigned, so a failed load leaves the current version exactly as it was.

  Add `#include <algorithm>` if `relation.cpp` lacks it.

- [ ] **Step 4: Run them to watch them pass**, along with step 1's tests.

Run: build, then `$WT/Build/Engine --gtest --gtest_filter='Relation*'`
Expected: PASS. That is 50 step-1 tests plus 13 new ones.

- [ ] **Step 5: Mutation.** Delete `++rows.generation;` from `Load`, rebuild (bump the mtime on restore), and run `RelationLoad.*`.
Expected: `GenerationChangesOncePerLoadAndNotForAnEmptyOne` fails. Restore the line.

- [ ] **Step 6: Commit.**

```bash
git -C Spehleon/Tabular add include/tabular/relation.h src/relation.cpp test/relation_load_gtest.cpp
git -C Spehleon/Tabular commit -m "Relations: read-only tables and atomic Load of one owner's rows"
```
Then bump the gitlinks in `Spehleon` and the superproject, together with the `cmake/sources_tests.cmake` line.

---

### Task 3: YAML front end for table definitions

**Files:**
- Create: `Spehleon/Tabular/include/tabular/schema_yaml.h`, `Spehleon/Tabular/src/schema_yaml.cpp`
- Modify: `Spehleon/cmake/sources_tabular.cmake` (add `schema_yaml.cpp`)
- Test: `Spehleon/Tabular/test/schema_yaml_gtest.cpp` (add it to `cmake/sources_tests.cmake`)

**Interfaces:**
- Consumes: `create_table`, `add_column`, `order_by`, `BuildSchema` (Task 2's `read_only` included).
- Produces:

```cpp
// schema_yaml.h
#ifndef TABULAR_SCHEMA_YAML_H
#define TABULAR_SCHEMA_YAML_H

#include "tabular/relation.h"
#include <string_view>

namespace tabular
{

// A YAML document of table definitions, built into a schema. label names the
// source in errors. The document is
//
//   tables:
//     - name: spawn_points
//       columns:
//         - {name: package, type: hash, key: true}
//         - {name: x, type: double}
//       order_by: {columns: [package], unique: true}   # optional
//       read_only: true                                # optional
//
// and type is one of hash, flags, int, double, condition, command.
tl::expected<schema, ErrorContext> SchemaFromYaml(std::string_view text, std::string_view label);

}

#endif // TABULAR_SCHEMA_YAML_H
```

- [ ] **Step 1: Write the failing tests** in `schema_yaml_gtest.cpp`.

```cpp
#include <gtest/gtest.h>
#include "tabular/schema_yaml.h"

#include <string>
#include <string_view>

using namespace tabular;

static bool Mentions(std::string const& message, std::string_view text)
{
	return message.find(text) != std::string::npos;
}

namespace
{

constexpr std::string_view kSpawnPoints = R"(
tables:
  - name: spawn_points
    read_only: true
    columns:
      - {name: package, type: hash, key: true}
      - {name: section, type: hash}
      - {name: x, type: double}
      - {name: y, type: double}
    order_by: {columns: [section, package], unique: true}
  - name: rules
    columns:
      - {name: when, type: condition}
      - {name: then, type: command}
      - {name: mask, type: flags}
      - {name: count, type: int}
)";

}

TEST(SchemaYaml, BuildsEveryTableColumnAndOrder)
{
	auto s = SchemaFromYaml(kSpawnPoints, "spawn.yaml");
	ASSERT_TRUE(s.has_value()) << s.error().format();
	ASSERT_EQ(s->tables().size(), 2u);

	table_def const& spawn = s->tables()[0];
	EXPECT_EQ(spawn.name, "spawn_points");
	EXPECT_TRUE(spawn.read_only);
	ASSERT_EQ(spawn.columns.size(), 4u);
	EXPECT_EQ(spawn.columns[0].type, column_type::Hash);
	EXPECT_TRUE(spawn.columns[0].key);
	EXPECT_FALSE(spawn.columns[1].key);
	EXPECT_EQ(spawn.columns[3].type, column_type::Double);
	ASSERT_EQ(spawn.order.size(), 2u);
	EXPECT_EQ(spawn.order[0], 1u);   // by name: section is column 1
	EXPECT_EQ(spawn.order[1], 0u);
	EXPECT_TRUE(spawn.unique);

	table_def const& rules = s->tables()[1];
	EXPECT_FALSE(rules.read_only);
	EXPECT_EQ(rules.order.size(), 0u);
	EXPECT_EQ(rules.columns[0].type, column_type::Condition);
	EXPECT_EQ(rules.columns[1].type, column_type::Command);
	EXPECT_EQ(rules.columns[2].type, column_type::Flags);
	EXPECT_EQ(rules.columns[3].type, column_type::Int);
}

TEST(SchemaYaml, AnOrderNamingAMissingColumnIsRefused)
{
	auto s = SchemaFromYaml(R"(
tables:
  - name: t
    columns: [{name: a, type: int}]
    order_by: {columns: [b]}
)", "t.yaml");
	ASSERT_FALSE(s.has_value());
	EXPECT_TRUE(Mentions(s.error().format(), "'b'")) << s.error().format();
	EXPECT_TRUE(Mentions(s.error().format(), "t.yaml")) << s.error().format();
}

TEST(SchemaYaml, AnUnknownTypeIsRefused)
{
	auto s = SchemaFromYaml("tables:\n  - name: t\n    columns: [{name: a, type: string}]\n", "t.yaml");
	ASSERT_FALSE(s.has_value());
	EXPECT_TRUE(Mentions(s.error().format(), "string")) << s.error().format();
}

TEST(SchemaYaml, AMisspelledKeyIsRefusedNotIgnored)
{
	auto s = SchemaFromYaml(R"(
tables:
  - name: t
    columns: [{name: a, type: int}]
    order_by: {columns: [a], uniqe: true}
)", "t.yaml");
	ASSERT_FALSE(s.has_value());
	EXPECT_TRUE(Mentions(s.error().format(), "uniqe")) << s.error().format();
}

TEST(SchemaYaml, MissingRequiredPartsAreRefused)
{
	EXPECT_FALSE(SchemaFromYaml("tables:\n  - columns: [{name: a, type: int}]\n", "t.yaml").has_value());
	EXPECT_FALSE(SchemaFromYaml("tables:\n  - name: t\n", "t.yaml").has_value());
	EXPECT_FALSE(SchemaFromYaml("tables:\n  - name: t\n    columns: [{name: a}]\n", "t.yaml").has_value());
	EXPECT_FALSE(SchemaFromYaml("something: else\n", "t.yaml").has_value());
}

TEST(SchemaYaml, CoreRefusalsCarryTheLabel)
{
	// Two tables named t: BuildSchema refuses it, and the label says which file.
	auto s = SchemaFromYaml(R"(
tables:
  - {name: t, columns: [{name: a, type: int}]}
  - {name: t, columns: [{name: a, type: int}]}
)", "dup.yaml");
	ASSERT_FALSE(s.has_value());
	EXPECT_TRUE(Mentions(s.error().format(), "dup.yaml")) << s.error().format();
	EXPECT_TRUE(Mentions(s.error().format(), "'t'")) << s.error().format();
}

TEST(SchemaYaml, MalformedYamlIsRefused)
{
	auto s = SchemaFromYaml("tables: [\n", "bad.yaml");
	ASSERT_FALSE(s.has_value());
	EXPECT_TRUE(Mentions(s.error().format(), "bad.yaml")) << s.error().format();
}
```

- [ ] **Step 2: Run them to watch them fail.**
Expected: FAIL to compile (`tabular/schema_yaml.h` not found).

- [ ] **Step 3: Implement** `schema_yaml.cpp`.

```cpp
#include "tabular/schema_yaml.h"
#include "Yaml/yaml_markup.h"
#include "Yaml/yaml_parse.h"

#include <cstddef>
#include <string>
#include <vector>

namespace tabular
{

namespace
{

using LF::Yaml::FieldType;
using LF::Yaml::MakeContainer;
using LF::Yaml::YamlEnum;
using LF::Yaml::YamlField;
using LF::Yaml::YamlSet;

// The authored shape, read by offset; the views handed to BuildSchema point into it.
struct column_yaml
{
	std::string name;
	int32_t     type = -1;
	bool        key  = false;
};

struct order_yaml
{
	std::vector<std::string> columns;
	bool                     unique = false;
};

struct table_yaml
{
	std::string              name;
	std::vector<column_yaml> columns;
	order_yaml               order_by;
	bool                     read_only = false;
};

struct document_yaml
{
	std::vector<table_yaml> tables;
};

// In column_type's order: the index is the enumerator.
char const* const kTypeNames[] = {"hash", "flags", "int", "double", "condition", "command"};
YamlEnum const    kTypes{kTypeNames, 6};

YamlField const kColumnFields[] = {
	{"name", FieldType::String, offsetof(column_yaml, name), true},
	{"type", FieldType::Enum,   offsetof(column_yaml, type), true, 0, nullptr, nullptr, &kTypes},
	{"key",  FieldType::Bool,   offsetof(column_yaml, key)},
};
YamlSet const kColumnSet{kColumnFields, nullptr, "column"};

YamlField const kOrderColumn{"", FieldType::String, 0};
YamlField const kOrderFields[] = {
	{"columns", FieldType::Sequence, offsetof(order_yaml, columns), true,
	 0, nullptr, &kOrderColumn, nullptr, MakeContainer<std::vector<std::string>>()},
	{"unique", FieldType::Bool, offsetof(order_yaml, unique)},
};
YamlSet const kOrderSet{kOrderFields, nullptr, "order_by"};

YamlField const kTableFields[] = {
	{"name",      FieldType::String,   offsetof(table_yaml, name), true},
	{"columns",   FieldType::Sequence, offsetof(table_yaml, columns), true,
	 0, &kColumnSet, nullptr, nullptr, MakeContainer<std::vector<column_yaml>>()},
	{"order_by",  FieldType::Nested,   offsetof(table_yaml, order_by), false, 0, &kOrderSet},
	{"read_only", FieldType::Bool,     offsetof(table_yaml, read_only)},
};
YamlSet const kTableSet{kTableFields, nullptr, "table"};

YamlField const kDocumentFields[] = {
	{"tables", FieldType::Sequence, offsetof(document_yaml, tables), true,
	 0, &kTableSet, nullptr, nullptr, MakeContainer<std::vector<table_yaml>>()},
};
YamlSet const kDocumentSet{kDocumentFields, nullptr, "table definitions"};

}

tl::expected<schema, ErrorContext> SchemaFromYaml(std::string_view text, std::string_view label)
{
	TreeErrorContext errors;
	LF::Yaml::NodePtr root = LF::Yaml::ParseTree(text, errors, label);
	document_yaml     document;
	if (root)
	{
		LF::Yaml::ReadSet(&document, *root, kDocumentSet, errors);
	}
	// Any diagnostic fails the load, an unknown key included: a misspelled
	// "uniqe" silently ignored is a table that is not unique.
	if (errors.has_errors() || !root)
	{
		return tl::make_unexpected(ErrorContext(errors.format()).with(std::string(label)));
	}

	// Which tables wrote order_by: the walker leaves an absent mapping at its default.
	LF::Yaml::Node const* tableNodes = root->get("tables");

	std::vector<std::vector<add_column>> columns(document.tables.size());
	std::vector<std::vector<uint32_t>>   orderColumns(document.tables.size());
	std::vector<order_by>                orders(document.tables.size());
	std::vector<create_table>            commands;
	commands.reserve(document.tables.size());
	for (size_t t = 0; t < document.tables.size(); ++t)
	{
		table_yaml const& in = document.tables[t];
		for (column_yaml const& c : in.columns)
		{
			columns[t].push_back(add_column{c.name, static_cast<column_type>(c.type), c.key});
		}
		bool ordered = tableNodes->as_sequence().items[t]->get("order_by") != nullptr;
		if (ordered)
		{
			for (std::string const& name : in.order_by.columns)
			{
				uint32_t found = UINT32_MAX;
				for (uint32_t c = 0; c < in.columns.size(); ++c)
				{
					if (in.columns[c].name == name)
					{
						found = c;
					}
				}
				if (found == UINT32_MAX)
				{
					return tl::make_unexpected(ErrorContext("the order of table '", in.name, "' names column '", name,
						"', which the table does not have").with(std::string(label)));
				}
				orderColumns[t].push_back(found);
			}
			orders[t] = order_by{orderColumns[t], in.order_by.unique};
		}
		commands.push_back(create_table{in.name, columns[t], ordered ? &orders[t] : nullptr, in.read_only});
	}

	auto built = BuildSchema(commands);
	if (!built)
	{
		return tl::make_unexpected(std::move(built.error()).with(std::string(label)));
	}
	return std::move(*built);
}

}
```

  If `ReadSet` reports an absent `order_by` differently (for example, it records nothing and leaves the default), the presence check above still holds, because it reads the node tree, not the struct. If `YamlField`'s aggregate order differs from what's written here, follow `yaml_markup.h`: `{key, type, offset, required, count, child, elem, enums, container}`.

- [ ] **Step 4: Run them to watch them pass.**

Run: build, then `$WT/Build/Engine --gtest --gtest_filter='SchemaYaml.*:Relation*'`
Expected: PASS.

- [ ] **Step 5: Mutation.** Change the failure check to `if (!root)`, so diagnostics are ignored.
Expected: `AMisspelledKeyIsRefusedNotIgnored` and `AnUnknownTypeIsRefused` fail. Restore the check, and bump the mtime.

- [ ] **Step 6: Commit** (Tabular first, then the gitlink bumps with `sources_tabular.cmake` and `sources_tests.cmake`).

```bash
git -C Spehleon/Tabular add include/tabular/schema_yaml.h src/schema_yaml.cpp test/schema_yaml_gtest.cpp
git -C Spehleon/Tabular commit -m "Relations: YAML front end for table definitions"
```

---

### Task 4: CSV front end into read-only tables

**Files:**
- Create: `Spehleon/Tabular/include/tabular/rows_csv.h`, `Spehleon/Tabular/src/rows_csv.cpp`
- Modify: `Spehleon/cmake/sources_tabular.cmake` (add `rows_csv.cpp`)
- Test: `Spehleon/Tabular/test/rows_csv_gtest.cpp` (add it to `cmake/sources_tests.cmake`)

**Interfaces:**
- Consumes: `relation_store::Load`, `load_rows` (Task 2), `SchemaFromYaml` (Task 3, tests only).
- Produces:

```cpp
// rows_csv.h
#ifndef TABULAR_ROWS_CSV_H
#define TABULAR_ROWS_CSV_H

#include "tabular/relation.h"
#include <string_view>

namespace tabular
{

// CSV text whose header row names the table's columns, in any order; every
// column but a Condition one appears exactly once. Cells are read by their
// column's type: a hash is CityHash32 of the text, flags an unsigned decimal
// or 0x hex below 2^32, an int a signed decimal, a double as from_chars reads
// it, a command verbatim. The rows replace the owner's rows in the table, all
// or none.
struct csv_rows
{
	uint32_t         table      = 0;
	uint64_t         owner      = 0;
	std::string_view text;
	std::string_view label;            // names the source in errors
	uint32_t         first_line = 1;   // the source line text starts on
};

tl::expected<void, ErrorContext> LoadCsv(relation_store& store, csv_rows const& command);

}

#endif // TABULAR_ROWS_CSV_H
```

- [ ] **Step 1: Write the failing tests** in `rows_csv_gtest.cpp`.

```cpp
#include <gtest/gtest.h>
#include "tabular/rows_csv.h"
#include "tabular/schema_yaml.h"
#include "cityhash/src/city.h"

#include <string>
#include <string_view>

using namespace tabular;

static bool Mentions(std::string const& message, std::string_view text)
{
	return message.find(text) != std::string::npos;
}

namespace
{

constexpr std::string_view kSchema = R"(
tables:
  - name: spawn_points
    read_only: true
    columns:
      - {name: package, type: hash, key: true}
      - {name: section, type: int}
      - {name: mask, type: flags}
      - {name: x, type: double}
      - {name: on_spawn, type: command}
    order_by: {columns: [package, section], unique: true}
  - name: rules
    columns:
      - {name: when, type: condition}
)";

relation_store Store()
{
	auto s = SchemaFromYaml(kSchema, "schema.yaml");
	EXPECT_TRUE(s.has_value()) << s.error().format();
	return relation_store(std::move(*s));
}

int64_t Hash(std::string_view name)
{
	return static_cast<int64_t>(CityHash32(name.data(), name.size()));
}

std::string Load(relation_store& store, std::string_view text, uint64_t owner = 1, uint32_t firstLine = 1)
{
	auto loaded = LoadCsv(store, csv_rows{0, owner, text, "spawn.csv", firstLine});
	return loaded ? std::string() : loaded.error().format();
}

}

TEST(RowsCsv, ReadsEveryTypeInHeaderOrder)
{
	relation_store store = Store();
	ASSERT_EQ(Load(store,
		"x, on_spawn, package, section, mask\n"
		"1.5, (spawn grazer), meadow, 2, 0x10\n"
		"-0.25, (spawn moth), meadow, -1, 3\n"), "");
	ASSERT_EQ(store.RowCount(0), 2u);
	// Sorted by (package, section): section -1 first.
	EXPECT_EQ(store.Value(0, 0, 0)->i, Hash("meadow"));
	EXPECT_EQ(store.Value(0, 0, 1)->i, -1);
	EXPECT_EQ(store.Value(0, 0, 2)->i, 3);
	EXPECT_EQ(store.Value(0, 0, 3)->d, -0.25);
	EXPECT_EQ(*store.CommandAt(0, 0, 4), "(spawn moth)");
	EXPECT_EQ(store.Value(0, 1, 2)->i, 16);
	EXPECT_EQ(store.Value(0, 1, 3)->d, 1.5);
}

TEST(RowsCsv, ReloadReplacesTheOwnersRows)
{
	relation_store store = Store();
	std::string_view header = "package,section,mask,x,on_spawn\n";
	ASSERT_EQ(Load(store, std::string(header) + "a,1,0,0,()\na,2,0,0,()\n"), "");
	uint64_t before = store.Generation(0);
	ASSERT_EQ(Load(store, std::string(header) + "a,3,0,0,()\n"), "");
	EXPECT_EQ(store.RowCount(0), 1u);
	EXPECT_EQ(store.Value(0, 0, 1)->i, 3);
	EXPECT_EQ(store.Generation(0), before + 1);
	EXPECT_EQ(store.Find(0, 0, cell::Integer(Hash("a")))->size(), 1u);
}

TEST(RowsCsv, BadCellsNameTheLineAndColumn)
{
	relation_store store = Store();
	std::string_view header = "package,section,mask,x,on_spawn\n";
	struct bad { std::string_view row; std::string_view mentions; };
	bad const cases[] = {
		{"a,12abc,0,0,()", "'section'"},
		{"a,1,0,1.5.2,()", "'x'"},
		{"a,1,0x100000000,0,()", "'mask'"},
		{"a,1,-1,0,()", "'mask'"},
		{"a,,0,0,()", "'section'"},
		{",1,0,0,()", "'package'"},
	};
	for (bad const& b : cases)
	{
		std::string error = Load(store, std::string(header) + "# a comment line\n" + std::string(b.row) + "\n", 1, 10);
		EXPECT_TRUE(Mentions(error, b.mentions)) << b.row << " -> " << error;
		EXPECT_TRUE(Mentions(error, "spawn.csv:12")) << b.row << " -> " << error;
		EXPECT_EQ(store.RowCount(0), 0u) << b.row;
	}
}

TEST(RowsCsv, RaggedRowsNameTheirLine)
{
	relation_store store = Store();
	std::string error = Load(store, "package,section,mask,x,on_spawn\na,1,0,0,()\n\na,2,0\n");
	EXPECT_TRUE(Mentions(error, "spawn.csv:4")) << error;
	EXPECT_TRUE(Mentions(error, "3 cells")) << error;
	EXPECT_EQ(store.RowCount(0), 0u);
}

TEST(RowsCsv, HeaderMustNameEveryColumnOnce)
{
	relation_store store = Store();
	EXPECT_TRUE(Mentions(Load(store, "package,section,mask,x\n"), "'on_spawn'"));
	EXPECT_TRUE(Mentions(Load(store, "package,section,mask,x,on_spawn,extra\n"), "'extra'"));
	EXPECT_TRUE(Mentions(Load(store, "package,package,section,mask,x,on_spawn\n"), "'package'"));
	EXPECT_TRUE(Mentions(Load(store, ""), "header"));
}

TEST(RowsCsv, AHeaderWithNoRowsEmptiesTheOwner)
{
	relation_store store = Store();
	ASSERT_EQ(Load(store, "package,section,mask,x,on_spawn\na,1,0,0,()\n"), "");
	ASSERT_EQ(Load(store, "package,section,mask,x,on_spawn\n"), "");
	EXPECT_EQ(store.RowCount(0), 0u);
}

TEST(RowsCsv, UniqueClashNamesTheFile)
{
	relation_store store = Store();
	std::string error = Load(store, "package,section,mask,x,on_spawn\na,1,0,0,()\na,1,0,0,()\n");
	EXPECT_TRUE(Mentions(error, "spawn.csv")) << error;
	EXPECT_TRUE(Mentions(error, "unique")) << error;
}

TEST(RowsCsv, TablesWithConditionsAreRefused)
{
	relation_store store = Store();
	auto loaded = LoadCsv(store, csv_rows{1, 1, "when\nx\n", "rules.csv"});
	ASSERT_FALSE(loaded.has_value());
	EXPECT_TRUE(Mentions(loaded.error().format(), "'when'")) << loaded.error().format();
}
```

- [ ] **Step 2: Run them to watch them fail.**
Expected: FAIL to compile (`tabular/rows_csv.h` not found).

- [ ] **Step 3: Implement** `rows_csv.cpp`.

```cpp
#include "tabular/rows_csv.h"
#include "Utility/load_csv.h"
#include "cityhash/src/city.h"

#include <charconv>
#include <string>
#include <vector>

namespace tabular
{

namespace
{

// The whole of text as T, or nothing: "12abc" is not 12.
template<typename T>
bool ParseWhole(std::string_view text, T& out, int base = 10)
{
	char const* end = text.data() + text.size();
	std::from_chars_result r;
	if constexpr (std::is_floating_point_v<T>)
	{
		r = std::from_chars(text.data(), end, out);
	}
	else
	{
		r = std::from_chars(text.data(), end, out, base);
	}
	return r.ec == std::errc() && r.ptr == end;
}

tl::expected<cell, ErrorContext> ReadCell(column_def const& column, std::string_view text)
{
	if (text.empty())
	{
		return tl::make_unexpected(ErrorContext("column '", column.name, "' is empty"));
	}
	switch (column.type)
	{
	case column_type::Hash:
		return cell::Integer(static_cast<int64_t>(CityHash32(text.data(), text.size())));
	case column_type::Flags:
	{
		uint64_t v    = 0;
		bool     hex  = text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
		bool     read = hex ? ParseWhole(text.substr(2), v, 16) : ParseWhole(text, v);
		if (!read || v > UINT64_C(0xFFFFFFFF))
		{
			return tl::make_unexpected(ErrorContext("column '", column.name, "' is a flag set, unsigned decimal or 0x hex below 2^32; got '", text, "'"));
		}
		return cell::Integer(static_cast<int64_t>(v));
	}
	case column_type::Int:
	{
		int64_t v = 0;
		if (!ParseWhole(text, v))
		{
			return tl::make_unexpected(ErrorContext("column '", column.name, "' is an integer; got '", text, "'"));
		}
		return cell::Integer(v);
	}
	case column_type::Double:
	{
		double v = 0;
		if (!ParseWhole(text, v))
		{
			return tl::make_unexpected(ErrorContext("column '", column.name, "' is a number; got '", text, "'"));
		}
		return cell::Real(v);
	}
	default:
		return tl::make_unexpected(ErrorContext("column '", column.name, "' holds no value"));
	}
}

}

tl::expected<void, ErrorContext> LoadCsv(relation_store& store, csv_rows const& command)
{
	auto label = [&](uint32_t line) { return cc(std::string(), command.label, ":", line); };
	auto defs  = store.definition().tables();
	if (command.table >= defs.size())
	{
		return tl::make_unexpected(ErrorContext("no table ", command.table).with(std::string(command.label)));
	}
	table_def const& def = defs[command.table];
	for (column_def const& column : def.columns)
	{
		if (column.type == column_type::Condition)
		{
			return tl::make_unexpected(ErrorContext("table '", def.name, "' has condition column '", column.name,
				"', which CSV cannot fill").with(std::string(command.label)));
		}
	}

	// Each column's position among the columns of its kind, as the store numbers them.
	std::vector<uint32_t> slot(def.columns.size());
	uint32_t valueCount   = 0;
	uint32_t commandCount = 0;
	for (uint32_t c = 0; c < def.columns.size(); ++c)
	{
		slot[c] = def.columns[c].type == column_type::Command ? commandCount++ : valueCount++;
	}

	CSV::File file(command.text, CSV::Options{}, command.first_line);
	if (file.rows() == 0)
	{
		return tl::make_unexpected(ErrorContext("no header row naming the columns of '", def.name, "'")
			.with(std::string(command.label)));
	}

	// Header cell -> column.
	size_t const          width = file[0].size();
	std::vector<uint32_t> columnOf(width);
	std::vector<bool>     named(def.columns.size(), false);
	for (size_t h = 0; h < width; ++h)
	{
		std::string_view name  = file.cell(h, 0);
		auto             found = store.definition().FindColumn(command.table, name);
		if (!found)
		{
			return tl::make_unexpected(std::move(found.error()).with(label(file.line_of_row(0))));
		}
		if (named[*found])
		{
			return tl::make_unexpected(ErrorContext("the header names column '", name, "' twice").with(label(file.line_of_row(0))));
		}
		named[*found] = true;
		columnOf[h]   = *found;
	}
	for (uint32_t c = 0; c < def.columns.size(); ++c)
	{
		if (!named[c])
		{
			return tl::make_unexpected(ErrorContext("the header does not name column '", def.columns[c].name, "'")
				.with(label(file.line_of_row(0))));
		}
	}

	size_t const                  count = file.rows() - 1;
	std::vector<cell>             values(count * valueCount);
	std::vector<std::string_view> commands(count * commandCount);
	for (size_t r = 0; r < count; ++r)
	{
		uint32_t line = file.line_of_row(r + 1);
		if (file[r + 1].size() != width)
		{
			return tl::make_unexpected(ErrorContext("row has ", file[r + 1].size(), " cells, but the header has ", width)
				.with(label(line)));
		}
		for (size_t h = 0; h < width; ++h)
		{
			column_def const& column = def.columns[columnOf[h]];
			std::string_view  text   = file.cell(h, r + 1);
			if (column.type == column_type::Command)
			{
				commands[r * commandCount + slot[columnOf[h]]] = text;
				continue;
			}
			auto value = ReadCell(column, text);
			if (!value)
			{
				return tl::make_unexpected(std::move(value.error()).with(label(line)));
			}
			values[r * valueCount + slot[columnOf[h]]] = *value;
		}
	}

	auto loaded = store.Load(load_rows{command.table, command.owner, count, values, commands});
	if (!loaded)
	{
		return tl::make_unexpected(std::move(loaded.error()).with(std::string(command.label)));
	}
	return {};
}

}
```

  `cc` is in `Support/strings.h`, which `Errors/errorcontext.h` already includes. The `commands` views point into `file`, which lives until `Load` returns; that is all the store needs, since it copies.

- [ ] **Step 4: Run them to watch them pass.**

Run: build, then `$WT/Build/Engine --gtest --gtest_filter='RowsCsv.*:SchemaYaml.*:Relation*'`
Expected: PASS.

- [ ] **Step 5: Mutation.** In `ParseWhole`, drop `&& r.ptr == end`.
Expected: `BadCellsNameTheLineAndColumn` fails on `12abc` and `1.5.2`. Restore it, and bump the mtime.

- [ ] **Step 6: Commit, then run the whole suite once.**

```bash
git -C Spehleon/Tabular add include/tabular/rows_csv.h src/rows_csv.cpp test/rows_csv_gtest.cpp
git -C Spehleon/Tabular commit -m "Relations: CSV front end loading rows into read-only tables"
```
Bump the gitlinks along with the cmake lines. Then run `env -u GTEST_FILTER $WT/Build/Engine --gtest > $LOG 2>&1` and `scripts/test-e2e-all.sh $WT/Build/Engine`, and record both counts.

- [ ] **Step 7: Docs.** In `definition.md`:
  - Mark order-of-work items 2 and 3 as built.
  - Record decisions 2, 4 and 5 under the Relations section.
  - Remove "whether a string column exists at all or is always a hash" from Undecided **only if** the owner rules on it at review; otherwise leave it.

  Commit in Tabular, then bump.
