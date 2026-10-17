# Picking and grids

**Status:** landed 2026-10-16 (approved by the owner the same day). See **As built** at the end for where the plan amended this spec.

**Contract:** `Spehleon/Tabular/docs/definition.md` ("A row", "Flag matching", "Picking", "Pivot").
This slice is the first where Tabular answers *which row*, not only *what rows exist*.

## Goal

1. A read-only table declares **inputs** and **native conditions** (bands, a flag-ranking
   column) beside its script condition, and a **pick** returns the row, the rows or the OR'd
   flags that the definition's picking rules give.
2. **Pivoted CSV** writes those tables as grids that expand into flat, key-sorted rows.
3. A **grid** is a real 2D array: a row key, a column key, one dense cell per pair.

## Rulings this rests on (owner, 2026-10-16)

- **All of a row's conditions must hold** for the row to count. Flags never fail a row; they
  rank it. At most one script condition per row, as the definition already says.
- **Inputs are declared** by the table and referred to by name; a pick takes their values in
  declared order. Tabular resolves no names at pick time; the caller resolves wires.
- **Two storage shapes.** Key × key pivots are 2D arrays, stored densely. Flags-of-key pivots
  (animation lookup tables) and output-flag pivots (phase tables) are flat rows sorted by key: a
  pick is a `lower_bound` and a walk to the end of the key run, which is the efficient form.
- **A blank grid cell is an error.**
- **Grid cells** are any value type, a string (e.g. clip paths) or a command (held as text).
  Never conditions.

## Out of scope

Totality and shadowed-row checks; weighted tables and draws; edges; machines; mutable-table
picks; dense storage for flag pivots; lexical string order; anything host-side.

## Design

### 1. Schema: inputs, bands, ranking, pick policy

`create_table` gains, at the end (aggregates are positional):

```cpp
struct add_input { std::string_view name; column_type type = column_type::Double; };  // Double or Flags

enum class pick_policy : uint8_t { None, Best, All, Or };

struct create_table
{
    ... existing fields ...
    std::span<add_input const> inputs;
    pick_policy                pick   = pick_policy::None;   // None: not pickable
    std::string_view           output;                       // Or: the flags value column OR'd
};
```

`add_column` gains `std::string_view input` (empty: none). `column_type` gains **`Band`**: a
condition stored as a `vec2` cell, holding when `start <= x < end` for its `double` input.

- **A `Band` column** must name a `Double` input. It is a condition: `Value` refuses it, and it
  is never a key or in an order. A table may have any number.
- **A `Flags` column with `input`** is the table's **ranking column**, naming a `Flags` input.
  At most one per table. It is still a value column (`Value` reads its `R`).
- **A pickable table** (`pick` not `None`) is read-only and has an `order_by` whose leading
  columns are its key columns (`key: true`), in declaration order; the pick's keys are that
  prefix. A table with no keys is one run.
- **`Or`** names `output`, a `Flags` value column. `Best` and `All` leave it empty.
- **Refused by `BuildSchema`**, each naming the table and column: an input named twice or of a
  type other than `Double`/`Flags`; a column naming an undeclared input or one of the wrong
  type; `input` on a column that is neither `Band` nor `Flags`; two ranking columns; two
  `Condition` columns; a pickable table that is writable, or whose order does not start with
  its keys; `Or` without a flags `output`, or `output` on another policy.
- `table_def` carries the resolved forms: `immutable_array<input_def> inputs`, `pick`,
  `uint32_t output`, `uint32_t ranking` (`UINT32_MAX` when none), and per column
  `uint32_t input` (`UINT32_MAX` when none).

**YAML:** `inputs: [{name, type}]`, `pick: best|all|or`, `output: <column>`, column key
`input: <name>`, type name `band`. Every other YAML rule stands: any diagnostic fails it.

### 2. A pivot column for ties

A pickable table may be loaded pivoted (§4), where flag ties go to column order. So every
version keeps, per row, a `uint16_t` **pivot column**: the header position the row came from,
0 for rows loaded unpivoted or inserted. `load_rows` gains `std::span<uint16_t const> pivot`
(empty: all 0), at the end.

### 3. Picks

`include/tabular/pick.h`, `src/pick.cpp`. Picks read a `TableVersion`, so a held version can be
picked without the store; `relation_store` gets the same three taking a table number.

```cpp
struct pick_query
{
    std::span<cell const> keys;     // the key columns, in order
    std::span<cell const> inputs;   // the declared inputs, in order
    void const*           this_in = nullptr;   // for the script condition
};

tl::expected<std::optional<size_t>, ErrorContext> PickBest(TableVersion const&, pick_query const&);
tl::expected<std::vector<size_t>, ErrorContext>   PickAll (TableVersion const&, pick_query const&);
tl::expected<uint32_t, ErrorContext>              PickOr  (TableVersion const&, pick_query const&);
```

**Holding.** A row holds when every `Band` holds for its input and its script condition (if any)
holds. Bands are tested first, so a script runs only for a row whose native conditions hold.
A script condition that cannot answer fails the pick with its error, naming the table and row;
it is never read as "does not hold".

**`PickBest`.** The key run is `FindOrdered(keys)`. With no ranking column, rows are tried in
row order. With one, for input `F` and each row's `R`, the run is ranked as animation's
`LookupBest` ranks a lookup row (`animation_pipeline_program.cpp`), so a table moved from
animation picks the same cell:

- a row with `R == 0` is the **default**, tried last;
- a row with `R != 0` and `R & F == 0` does not match and is never tried;
- the rest are tried best first by `(R & F == R, R & F, R)`, each compared as an integer,
  larger first; ties go to the lower pivot column, then to row order.

The first ranked row that holds is returned; if none holds, the default if it holds; if not,
an empty optional (a table the totality check, a later slice, will refuse).

*Compared with the definition's wording:* the definition ranks by the largest `R & F` with an
exact match first. Animation's order agrees on every case the definition names (an exact match
is the only row with `R & F == R` and `R & F == F`), and adds two tie-breaks: a row whose
flags are all present beats one with a flag missing, and then the larger `R` wins.
`definition.md` is updated to state this order.

**Load check.** A `Best` table with a ranking column must have an `R == 0` row in every key run;
`Load` refuses a version without one, naming the key (its first row's CSV line through
`load_refusal::row`).

**`PickAll`.** Every row of the run that holds, in row order. Ranking is ignored.

**`PickOr`.** The OR of `output` over every row of the run that holds; 0 when none does. This is
`PipelineClosure::eval_phase_bits`: half-open bands, rows sharing a key accumulate, no default
row needed.

**Refusals** (errors, never asserts): a pick of the wrong policy for the table; key or input
counts that differ from the declaration; a `Flags` input whose value does not fit 32 bits.

### 4. Pivoted CSV

`csv_rows` gains `bool pivoted` (at the end). A pickable table names its pivot in YAML:
`pivot: {headers: <flags column>, cells: <column>}`, giving `create_table` a `pivot_headers`
and `pivot_cells` (column numbers, `UINT32_MAX` for none).

- **Header row:** the key columns' names, in key order, then the flag headers. A table with a
  pivot has exactly its keys, the `headers` column and the `cells` column; any other column
  (a script condition included, which a grid cell could not also fill) is refused by
  `BuildSchema`. A pivoted file is loaded with `csv_rows::pivoted` set; an unpivoted load of such
  a table is also allowed (its rows get pivot column 0), so a default row can come from either.
- **The flag domain comes from the headers**, read once, in order (cook §2.2):
  - `Best`: the **first header is the 0 value**, spelled `None` (case-insensitive), taking no
    bit; it is the column for each run's default.
  - `Or`: there is no 0 column; the first header takes bit 0.
  - Each simple header after takes the next bit, left to right; at most 32 bits.
  - A combo header `A|B` carries the OR of headers to its left and takes no bit.
  - A duplicate header, an unknown name in a combo, or a 33rd bit is refused, naming it.
- **Each non-empty cell** becomes one row: the line's keys, the header's flags in `headers`,
  the cell parsed by the `cells` column's type (a band `a..b` for a phase table, a value for a
  lookup table), and the header's position as its pivot column. Blank cells give no row.
- Rows are handed to `Load` in line order; the table's order sorts them by key, keeping line
  order within a key, then pivot column order within a line.
- The header domain is returned to the caller so it can name bits: `LoadCsv` fills an optional
  `std::vector<std::string>* flag_names` (index = bit; the `Best` 0 column is not in it).

### 5. Grids

A grid is a table kind: read-only, a dense 2D array of one cell type.

```cpp
struct create_grid_axis { std::string_view name; column_type type = column_type::Hash; };  // Hash or Int

struct create_table
{
    ... ;
    create_grid_axis const* grid_rows    = nullptr;   // set: this table is a grid
    create_grid_axis const* grid_columns = nullptr;
    column_type             grid_cells   = column_type::Int;
};
```

YAML: `grid: {rows: {name, type}, columns: {name, type}, cells: <type>}`. A grid has no
`columns`, `order_by`, `inputs`, `pick` or `pivot`; any of those, an axis type other than
`hash`/`int`, or a cell type of `condition` or `band` is refused.

**Storage.** `GridVersion` (`include/tabular/grid.h`, `src/grid.cpp`), immutable and refcounted
like `TableVersion`: row keys and column keys, each a sorted array of `(key, position)`; and the
cells, `rows × columns` row-major, as `std::vector<cell>` for a value grid or
`std::vector<counted_string>` for a string or command grid (interned as string columns are).
The store holds one version slot per grid beside the table versions.

**Loading.** `relation_store::LoadGrid(load_grid const&)`, where `load_grid` is
`{table, owner, row_keys, column_keys, values | strings}` (row-major cells; one of the two
spans, by cell type). It builds the whole version, checks it, then swaps it in; a refusal
leaves the current version. A duplicate row or column key is refused with `load_refusal`
naming both. `Retract(owner)` replaces a grid that owner loaded with an empty one.

**Reading.**

```cpp
counted_ptr<GridVersion const> relation_store::Grid(uint32_t table) const;   // refused for a non-grid
// on GridVersion:
tl::expected<uint32_t, ErrorContext>       RowOf(cell key) const;
tl::expected<uint32_t, ErrorContext>       ColumnOf(cell key) const;
tl::expected<cell, ErrorContext>           At(cell row_key, cell column_key) const;
tl::expected<counted_string, ErrorContext> StringAt(cell row_key, cell column_key) const;
cell                                       At(uint32_t row, uint32_t column) const;        // asserts bounds
counted_string const&                      StringAt(uint32_t row, uint32_t column) const;  // asserts bounds
uint32_t Rows() const; uint32_t Columns() const; uint64_t Generation() const;
```

An unknown key is an error, never a default; a value read of a string grid, or a string read of
a value grid, is an error. The index overloads assert their bounds (asserts are live in
release, so a bad index aborts rather than reading past the array); they exist so a hot caller
resolves `RowOf`/`ColumnOf` once. `Insert`, `Load`, `Value`, `Find`, `FindOrdered` and the picks
refuse a grid; `LoadGrid` and `Grid` refuse a non-grid.

**Grid CSV.** `LoadGridCsv(relation_store&, csv_grid const&)` with `csv_grid { table, owner, text,
label, first_line }`. The top-left cell is ignored; the rest of the header row is the column
keys; the first cell of each line is its row key. Keys parse as key cells (hash: CityHash32 of
the text; int: decimal). Refused, naming line and column header: a blank cell; a ragged line; a
duplicate row key (both lines) or column key (both headers); a cell that does not parse as the
cell type. Trimming, `#` lines, blank lines and the BOM behave as in `LoadCsv`.

## Error handling

Every refusal returns `ErrorContext` and changes nothing; no new log calls; no asserts except
the grid index overloads' bounds, whose indices come from `RowOf`/`ColumnOf`.

## Tests (gtest, `Spehleon/Tabular/test/`)

Each names the production change that would turn it red.

1. Schema refusals of §1, one each; YAML round-trips `inputs`, `band`, `input`, `pick`,
   `output`, `pivot`, `grid`.
2. `PickBest` without ranking returns the first row whose bands hold; falls through a row whose
   band fails, and one whose script condition returns false.
3. Flag ranking: exact wins; a subset row beats a non-subset with a larger `R & F`
   (`(full, hit, R)` order); larger `R` breaks a tie of the same hit; equal rows go to pivot
   column then row order; `R & F == 0, R != 0` never wins; the default only when nothing ranked
   holds. A table fixture copied from animation's `test_lookup_best.cpp` cases gives the same
   cells.
4. A `Best` ranked table with a key run lacking `R == 0` is refused at load, naming the line.
5. A failing script condition fails the pick with its error and the row; bands are tested before
   the script (a counting closure is not called for a row whose band fails).
6. `PickOr` matches `eval_phase_bits` on animation's phase CSV example: half-open edges
   (`t == end` contributes nothing), rows sharing a key accumulate, nothing holding gives 0.
7. `PickAll` returns holding rows in row order.
8. Pick refusals: wrong policy, wrong key or input count.
9. Pivoted CSV: `None` column is the default; bits in header order; `A|B` combo; duplicate
   header, unknown combo name and 33rd bit refused; blank cells give no rows; ties follow column
   order; `flag_names` returned.
10. Grid: lookup by keys and by indices on a held version; a reload swap while the old version
    is held; string and command grids; `Retract` empties it; table calls refuse a grid and grid
    calls refuse a table.
11. Grid CSV refusals: blank cell (line and header named), ragged line, duplicate row key, duplicate
    column key, unparsable cell.

## Files

- `include/tabular/relation.h`, `src/relation.cpp`: `Band`, inputs, pick policy, ranking, pivot,
  the pivot-column array, the default-row load check, grid schema and version slots,
  `LoadGrid`, `Grid`, refusals.
- `include/tabular/pick.h`, `src/pick.cpp` (new): the three picks.
- `include/tabular/grid.h`, `src/grid.cpp` (new): `GridVersion`.
- `include/tabular/rows_csv.h`, `src/rows_csv.cpp`: pivoted loading; `LoadGridCsv`.
- `src/schema_yaml.cpp`: the new keys and type name.
- Tests above; new test files registered in the superproject `cmake/sources_tests.cmake`, new
  sources in Spehleon's and the superproject's source lists.
- `docs/definition.md`: the flag-ranking order; the Undecided bullet on condition kinds becomes
  the ruling (band, flags ranking, one script condition; all must hold); grids under Pivot.
  `docs/feature-checklist.md`: tick what this builds.

## As built

The plan (`docs/superpowers/landed/plans/2026-10-16-picking-and-grids.md`) amended the spec in six places, and the code follows the plan:

1. Command structs name inputs, outputs and pivots by number, like `order_by` names its columns; YAML resolves names to numbers. So `add_column::input` and `create_table::output` are `uint32_t` (`UINT32_MAX` = none), and the pivot is `pivot_by const*` (`{headers, cells}` column numbers).
2. The grid shape is one struct, `grid_shape const* grid` on `create_table` (`{rows, columns, cells}`).
3. A pivot's cells column may be a string column, besides a value or band column.
4. Bands compare in float, as `eval_phase_bits` does: the `double` input is narrowed to `float` and tested `x >= start && x < end`.
5. The default-row load check covers key runs that contain a row of the load. A run made only of survivors is not re-checked, so removing rows is never refused, and such a run's `PickBest` returns an empty optional when nothing ranked holds.
6. "At most one script condition" applies to every table, not only pickable ones.
