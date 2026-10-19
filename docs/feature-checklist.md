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
- [x] Two kinds of table, each refusing the other's writes: read-only and mutable; a best table may be mutable, held as versions and checked on insert [def Two kinds]
- [x] Read-only tables are immutable versions, replaced whole only after the new one checks out (RCU); a held version stays consistent [def Two kinds]
- [x] Hash = `CityHash32` of the bytes as written, matching a script literal's `NameHash` [CONTEXT.md Hash; sr §3.1]
- [x] Runs with no world, no threads, no script engine, no files [def What Tabular is]
- [x] RAII owner handles that retract their rows when dropped (crash and unload recovery) [def Lifetimes; cat D1 §1.3] (`owner_handle`; retracts from every table, read-only and grids included, see def Owner handles)
- [x] Report that a relation's rows changed, so the owning subsystem can apply its reload policy and dependent queries wake [def First version, Triggers; arc §6.2] (the per-table generation counter; there is no callback)
- [x] Refuse hash 0 and same-file hash collisions, naming both names [sr §3.1] (CSV front ends; within one load, any column)
- [x] `vec2` column: two floats, one cell; the only vector type. Bands are `vec2`; a position is a string column naming the Section plus one or two `vec2` columns; no `Location` type [def Column types, owner 2026-10-16]
- [x] String column holding an interned `counted_string`, in its own per-table slot array like commands [def Column types, owner 2026-10-16]

## 2. Front ends

- [x] Grid tables: key × key, dense, every cell filled; grid CSV [owner 2026-10-16]
- [x] YAML table definitions; any diagnostic fails, unknown keys included [def order 2]
- [x] CSV rows into read-only tables; header names columns; refusals name the line and the clashing line; BOM dropped; no Unicode normalisation [def order 3]
- [x] CSV filling condition columns: `csv_rows::condition`, a `std::function<tl::expected<closure, ErrorContext>(std::string_view)>`, is called per cell; read-only `Load` accepts condition columns [def order 3, owner 2026-10-16]
- [x] CSV quoting, or a rest-of-line last column, so a command cell can hold a comma [def order 3; sr §6.1 `restInLastColumn`] (a table's `suffix`; no quoting)
- [x] Pivoted tables: any axis pivots into the headers (clip table pivots a key; phase table pivots the output, cells are bands) [def Pivot; cat §2.6; arc §5.2; cook §2.2]
- [x] Flag domain from pivot headers: first column None = 0, each next column the next bit, `A|B` combos OR earlier columns [cook §2.2; arc §5.2]
- [ ] `.tabular` bundle: one file holding YAML definitions plus CSV sections; the bundle file itself is the host's [arc §5.1; def What it is not]
- [ ] Mutable-table writes from s-expressions and script expressions (spawn tables, gambits, screen ownership) [def Two kinds]

## 3. Queries: matching and picking

Needed before machines: "a machine is a SELECT checked to always produce exactly one row" [def].

- [x] Flags match: all-in-input first, then the largest `R & F` as an integer (bit order is priority), then the larger `R`; else the required `R == 0` default row; ties go to row order; pivot headers are distinct flag sets [def Flag matching; arc §3.2 `best`; cook §5]
- [x] Ranked fall-through: keys select candidates, flags rank them, take the best row whose other conditions hold, down to the default [def Picking]
- [~] Native condition kinds on Tabular's own types only (flags ranking and band are built; edge not built): flags subset, band (half-open `start <= x < end`, as animation's phase bands) / one-input comparison against a literal, edge (crossing over the previous..now interval) (?) [def A row, What it is not; arc §3.1; cat §2.6]
- [x] At most one script condition per row, any number of native ones [def A row]
- [x] Weighted tables: keys + one weight + outputs, no conditions; draw across the selected rows [def Picking; c3 §5.9] (`pick: weighted`, `PickWeighted`)
- [x] Pivoted flag outputs OR together, as animation phase tables do (`PipelineClosure::eval_phase_bits`): headers are flags, cells are bands, blank cells contribute nothing; every row whose key matches and whose band holds ORs in its column's bit; several rows may share a key and accumulate; nothing holding gives 0, so an OR table is total with no default row [def Picking; `src/Animation/authoring-form-spec.md` §Phase tables]
- [x] Pick "all": every matching row runs in row order [arc §3.2; cat §2.6]
- [ ] Query order for unordered results (nearest, then stable id) [c3 §8.9]
- [ ] Fan-out over a query result [c3 §8.1]
- [ ] Display relations: one relation with several queries, or several relations (?) [def Undecided; cat D1]

## 4. Load-time checks

Whatever the loader checks, an author can know by reading the table [def promise 2].

- [~] Totality over finite key domains; a table with opaque conditions must end in an unconditional row or an explicit hold [def promise 1, Key domains; cat §2.6] (built for best tables, read-only and mutable: every key declares its domain (members, another table's column, or rows), every tuple of the domain has a total run, orphans are reported, a foreign domain re-checks its dependents when its table changes, and a bare retract reports instead of refusing; and a machine's `Prepare` checks each clause over the domain its sources close: a hash state column by its members, an earlier best or weighted clause by what its rows can answer, a literal by its value; any other source (a machine input, an int or double state, an or clause's flags) keeps the table's declared domain; one check with the domain as its parameter, and `Prepare` reports orphans as a load does. At run time `Step` runs that layout, so every in-domain tuple picks a row; an input-bound key whose value has no run holds its clause for the step)
- [x] Shadowed / unreachable rows refused (e.g. an earlier row with no conditions makes later rows dead) [arc §2.9; sr §6.1, §7] (best tables, on load and on insert; a row in no list of the run layout; partial overlaps are priority within one owner; between owners, equal-flag overlaps are refused)
- [x] Within one step, tables that feed each other form a DAG; a cycle is refused, naming the tables [def promise 2; attach §4] (P11: a clause names only earlier clauses of its list, so the DAG holds by how a machine is written; a clause naming itself or a later one is refused at definition build, naming the machine, list and clause)
- [x] Two tables writing one state column refused [arc §2.6; attach §4; c3 §5.4] (R4 as accepted: two clauses writing one column are ordered by position and the later write wins, so they are not refused; one clause writing a column twice is. `Step` writes in clause order, the entry list on top of the step list)

## 5. Host interface

- [x] Script conditions (anything with a wire, anything complicated) reach the host only through the loader's `condition(string_view)` function (§2), with no symbol table; native conditions (`a..b` bands, flag sets) are read by Tabular and never go through it [def Host interface, owner 2026-10-16]
- [ ] Compile refuses any snippet it cannot prove halts; read-only snippets call only read-only functions; functions that take a script callback are refused [def Host interface; guard Rulings]
- [x] Input resolution for every name Tabular does not own (wires are the host's) [def Host interface; cat §2.3] (`host_binding::resolve`, called once per machine input at `Bind`, returns an `input_getter` read with one indirect call per step; no symbol table)
- [x] Sinks for emitted commands and messages [def Host interface] (`host_binding::sink(instance, table, command)`, required by `Bind` when the machine emits; every answering `emit` clause hands it a view of its row's command after the step's swap, step then exit then entry; a message to another object or to self is a command, `(message ...)`, so the one sink carries both)
- [x] A failing condition (raise, null path) holds the table for the step, reported once per (table, row) [arc §2.11; sr §6.3] (a condition that cannot answer, a NaN ranged input, an over-wide flag set, a key with no run or a getter that cannot answer leaves the clause unwritten, and clauses reading it hold too; `host_binding::report(instance, table, row, error)` hears each once per instance: a condition per (clause, scripted row), a bad input per (clause, input), a missing run per clause, a getter per input, extra message arguments once; a condition is asked once per evaluation and the report carries the host's own error from that asking; a cascaded hold is not reported, and a repeat allocates nothing)

## 6. Machines

- [~] Definition (immutable, per bundle section) → Binding (per definition × object type: closures, resolved inputs, state layout) → Instance (per object: state vector, edge history) [def Lifetimes; cat §2.2] (built: `create_machine`, YAML `machines:`, `CheckMachine`, `Prepare` returning an immutable `Machine` that holds the versions it read and can say it is stale, `Bind` returning a shared `Binding` of one getter per input, and the snake_case `instance` holding the current and next state vectors; edge history waits for edges, slice 5)
- [~] State vector declared in the definition, owned by the table, not mirrored in script; script reads it late-bound by name [sr §4.1; memory: no script mirror] (declared: hash columns with members and an initial member, int and double columns with an initial value; an instance holds it and `instance::State(column)` reads it by index; reading it by name from script is the host adapter's)
- [x] Values write the *next* state vector, swapped in at the end of the step; next state always names a member, never computed from a state [def A row] (a step copies current to next, clauses write next, next becomes current; a written hash is a member by `Prepare`'s check)
- [x] Transition tables and entry tables [def Structure] (ordinary clauses under `step`, `exit` and `entry`; `Step` runs the step list, then, when a hash state column changed, the exit list keyed on the state being left (current) and the entry list on the one entered (next), entry's writes last (neither may set a hash state column); `CreateInstance` runs create, then entry, and no exit)
- [ ] Bridges: a suffix command describing the steps between two states (?) [def Structure, Undecided; cat §2.1]
- [ ] State-function table: a state maps to an ordinary host script function, which may suspend; the machine holds the state until it returns [def State functions]
- [ ] Function-chain table: the chain of functions between two functions [def State functions]
- [ ] State functions are aborted and their state re-entered on reload [def State functions]
- [x] "Exactly one row" proved for the machine's SELECT, for input values in the table's declared domain, then compiled to run fast (closures or machine code; undecided) [def A machine is a SELECT; sr §6.4; guard §3] (proved and laid out by `Prepare`: every tuple of each best clause's domain has a total run with no dead row, every weighted clause's tuple has a run, an or clause always answers one value (0 for no run), every hash a set writes is a member; the compiled form is that layout, which `Step` runs: binary search to the run, ranked flag groups, binary search to the cell, its list. An input value with no run holds the clause. Compiling further, to machine code, waits on slice 7's measured budgets (R8))
- [ ] Hot reload migrates instance state by name: keep, reset to initial, or drop [def promise 5; sr §4.4]
- [ ] Snapshot rule: wires read other entities' committed state as of step start (?) [def Undecided; c3 §8.5]
- [x] Several machines per object as orthogonal regions [arc §4.1] (several instances composited into one host object, each stepped with that object as `self`)

## 7. Commands, messages, randomness

- [x] Commands: the command column stores the s-expression text, handed to the host's sink when an `emit` clause answers [def A row Suffix]
- [ ] Decide whether a consumer compiles its s-expression at bind, so a step enqueues a prebuilt record (?) [def Undecided]
- [x] A table absorbs a message, one at a time, in arrival order [def Machines, Messages] (`Step` takes one `message{type, arguments}`, absorbed in that step; arrival order is the host's queue; a table keys on the type through a `domain: message` key whose blank cell is no message, a type with no run falls through to the blank run in `PickBest` and `Step` alike, the blank never leaks into a non-message key (a foreign-column domain from a message key drops it; a non-message key reading an earlier clause's message key is refused), extra arguments are reported once, and `instance::Absorbed()` tells the host whether to route the message to script)
- [ ] A delayed message arriving after its state ended and was re-entered (?) [def Undecided]
- [x] Deterministic randomness: counter-based draw from the host's seed, salted per draw site, no stored generator [def promise 4; cat §2.5] (`Draw`)
- [ ] `random()` in a table snippet and in ordinary script use the same deterministic generator; `random(...)` inside a command (?) [def Undecided]

## 8. Budgets and evidence

- [ ] A measured number and a failing benchmark for each: build a definition, bind, create an instance, step (per row), relation insert / retract / query [def Budgets; retro]
- [~] Parity / oracle tests with reddening mutations for any compiled path [cook §7; sr §9; guard §6] (`Step` against `PickBest`, `PickOr` and `PickWeighted`: randomized, seeded, through cuts, range ends, NaN and over-wide flags, each term of the flag rank and signed multi-key tuples; the helpers Step shares with the picks rest on the Pick suites, not on parity; slice 7 extends them alongside the budgets)
- [ ] Provenance: the last few picks per object (table, row, inputs), debug only [arc §2.10]

## Consequences

These are not features to build. Each goes red to green on its own once the features it follows
from are built; a test for one is a test of those. `[x]` when everything it follows from is `[x]`.

- [x] Handles fit in a table: a `uint64_t` punned into an `int64_t` column, no handle type ← typed columns (§1) [def Column types, owner 2026-10-16]
- [x] Unique is provable at load ← a unique clash is refused (§1) + read-only rows arrive only by load (§1, RCU) [arc §3.2]
- [x] A failed rebuild keeps the old version running ← read-only versions replaced only after the new one checks out (§1, RCU) [def promise 5; attach §6]
- [x] Conditions are `{function, data, release}` closures, released with their row ← the condition function returns the `closure` type `relation.h` already defines (§5) [def Host interface; cat §2.3]
- [x] Every query picks a row (totality at run time) ← flags match with its required default row (§3) + ranked fall-through down to the default (§3) + the totality check (§4) [def promise 1; cat §2.6]; a machine clause over a tuple of its proved domain always picks, and one whose input-bound key has no run, or whose pick fails, holds
- [x] A missing default (`R == 0`) row is refused ← flags match requires it (§3) [def Flag matching]; checked, as totality, for every key run of a version a load that brings rows publishes; a bare retract is unchecked, so a run it leaves without one picks nothing
- [x] No tie is ever ambiguous ← flags match breaks ties by row order (§3) [def Flag matching]
- [x] Band gaps and overlaps are caught ← a gap fails the totality check (§4); an overlap is a shadowed row (§4) [c3 §5.12]; a gap is a totality refusal, a full overlap a dead row, a partial overlap priority within one owner; between owners, equal-flag overlaps are refused
- [x] Tables run in DAG order within a step, so a phase → lookup → bridge chain settles in one step ← tables that feed each other form a checked DAG (§4) [def A row]; from clause order plus in-step reads of next and of earlier clauses' answers, with no settling loop
- [~] Triggers: at instance creation and on entering a state, every step, and when rows change ← entry and transition tables (§6) + reporting that rows changed (§1) [def Triggers]; creation, entry, exit and step run; a rows change is seen as `Machine::Stale`, and the re-`Prepare` plus `Rebind` it triggers is slice 6
- [ ] Edges are machine state ← the Instance holds edge history (§6) + edge is a native condition kind (§3) [def Structure; cat §2.6]
- [x] A seed per instance and step ← counter-based draws from the host's seed (§7) [def Host interface, promise 4]; `Step` takes the seed and a weighted clause draws at `DrawSite(machine, list, clause)`
- [x] Delays are a message to self with `:delay` ← no clock in Tabular (Rejected) + sinks for messages (§5) [def What it is not]; the grazer's panic entry emits `(message this "calm" :delay random(0.4, 1.0))` and the calm message, when the host delivers it, moves the brain

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
