# Tabular: feature checklist

Every feature the Tabular documents describe, in one place, checked against what is built.
`definition.md` is the contract. Where an older document disagrees with it, the older one
lost; those features are under **Rejected** so they are not rebuilt by accident.

Sources, cited in brackets:

| Tag | Document |
|---|---|
| def | `docs/definition.md` |
| cat | `docs/project-notes/2026-10-12-table-catalogue-and-shape.md` |
| c3 | `docs/project-notes/2026-10-12-c3-bootstrap-fit.md` |
| retro | `docs/project-notes/2026-10-12-retrospective.md` |
| arc | superproject `docs/superpowers/superseded/arcs/tables-as-queries.md` |
| cook | superproject `docs/superpowers/superseded/specs/2026-10-07-tabular-cook-and-lookup-parity-design.md` |
| attach | superproject `docs/superpowers/superseded/specs/2026-10-08-tabular-attach-design.md` |
| sr | superproject `docs/superpowers/superseded/specs/2026-10-09-tabular-state-and-rules-design.md` |
| guard | superproject `docs/superpowers/specs/2026-10-09-context-free-guard-closures-design.md` |

`[x]` built and tested. `[~]` partly built (what is missing follows). `[ ]` open.
`(?)` the feature is listed in **Undecided** in def, so its shape is not settled yet.

## 1. Relations core

- [x] Typed columns: hash, flags, int, double, condition, command [def Structure; CONTEXT.md]
- [x] Command-pattern structs for create table, columns, insert, retract [def order 1]
- [x] Every row carries its owner; retract every row an owner has [def First version; arc §6.2; cat D1]
- [x] Key columns are equality and indexed: candidates are a lookup, not a scan [def A row]
- [x] A sorted order with prefix lookup, optionally unique; a unique clash is refused [def order 1]
- [x] Two kinds of table, each refusing the other's writes: read-only and mutable [def Two kinds]
- [x] Read-only tables are immutable versions, replaced whole only after the new one checks out (RCU); a held version stays consistent [def Two kinds]
- [x] Hash = `CityHash32` of the bytes as written, matching a script literal's `NameHash` [CONTEXT.md Hash; sr §3.1]
- [x] Runs with no world, no threads, no script engine, no files [def What Tabular is]
- [ ] RAII owner handles that retract their rows when dropped (crash and unload recovery) [def Lifetimes; cat D1 §1.3]
- [ ] Report that a relation's rows changed, so the owning subsystem can apply its reload policy and dependent queries wake [def First version, Triggers; arc §6.2]
- [x] Refuse hash 0 and same-file hash collisions, naming both names [sr §3.1] (CSV front ends; within one load, any column)
- [x] `vec2` column: two floats, one cell; the only vector type. Bands are `vec2`; a position is a string column naming the Section plus one or two `vec2` columns; no `Location` type [def Column types, owner 2026-10-16]
- [x] String column holding an interned `counted_string`, in its own per-table slot array like commands [def Column types, owner 2026-10-16]

## 2. Front ends

- [x] Grid tables: key × key, dense, every cell filled; grid CSV [owner 2026-10-16]
- [x] YAML table definitions; any diagnostic fails, unknown keys included [def order 2]
- [x] CSV rows into read-only tables; header names columns; refusals name the line and the clashing line; BOM dropped; no Unicode normalisation [def order 3]
- [x] CSV filling condition columns: `csv_rows::condition`, a `std::function<tl::expected<closure, ErrorContext>(std::string_view)>`, is called per cell; read-only `Load` accepts condition columns [def order 3, owner 2026-10-16]
- [ ] CSV quoting, or a rest-of-line last column, so a command cell can hold a comma [def order 3; sr §6.1 `restInLastColumn`]
- [x] Pivoted tables: any axis pivots into the headers (clip table pivots a key; phase table pivots the output, cells are bands) [def Pivot; cat §2.6; arc §5.2; cook §2.2]
- [x] Flag domain from pivot headers: first column None = 0, each next column the next bit, `A|B` combos OR earlier columns [cook §2.2; arc §5.2]
- [ ] `.tabular` bundle: one file holding YAML definitions plus CSV sections; the bundle file itself is the host's [arc §5.1; def What it is not]
- [ ] Mutable-table writes from s-expressions and script expressions (spawn tables, gambits, screen ownership) [def Two kinds]

## 3. Queries: matching and picking

Needed before machines: "a machine is a SELECT checked to always produce exactly one row" [def].

- [x] Flags match: all-in-input first, then the largest `R & F` as an integer (bit order is priority), then the larger `R`; else the required `R == 0` default row; ties go to column order if pivoted, then row order [def Flag matching; arc §3.2 `best`; cook §5]
- [x] Ranked fall-through: keys select candidates, flags rank them, take the best row whose other conditions hold, down to the default [def Picking]
- [~] Native condition kinds on Tabular's own types only (flags ranking and band are built; edge not built): flags subset, band (half-open `start <= x < end`, as animation's phase bands) / one-input comparison against a literal, edge (crossing over the previous..now interval) (?) [def A row, What it is not; arc §3.1; cat §2.6]
- [x] At most one script condition per row, any number of native ones [def A row]
- [ ] Weighted tables: keys + one weight + outputs, no conditions; draw across the selected rows [def Picking; c3 §5.9]
- [x] Pivoted flag outputs OR together, as animation phase tables do (`PipelineClosure::eval_phase_bits`): headers are flags, cells are bands, blank cells contribute nothing; every row whose key matches and whose band holds ORs in its column's bit; several rows may share a key and accumulate; nothing holding gives 0, so an OR table is total with no default row [def Picking; `src/Animation/authoring-form-spec.md` §Phase tables]
- [x] Pick "all": every matching row runs in row order [arc §3.2; cat §2.6]
- [ ] Query order for unordered results (nearest, then stable id) [c3 §8.9]
- [ ] Fan-out over a query result [c3 §8.1]
- [ ] Display relations: one relation with several queries, or several relations (?) [def Undecided; cat D1]

## 4. Load-time checks

Whatever the loader checks, an author can know by reading the table [def promise 2].

- [~] Totality over finite key domains; a table with opaque conditions must end in an unconditional row or an explicit hold [def promise 1; cat §2.6] (built per key run for best tables; key domains wait for machines, which close them)
- [x] Shadowed / unreachable rows refused (e.g. an earlier row with no conditions makes later rows dead) [arc §2.9; sr §6.1, §7] (best tables; partial overlaps are priority)
- [ ] Within one step, tables that feed each other form a DAG; a cycle is refused, naming the tables [def promise 2; attach §4]
- [ ] Two tables writing one state column refused [arc §2.6; attach §4; c3 §5.4]

## 5. Host interface

- [ ] Script conditions (anything with a wire, anything complicated) reach the host only through the loader's `condition(string_view)` function (§2), with no symbol table; native conditions (`a..b` bands, flag sets) are read by Tabular and never go through it [def Host interface, owner 2026-10-16]
- [ ] Compile refuses any snippet it cannot prove halts; read-only snippets call only read-only functions; functions that take a script callback are refused [def Host interface; guard Rulings]
- [ ] Input resolution for every name Tabular does not own (wires are the host's) [def Host interface; cat §2.3]
- [ ] Sinks for emitted commands and messages [def Host interface]
- [ ] A failing condition (raise, null path) holds the table for the step, reported once per (table, row) [arc §2.11; sr §6.3]

## 6. Machines

- [ ] Definition (immutable, per bundle section) → Binding (per definition × object type: closures, resolved inputs, state layout) → Instance (per object: state vector, edge history) [def Lifetimes; cat §2.2]
- [ ] State vector declared in the definition, owned by the table, not mirrored in script; script reads it late-bound by name [sr §4.1; memory: no script mirror]
- [ ] Values write the *next* state vector, swapped in at the end of the step; next state always names a member, never computed from a state [def A row]
- [ ] Transition tables and entry tables [def Structure]
- [ ] Bridges: a suffix command describing the steps between two states (?) [def Structure, Undecided; cat §2.1]
- [ ] State-function table: a state maps to an ordinary host script function, which may suspend; the machine holds the state until it returns [def State functions]
- [ ] Function-chain table: the chain of functions between two functions [def State functions]
- [ ] State functions are aborted and their state re-entered on reload [def State functions]
- [ ] "Exactly one row" proved for the machine's SELECT, then compiled to run fast (closures or machine code; undecided) [def A machine is a SELECT; sr §6.4; guard §3]
- [ ] Hot reload migrates instance state by name: keep, reset to initial, or drop [def promise 5; sr §4.4]
- [ ] Snapshot rule: wires read other entities' committed state as of step start (?) [def Undecided; c3 §8.5]
- [ ] Several machines per object as orthogonal regions [arc §4.1]

## 7. Commands, messages, randomness

- [~] Commands: the command column stores the s-expression text; enqueueing to a sink on pick is not built [def A row Suffix]
- [ ] Decide whether a consumer compiles its s-expression at bind, so a step enqueues a prebuilt record (?) [def Undecided]
- [ ] A table absorbs a message, one at a time, in arrival order; which step is still open (?) [def Undecided]
- [ ] A delayed message arriving after its state ended and was re-entered (?) [def Undecided]
- [ ] Deterministic randomness: counter-based draw from the host's seed, salted per draw site, no stored generator [def promise 4; cat §2.5]
- [ ] `random()` in a table snippet and in ordinary script use the same deterministic generator; `random(...)` inside a command (?) [def Undecided]

## 8. Budgets and evidence

- [ ] A measured number and a failing benchmark for each: build a definition, bind, create an instance, step (per row), relation insert / retract / query [def Budgets; retro]
- [ ] Parity / oracle tests with reddening mutations for any compiled path [cook §7; sr §9; guard §6]
- [ ] Provenance: the last few picks per object (table, row, inputs), debug only [arc §2.10]

## Consequences

These are not features to build. Each goes red to green on its own once the features it follows
from are built; a test for one is a test of those. `[x]` when everything it follows from is `[x]`.

- [x] Handles fit in a table: a `uint64_t` punned into an `int64_t` column, no handle type ← typed columns (§1) [def Column types, owner 2026-10-16]
- [x] Unique is provable at load ← a unique clash is refused (§1) + read-only rows arrive only by load (§1, RCU) [arc §3.2]
- [x] A failed rebuild keeps the old version running ← read-only versions replaced only after the new one checks out (§1, RCU) [def promise 5; attach §6]
- [x] Conditions are `{function, data, release}` closures, released with their row ← the condition function returns the `closure` type `relation.h` already defines (§5) [def Host interface; cat §2.3]
- [ ] Every query picks a row (totality at run time) ← flags match with its required default row (§3) + ranked fall-through down to the default (§3) + the totality check (§4) [def promise 1; cat §2.6]
- [x] A missing default (`R == 0`) row is refused ← flags match requires it (§3) [def Flag matching]; checked for key runs a load adds rows to, as totality; a run left without one by removing rows picks nothing
- [x] No tie is ever ambiguous ← flags match breaks ties by row order, or column order if pivoted (§3) [def Flag matching]
- [x] Band gaps and overlaps are caught ← a gap fails the totality check (§4); an overlap is a shadowed row (§4) [c3 §5.12]; a gap is a totality refusal, a full overlap a dead row, a partial overlap priority
- [ ] Tables run in DAG order within a step, so a phase → lookup → bridge chain settles in one step ← tables that feed each other form a checked DAG (§4) [def A row]
- [ ] Triggers: at instance creation and on entering a state, every step, and when rows change ← entry and transition tables (§6) + reporting that rows changed (§1) [def Triggers]
- [ ] Edges are machine state ← the Instance holds edge history (§6) + edge is a native condition kind (§3) [def Structure; cat §2.6]
- [ ] A seed per instance and step ← counter-based draws from the host's seed (§7) [def Host interface, promise 4]
- [ ] Delays are a message to self with `:delay` ← no clock in Tabular (Rejected) + sinks for messages (§5) [def What it is not]

## Rejected

Do not rebuild these; the contract ruled them out.

- A cooked `.kre_tabular` binary: text is parsed at load [retro; vs cook, arc §6.1, sr §3.2]
- Tabular parsing or compiling AngelScript conditions itself (fork parser, operand resolution): the host owns the language [def What it is not; vs arc §2.11, sr §6.2]
- Native numeric comparisons that combine inputs, do arithmetic or mirror host types [def What it is not; retro]
- Reading another object's table state through paths (`lamp.brain.state`) [cat §2.4; vs sr §5]
- Script records bound to a Tabular instance (`Tabular(path, root, record)`) [sr supersedes attach §3.1]
- Linear scan only, no index [def A row: keys are indexed; vs arc §3.3]
- Timers and counters inside Tabular [def What it is not]
- Loading a mutable table from CSV, or mutating a read-only one [def Two kinds]

## Non-goals

Query language or ad-hoc queries; Datalog or recursion; arithmetic in cells or sequences as
rows; a scheduler; file IO; owning wires, the bundle file or reload policy; a rule DSL outside
the script language [arc §7; def What it is not; memory: code is data].
