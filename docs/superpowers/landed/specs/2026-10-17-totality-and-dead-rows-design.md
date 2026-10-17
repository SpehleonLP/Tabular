# Totality and dead rows

**Status:** landed 2026-10-17 (approved by the owner the same day). See **As built** at the end for where the plan amended this spec.

**Contract:** `Spehleon/Tabular/docs/definition.md` ("What it promises" 1 and 2, "Flag matching",
"Picking"). The picking slice (`landed/specs/2026-10-16-picking-and-grids-design.md`) made a
pick answer *which row*; this slice makes the loader prove that a best table always has one,
and that every row in it can be picked.

## Goal

1. A **best** table is **total**: in every key run, some row holds for every input the
   table can be given. A load that breaks this is refused, naming the key and an input that
   would pick nothing.
2. A best table has **no dead rows**: a row that earlier rows always take the place of is
   refused, naming it and the rows that cover it.
3. Hash cells are trustworthy: **hash 0 and two names with one hash** in one file are refused.

## Rulings this rests on (owner, 2026-10-17)

- **Hash key domains are closed by a machine**, which relates its keys to each other (states
  name states). Nothing outside a machine closes them, so until machines exist totality is per
  key run: a key with no run in the table is not checked here.
- **Tables like "a reaction by energy level" are real**: best tables with bands are an
  implication of the other rules, not a hypothetical.
- **A band input declares its range, and a pick clamps the input to it before testing.**
- **Totality over script conditions cannot be proved**, so a table guarded by them needs a
  **quiescent row**: rows that hold with no script condition. The default is declared by
  writing those rows, not by new syntax (chosen over a marked `default` row and a table-level
  default value).

The worked example the rulings were made on:

```yaml
tables:
  - name: reaction
    read_only: true
    pick: best
    inputs:
      - {name: energy, type: double, range: 0..1}
    columns:
      - {name: stimulus, type: hash, key: true}
      - {name: energy,   type: band, input: energy}
      - {name: when,     type: condition}
      - {name: reaction, type: string}
    order_by: {columns: [stimulus]}
```

```csv
stimulus, energy,   when,         reaction
predator, 0..0.2,   ,             freeze
predator, 0.2..1,   cornered(),   fight
predator, 0.2..1,   ,             flee
food,     0..0.6,   ,             eat
food,     0.6..1,   ,             ignore
```

`freeze` and `flee` are the quiescent rows of `predator` and cover `0..1`, so the table loads.
Without the `flee` row, an uncornered predator at energy 0.5 picks nothing, and the load is
refused: `key (predator): energy 0.2 has no row without a script condition`.

## Out of scope

Totality over key domains (machines close them); the DAG and two-writer checks (need machine
state); totality of `all` and `or` tables (an empty answer is a real answer for both: an or
table answers 0, an all table runs nothing); partial band overlaps in best tables (legal: the
earlier row wins where they overlap); mutable tables (rows arrive one at a time, so no load is
a whole run); grids (dense, total by construction); weighted tables; edges.

## Design

### 0. A row with no script condition

The definition allows **at most** one script condition per row, but today every row of a table
with a condition column carries a closure: `Load` and `Insert` refuse a closure with no
`function`, and the CSV front ends hand every condition cell, blank or not, to the host. So
Tabular cannot tell a quiescent row from one whose host-compiled condition always holds, and
totality cannot be checked. This slice adds the missing state:

- A **`closure` with a null `function`** means **no script condition**: the row holds as far
  as that column goes, and nothing is called or released. Its `data` and `release` must be
  null too; a null function with either set is refused (the data would leak).
- `Load` and `Insert` accept it; the refusal of a null function goes.
- **A blank condition cell in CSV means no condition** and is not handed to the host. This is
  the one blank cell the CSV front ends accept, because "no condition" is the cell's natural
  reading and the alternative (the host compiling blank to `true`) hides quiescence from the
  loader. Every other blank cell is still refused.

### 1. Hash 0 and same-file collisions

In one call to a CSV front end (rows, pivoted rows or grid), every text that becomes a hash, in
any column, is recorded with its line. Refused:

- a text whose `CityHash32` is **0**, which the state-and-rules design reserves (checklist §1, [sr §3.1]);
- **two different texts with the same hash**, naming both texts and both lines.

The same text on many lines is the point of a key and is not a collision. Rows built in code
carry hashes, not texts, so the builder checks neither. The front ends share one helper (in
`src/csv_cells.h`), so the three cannot drift.

### 2. Input ranges and clamping

`add_input` gains a range; `input_def` keeps it.

```cpp
struct add_input
{
    std::string_view name;
    column_type      type  = column_type::Double;   // Double or Flags
    std::optional<std::array<float, 2>> range;      // Double only: lo..hi, half-open
};
```

YAML: `{name: energy, type: double, range: 0..1}`, written as a `vec2` cell is.

- A range is **half-open, `lo <= x < hi`**, with `lo < hi`, both finite. A range on a `Flags`
  input is refused.
- **Required** on every `Double` input a band reads in a `best` table (totality needs a finite
  domain to cover). Optional anywhere else; an or table may declare one for its clamp.
- **A pick clamps** a ranged input before any band tests it: below `lo` reads as `lo`, at or
  above `hi` reads as the largest float below `hi` (`std::nextafter(hi, -inf)`), so the top
  value lands in the top band rather than in none. Bands compare in float, so the clamp is in
  float too. Inputs with no range are not clamped.
- **A band must lie inside its input's range**, `lo <= start` and `end <= hi`, checked at load.
  Any part outside could never be reached once inputs are clamped, and an author can see that
  by reading the row.

### 3. Coverage (internal)

`src/coverage.h`, one function:

```cpp
struct interval { float lo; float hi; };   // half-open

// The first point of the box `domain` that no box in `boxes` contains, or none. A box is a
// half-open interval per dimension. Exact: every dimension is cut at every box edge, and
// one point per cell is tested.
std::optional<std::vector<float>> Uncovered(std::span<interval const> domain,
                                            std::span<std::span<interval const> const> boxes);
```

A row's box has one interval per **ranged input a band reads**, not per band column: two band
columns on one input intersect. A row with no band on an input spans its whole range. With no
band inputs at all the domain has no dimensions, and it is covered exactly when there is at
least one box.

Cost is `O(n^(d+1))` for `n` boxes and `d` band inputs. Tables carry one band input, sometimes
two; it runs at load, never at pick. The "first" point is the lowest cell in dimension order,
so a refusal is deterministic.

### 4. Totality

Runs inside `Load` of a `best` table, on every key run the load adds rows to (as the default
check does today; runs it does not touch are not re-checked, so removing rows is never refused).

- A row is **quiescent** when it has no script condition and, if the table has a ranking
  column, its flags are 0. A ranked row never helps: an input with no flags skips every one.
- The run is **total** when the quiescent rows' boxes cover the domain (§3). With no band
  input this is "the run has a quiescent row".
- Refused otherwise, `refusal.row` set to the run's first loaded row:
  `table 'reaction': key (<key>): energy 0.2 has no row without a script condition`, or for a
  run with no bands, `key (<key>) has no row without a script condition or flags`.

This **replaces** the "has a row with no flags" check. That check passes a run whose only
`R == 0` row carries a script condition, or covers half the range, and does not run at all on
a best table with no ranking column; each of those is now refused.

### 5. Dead rows

Also inside `Load` of a `best` table, on the key runs the load adds to. A row `B` is **dead**
when the boxes of the rows tried before it **for every input**, among those with no script
condition, cover `B`'s box. Which rows those are follows from the pick order:

- **No ranking column:** rows are tried in row order, so the earlier rows of the run.
- **Ranking column, `B`'s flags `R != 0`:** earlier rows with the **same `R`**, in tie order
  (pivot column, then row). A row with different flags never shadows `B`: at input `F = R_B`,
  `B` is a full match with `R & F = R_B`, and any other full match `A` has `R_A` a strict
  subset of `R_B`, so a smaller `R & F`; any `A` that is not a full match ranks below `B`.
- **Ranking column, `R == 0`:** the earlier `R == 0` rows. Ranked rows never shadow them, for
  the reason in §4.

`B`'s own script condition does not matter: a dead row's condition is never asked. Refused with
`refusal.row` = `B` and `refusal.earlier` = the last covering row in try order (front ends name
both lines), the message listing every covering row:
`table 'reaction': key (<key>): row 4 is never picked; rows 2, 3 cover it`.

Exempt: `all` and `or` tables, where every row that holds counts.

### 6. Documents

- `definition.md`: totality as built (per key run; quiescent rows; key domains closed by
  machines); input ranges and the clamp; a blank condition cell is no condition; hash 0 and
  same-file collisions.
- `feature-checklist.md`: §1 hash refusal `[x]`; §4 totality `[~]` (per run; key domains wait
  for machines), shadowed rows `[x]`; Consequences: missing default `[x]`, band gaps and
  overlaps `[x]` (a gap is a totality refusal, a full overlap a dead row, a partial overlap
  priority).
- This spec and its plan move to `landed/` with an **As built** section.

## Tests

Each check gets a refusal test and a passing neighbour, with a reddening mutation run on the
check itself:

- §0: null-function closure loads and holds; null function with data refused; CSV blank
  condition never reaches the host's function (the test's function fails on any call).
- §1: two texts colliding (a known `CityHash32` pair, found once and pinned in the test) in
  rows, pivoted and grid CSV, naming both lines; repeated text accepted.
- §2: a ranged input below, at, and above the range picks the bottom, bottom and top band; a
  missing range on a best band input refused at schema build; a band past the range refused.
- §3: one dimension with a gap, a touching seam (`0..0.5`, `0.5..1`, covered), two dimensions
  with an L-shaped hole; no dimensions with and without a box.
- §4: the worked example loads; without `flee` refused at energy 0.2; an `R == 0` row with a
  script condition only refused; a best table with no ranking column and every row scripted
  refused; a run the load does not touch stays unchecked.
- §5: a quiescent row covering a later one refused; a scripted earlier row does not shadow;
  same-`R` rows in pivot order; different `R` never refused; two rows that together cover a
  third refused, naming both.

## As built

The plan (`docs/superpowers/landed/plans/2026-10-17-totality-and-dead-rows.md`) amended the spec in six places, and the code follows the plan:

1. A range is three fields, `bool ranged; float lo; float hi;`, at the end of `add_input` and `input_def`, not `std::optional<std::array<float, 2>>`, so `relation.h` gains no include and positional `add_input{name, type}` initialisers keep compiling.
2. A dead row's covering row is reported in a new `load_refusal::covered_by`, not in `earlier`: the CSV front ends print `earlier` as "same key as line N" and `covered_by` as "covered by line N".
3. Messages keep "no row with no flags" for ranked tables (`key (1): t 0.2 has no row with no flags and no script condition`); an unranked table says `... has no row with no script condition`. The spec's "without a script condition" wording was not used.
4. A band whose start is not below its end is refused with the out-of-range band check, and a row whose band columns on one input do not overlap is a dead row with no covering rows ("its bands hold for no input").
5. A survivor made dead by a load (a loaded row ahead of it in tie order) refuses the load; `refusal.row` is then the loaded covering row.
6. Two private helpers are tested directly: `test/coverage_gtest.cpp` includes `src/coverage.h`, and `test/csv_hash_gtest.cpp` includes `src/csv_cells.h` to test hash 0, since no text hashing to 0 is known.

Beyond the plan:

- `Uncovered` takes the boxes flat, `count` boxes of `domain.size()` intervals one after another, rather than a span of spans.
- A dead-row message with one covering row reads "loaded row 2 covers it"; with several, "loaded rows 0, 1 cover it".
- `range` in YAML is checked by presence, as `input` is: a written `range: ""` is refused rather than read as no range.
- The old "has a row with no flags" check is gone; totality replaces it, and also runs on best tables with no ranking column.
