# Relations: the column set

**Status:** landed 2026-10-16 (approved by the owner the same day).

**Contract:** `Spehleon/Tabular/docs/definition.md`. This slice builds its "Column types beyond the
four native ones" paragraph and the condition sentence of "Order of work" item 3.

## Goal

Every column type Tabular has can be declared in YAML, filled from CSV, held by a read-only
version and read back. Three additions get there:

1. a **string** column holding a `counted_string`;
2. a **`vec2`** column holding two floats;
3. **condition cells loaded from CSV** through a function the caller passes, which needs read-only
   `Load` to accept condition columns (it refuses them today).

## Rulings this rests on (owner, 2026-10-16)

- A string column holds a `counted_string`.
- `vec2` (two floats) is the only vector type; there is no `vec4`, `vec3` or `Location`. A 2D
  position is a string column naming the Section plus a `vec2` column; a 3D one adds a second
  `vec2` (`zw`) or a `double`. A band `a..b` is a `vec2`, as animation's phase bands are floats.
- Script conditions reach the host only through the loader's condition function, given cell text
  and nothing else. Native conditions never go through it (they are a later slice).
- Handles are `uint64_t` punned into an `int64_t` column; nothing to build.

## Out of scope

Native conditions (bands, flag subsets) and evaluating them; picking; totality checks; quoting in
CSV; lexical ordering of strings; strings or `vec2` as keys of an order; anything host-side.

## Design

### Cell layout stays one 8-byte cell per value column

`vec2` is two floats, exactly 8 bytes, so it is a value column like `int` or `double`. The `cell`
union gains `float v[2]` (not `glm::vec2`, whose constructors would make the union non-trivial)
and a `cell::Vector(float x, float y)` maker; readers hand it out as a `cell`, read through `v`.
`cell` stays trivially copyable and `static_assert(sizeof(cell) == 8)` holds.

### Strings are a slot kind of their own

A row today is split by column kind: value columns in the table's cell array, command text in a
per-table array of strings, conditions as closures. Strings get a fourth per-table array, of
`counted_string`, numbered by string column the way commands are numbered by command column.

Raw pointer bits in the cell union were rejected: every copy of a cell would have to know its
column's type to add or drop a reference, which needs a raw-handle API on `counted_string` and is
how leaks and double frees are made. Here reference counting stays inside `counted_string`'s own
copy and destruction, and copying a survivor into a new version is an ordinary copy.

- **Writes.** `insert_row` and `load_rows` gain `std::span<string_in const> strings`, one per
  string column per row, row-major like `commands`. `string_in` is a small view implicitly
  constructible from a `std::string_view` or from a `counted_string const&` (owner, 2026-10-16: a
  caller that already holds a `counted_string` passes it as it is), so
  `std::vector<string_in>(handles.begin(), handles.end())` adapts an array of either. The store
  interns each with `counted_string::MakeShared` of its bytes. A `counted_string` already interned
  in the default table comes back as the same handle, so nothing is copied; one interned in another
  table, or unique, is interned by content, because under `HAVE_IMGUI` each intern table dedups
  separately and only the default table's handles may be compared by identity.
- **Interning makes identity equality.** Interned strings with the same content share one handle,
  so two string cells are equal exactly when their handles are. A string column may therefore be a
  **key**. As built, the index keys on the content (a `string_view` into the held handle's bytes,
  which never move while a row holds them), so `Find(table, column, std::string_view)` (an
  overload, on the store and as `Find(column, std::string_view)` on `TableVersion`) looks the probe
  up without interning it and a lookup never grows the intern table.
- **Not in an order.** `BuildSchema` refuses a string column in `order_by`: handle order is
  arbitrary, and lexical order is not needed yet.
- **Reads.** `StringAt(table, row, column) -> tl::expected<counted_string, ErrorContext>` on the
  store and `StringAt(row, column)` on `TableVersion`. `Value` refuses a string column, as it
  refuses condition and command columns.

### `vec2` restrictions

`BuildSchema` refuses a `vec2` column as a key or in `order_by`. Float bit-equality is a poor key
(`-0` against `0`, NaN), and nothing needs one.

### Conditions in read-only tables

`load_rows` gains `std::span<closure const> conditions`, one per condition column per row,
row-major. The plain `closure` cannot be shared, but a survivor's condition is held by both the
old version and the new one, and `release` must run exactly once. So the store wraps each adopted
closure in a small refcounted holder (`counted_ptr`, atomic count) whose destructor calls
`release`; versions hold the holders. `release` then runs when the last version holding the row is
destroyed, whichever that is.

- **Adoption.** `Load` adopts every closure passed to it, succeed or fail, as `Insert` already
  does: on a refusal the closures it was given are released, so a caller never has to.
- **Retract.** Loading no rows for an owner is how `Retract` clears a read-only table. Today it
  skips tables with condition columns, because `Load` refuses them; with this slice that skip goes
  and every read-only table is cleared through `Load`.
- **Reads.** `TableVersion` gains `Holds(row, column, this_in)`, as the store has; the store's
  `Holds` reads read-only tables too.
- Writable tables are unchanged: `Insert` already adopts closures.

### YAML

The column `type` gains `string` and `vec2`. Every other rule stands: any diagnostic, an unknown
key included, fails the definition.

### CSV

- **String cell:** the cell text, trimmed like every cell; an empty cell is refused like every
  cell.
- **`vec2` cell:** `a..b`, two floats read whole with `from_chars`, the one format for every
  `vec2`, a position included (`12.5..3`). Anything else, `a..` or `..b` or a third part, is
  refused naming the column and the text.
- **Condition cell:** `csv_rows` gains
  `std::function<tl::expected<closure, ErrorContext>(std::string_view)> condition`. Each condition
  cell's text is passed to it; an error comes back with the cell's line and column added. A CSV for
  a table with condition columns and no function is refused before any cell is read. The function
  returns an error rather than a bare closure because errors are returned, not logged, and a bare
  closure has no way to say a cell did not compile.
- **Ownership across a failure.** Closures built for earlier cells are held by a guard that releases them on any early return; `Load` adopts them all once called.

## Error handling

All refusals return `ErrorContext` and change nothing: a refused `Load` leaves the current version
in place; a refused CSV names its line (and, for a unique clash, the line it clashes with), as
today. No new log calls.

## Tests (gtest, in `Spehleon/Tabular/test/`)

Each names the production change that would turn it red.

1. A string column round-trips through YAML, CSV, `Load` and `StringAt`, on the store and on a held
   version; and through `Insert` on a writable table, where the row's strings survive a `Retract`
   of another owner.
2. A string key column is found by content, including from a probe built separately from the
   loaded text (identity through interning, not through the caller's buffer).
2b. A row inserted with a `counted_string` stores that same handle when it is interned
   (`ptr_compare == 0`), and a unique `counted_string` with the same text is found by the same key.
3. A `vec2` column round-trips through YAML, CSV, `Load` and `Value`.
4. `BuildSchema` refuses a string in `order_by`, and a `vec2` as a key or in `order_by`.
5. `vec2` CSV cells `1..`, `..2`, `1..2..3`, `1,2` and `a..b` are refused naming the column.
6. `release` runs exactly once for a condition: after a reload that keeps the row (survivor
   shared, then old version dropped), after a refused `Load`, after `Retract`, after the store is
   destroyed while a version is still held and then the version is dropped. A counting closure
   pins each.
7. A CSV condition function's error names the line; closures built for earlier cells are released
   (counted).
8. A CSV for a condition table with no function is refused before the function would have been
   called.
9. `Retract` clears a read-only table with condition columns (the old skip is gone).
10. `Holds` on a held `TableVersion` calls the closure with `this_in` and reports its error.

## Files

- `include/tabular/relation.h`: `column_type::String` and `column_type::Vec2`; `cell` gains `v` and
  `Vector`; `strings` and `conditions` spans on `insert_row` / `load_rows` as above; `StringAt`,
  the string `Find` overload, `TableVersion::Holds`.
- `src/relation.cpp`: the string slot array, interning, the condition holder, `Load` and `Retract`
  changes, schema refusals.
- `include/tabular/rows_csv.h`, `src/rows_csv.cpp`: string and `vec2` cells, the condition function.
- `src/schema_yaml.cpp`: the two type names.
- Tests above; `cmake/sources_tests.cmake` if a new test file is added.
- `docs/definition.md`: mark the column set built in "Order of work"; `docs/feature-checklist.md`:
  tick the items this slice builds.
