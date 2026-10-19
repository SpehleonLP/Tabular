# The rest of Tabular

**Status:** landed 2026-10-19 on `tabular-slice0` (slices 0–8), for the owner's review of the
branch. What was built is in **As-built** and the decisions left to the owner in **Open owner
decisions**, both at the end; the rulings blocks are the reasoning behind them.

**Contract:** `Spehleon/Tabular/docs/definition.md`. **Inventory:** `docs/feature-checklist.md`.

This spec covers everything left on the checklist at once. It does not design each item
separately. Its main job is to sort the open items into **premises**, which get built, and
**consequences**, which must not be built: each consequence should hold once its premises
do. A consequence with its own code path hides bugs in its premises. The special path makes
the feature look done while the general mechanism that should have produced it is wrong on
some input. The last slice's "a key run has a row with no flags" check was one: it stood in
for totality, passed a run whose only default row was scripted, and never ran on unranked
best tables.

## Goal

**Tabular keeps every promise in `definition.md`.** Every checklist item is in one of three
states:

- `[x]`, built and tested;
- moved under **Rejected** or **Non-goals** by an owner ruling; or
- moved to the host adapter's list (§8) by an owner ruling.

**Done when:**

1. Every premise in §2 is built. Each has its own refusal and passing tests, and a mutation of
   it turns at least one test red.
2. Every consequence in §3 has a test that drives it **through its premises only**. If you
   delete any code written specifically for that consequence, the test still passes. A
   consequence that needs code of its own is a finding: either a premise is missing from §2,
   or the item is not a consequence. Either way it goes back to the owner, not into the code.
3. The worked machine in §4 loads, binds, steps and hot-reloads in a gtest with no world.
4. Each slice ends with an independent review. It applies `review-agentic-systems` plus the
   emergence lens (§9), and every finding is fixed or ruled on.
5. The full suite passes once at the end, with its counts reported, and
   `scripts/test-compile-dependencies.sh` passes.
6. `definition.md`, the checklist and this spec (moved to `landed/` with an As-built section)
   all say what was built.

**How the work runs.** One branch per slice, in §5's order. A slice starts without asking
unless a ruling in §7 blocks it. A blocked slice is skipped, and its ruling is put to the
owner with my recommendation. Merges still need the owner's approval.

## Rulings (owner, 2026-10-18)

- The recommendations in §7 and §10 are accepted (R1-R12).
- **A machine is one query, prepared once.** `create_machine` is a command struct like
  `create_table`, written in YAML. `Prepare` proves totality and unambiguity and compiles the
  query, and `Step` runs that query. Tabular optimizes this one prepared call over general
  flexibility, which is where it differs from relational libraries. The interpreted picks stay
  as the oracle the compiled step must match. The design sketch was reviewed as
  https://claude.ai/artifact/1pMMs25cL7nDEr83XNCVg9 (P6, P7, P10, P11 below).
- **Flags are 32 bits** (an `asDWORD`), and `Prepare` takes all 32 with no further limit. The
  proof never enumerates flag values: totality needs only the default rows, and only rows with
  equal flags shadow each other. Each cell stores its rows grouped by flag set, and `Step`
  ranks those groups against the input flags.

- **A published version is total** (slice 0 review). A load that brings rows checks every key
  run in the version it would publish, not only the runs it touches or left; the special
  "runs the owner left" path goes. A bare `Retract` stays unchecked, so crash recovery never
  fails. This replaces R11's narrower check and makes `Retract` + `Load` agree with one `Load`.
- **Two owners' rows with equal flags may not overlap in one key run.** Two contributors
  claiming the same inputs is refused, naming both owners and lines. Within one owner, an
  overlap is still priority by row order. Ties then never depend on load history, so reloading
  identical rows is idempotent.

- **A key says what its domain is** (owner, 2026-10-18, from the display-selection worked
  example). Every key column of a best table declares where its domain comes from:
  - members;
  - another relation's column (a foreign key, e.g. `owner.display` from `display`);
  - a machine clause's output; or
  - explicitly `rows`, meaning the keys its own rows name.

  Nothing is inferred. Totality is checked over the domain, so a run whose key has left the
  domain (its display was retracted, its stimulus is no longer produced) is an orphan. It is
  reported, never refused, and `Retract` needs no refuse or cascade rule. This refines the
  version-totality ruling above: "every key run of the version" becomes "every key in the
  domain". Mutable tables get totality on insert and retract the same way, since the
  display tables are where "always picks a row" matters most.

  Applied in slice 2 (coordinator, from the slice 2 review; the owner may overrule at code
  review):
  - **`rows` columns name sub-tuples, not a product.** The key columns declared `rows` together
    contribute the joint sub-tuples the rows name; every other key column contributes its
    declared domain, and the key's domain is their cross product. All-`rows` is exactly the
    runs present. A product of per-column `rows` values would close one column's domain from
    another's values, which is inference no source declared.
  - **A stranded dependent blocks its source.** A change that brings rows proves every table
    whose domain it feeds. So a dependent left not total by a bare retract refuses every
    change to its source that brings rows until it is made total again. The refusal says the
    dependent is not total; it does not blame the source change.
  - **The report is the state, not a delta:** every orphan, and after a bare retract every
    not-total run, of each table the change touched and of their dependents.
  - **Slice 3:** a dependent's generation does not move when its domain changes (its rows and
    picks do not). `Prepare` proves over the domain, so a prepared machine must watch the
    generation of every table its domains read, not only the tables it picks from.

  Applied in slice 3a (coordinator, from the slice 3a review; the owner may overrule at code
  review):
  - **A clause answers one value per column, not one row.** So a clause may read a best, an
    `or` or a `weighted` table (definition.md's phase → lookup → bridge chain reads an `or`
    table; §3's per-row site salt reads a weighted one). An `all` table has no single answer
    and is refused. An `or` clause exposes only its flags output and cannot `emit`.
  - **A source that does not close a key leaves the table's declared domain.** An input, an
    int/double state column and an `or` clause's output close nothing, so that key is proved
    over what the table declares. The machine's own sources close the rest (state members,
    an earlier best clause's reachable answers, a literal). This is how "a machine clause's
    output" as a domain reads: the machine supplies it at `Prepare`, and the table keeps its
    declared domain for `Load`. One domain builder serves both.
  - **`Prepare` reports orphans** as `Load` does: runs no clause can reach are noted, so a
    typo'd key in a `rows` domain is visible.
  - **P9's `writes:` is dropped.** A clause's `set:` names the state column; a table read by
    two machines could not carry one `writes:`. The member check on a written hash therefore
    runs at `Prepare`, not at load.
  - **A literal for a ranged input must be finite and inside the range** (R12: a clamp has no
    place for NaN; a constant outside the range is a mistake).
  - **For 3b:** an input-bound key is proved only over the table's declared domain. A step
    whose input value has no run holds that table and reports once, as P15 does for a failing
    pick.

  Applied in slice 3b (coordinator, from the slice 3b review; the owner may overrule at code
  review):
  - **An entry clause may not set a hash state column.** Entry runs once per step, after the
    step list; a state an entry write reached would get no entry of its own, so its state
    function (R6) would silently never start. A transition belongs in the step list. Entry
    may still write int/double columns. "Entered" means "differs from current at the end of
    the step", so A → B → A within one step enters nothing.
  - **A step allocates nothing, holds included.** Tabular builds no error text on the step
    path; the slice 4 report site adds context. A host's own failure message past small-string
    size allocates on the host side.
  - **Holds cascade:** a clause reading a held clause holds. Inputs are read once per step, as
    one snapshot every clause shares.

  Applied in slice 4 (coordinator, from the slice 4 review; the owner may overrule at code
  review):
  - **A message key is declared on the table** (`domain: message`) and bound both ways to the
    machine's `message: {type, arguments}`. A blank cell there is hash 0, "no message", the
    one place 0 is allowed. A type no run keys on falls through to the blank run; that is a
    property of the table, so `PickBest` falls through too, through the one lookup helper.
  - **0 does not leak.** A foreign-column domain read from a message key drops 0, and a
    clause source carrying a message key into a key that is not one is refused.
  - **Absorbed** means a run keyed on the type answered; a fall-through or a hold is not.
  - **Commands go out after the swap,** in the order the lists ran: create then entry at
    creation; step, exit, entry per step. Exit reads current and may not set a hash state.
  - **A failure is reported once per (clause, row) per instance,** with the host's own error
    from the one evaluation; a cascaded hold is not reported. Report memory covers scripted
    rows only.
  - **Arguments are positional slots shared by every type:** a missing one reads 0, as a step
    with no message does. Extra arguments are reported once. Per-type arity is not declared;
    the owner may want it.

  Applied in slice 5 (coordinator, from the slice 5 review; the owner may overrule at code
  review):
  - **An edge holds when its input entered the band or passed through it** since the previous
    step, in either direction; staying inside or leaving does not. An edge row is a
    conditional row (like a scripted one), and its layout box is where a crossing can end, so
    the between-owners overlap rule reads that box.
  - **`NoEdge` (a blank cell) is "no edge"** in an edge column, as an empty closure is "no
    condition".
  - **One previous value per input,** set at creation (so create and entry cross nothing). A
    held step still spends the crossing; a NaN or failed read does not advance it. Edges read
    host inputs only: message arguments, state, clauses and literals are refused.
  - **Crossing is linear** on an ordinary input. One that wraps (an animation phase) declares
    `cyclic` (owner, 2026-10-19), and its crossing goes the shorter way around.

  Applied in slice 6 (coordinator, from the slice 6 review; the owner may overrule at code
  review):
  - **`Rebind` cannot be refused** and returns nothing: a binding holds everything it reads,
    migration always lands on a valid state, and a clause that cannot answer holds. It runs
    the old binding's exit (to the old sink), migrates by name from the state the exit left
    (a value kept while valid; an int ↔ double change resets), installs the new binding and
    runs its entry (to the new sink). Create does not run again.
  - **Previous input values carry only between getter inputs** of the same name and type; a
    new, retyped or message-argument input takes the value read during `Rebind`, so a reload
    crosses no edge. Report-once memory starts empty under the new binding.
  - **A definition-only change moves no generation;** the host that rebuilt the definition
    knows. An exit that holds sends no abort; the host learns of it through the report.
  - **Bridges, state functions and function chains are consequences** with tests and no code.
    A bounded repeat is a command the host runs (owner, 2026-10-19).

  Applied in slice 7 (coordinator, from the slice 7 review; the owner may overrule at code
  review):
  - **Provenance is switched by a generated header.** The engine's CMake writes
    `generated/tabular_build_config.h` (`TABULAR_PROVENANCE` 1 in Debug, 0 in the release
    family), included only by `tabular/instance.h`, so flipping it rebuilds only the units that
    use instances, not every command line. A build that defines the macro itself keeps its own
    (the budget gate sets 0); one with neither has it off. When it is on the instance type
    carries an ABI tag (and MSVC records the setting per object), so a mixed build fails to
    link rather than reading another layout.
  - **The ring is 16 clause runs of up to 8 values,** sized at creation and kept across
    `Rebind`, so a reload shows the old exit's picks then the new entry's and recording
    allocates nothing. A wider clause keeps its first eight values and says it was cut.
  - **Release builds have no provenance API at all,** not an empty ring: a caller cannot read
    "no picks" from a build that records none.
  - **Budgets are measurements with margins** (times 2×, bytes 1.25×, the grazer's whole build
    about 2.2× its slowest run), taken on one stated machine. The gate
    (`scripts/test-tabular-budgets.sh`) is therefore a regression check for that machine, not a
    portable bound, and runs on a quiet machine outside the e2e runner and CTest shards.
    `definition.md`'s budget table and `bench/budgets.cpp` are checked against each other both
    ways, so neither drifts.
  - **R8's criterion was ill-posed.** "Strike the compiled path if the budgets are met" cannot
    fail when every budget is the measurement times a margin. Whether 125 ns per row evaluated
    (about 1 µs at 400 MHz) is enough needs a load target (machines × rows per tick) that the
    contract does not state, so compiling to machine code stays open for the owner. The
    profile says the binary searches to the tuple and the cell are half the step, which
    machine code would not remove.

- **A version is a function of the owners' loads, not of load order** (follows from the
  idempotent-reload ruling, applied in slice 1). Among rows with equal order keys, rows sort by
  owner, then by their index in that owner's load. So `PickAll`, `PickWeighted` and the
  dead-row try order never change when an owner reloads identical rows.

## 1. The method: premises and consequences

For each open item, ask whether `definition.md` already implies it from other features. If
it does, the item is a consequence and gets no code of its own. What it needs is a test, plus
the premises that make the test pass.

Two kinds of finding go to the owner instead of into the code:

- **A consequence that will not emerge.** Some premise is wrong or missing. The fix belongs
  in the premise.
- **A premise the contract does not state.** The design needs a rule the definition does not
  have. That is a contract edit, made before the code.

## 2. Premises (to build)

| # | Premise | Checklist | Slice |
|---|---|---|---|
| P1 | **Owner handles.** A movable `owner_handle` holds an owner id and a store. Dropping it retracts that owner's rows from every table, read-only and grids included, through the one `Retract` (slice 1 decision: a dead contributor has nobody left to reload its read-only rows, so a mutable-only handle would leave them behind). | §1 RAII | 1 |
| P2 | **Relation generation.** Each table has a counter. It goes up on every change that becomes visible: a published version, an insert, a retract. "Rows changed" means the counter moved. | §1 report | 1 |
| P3 | **The suffix is the trailing cells.** A table may declare `suffix: command` instead of a final command column. In CSV, every cell after the declared columns, joined back with their commas, is that row's command text. | §2 quoting; §7 commands | 1 |
| P4 | **Counter-based draw.** `Draw(seed, site) -> uint64_t` is a pure hash with no stored state. `site` salts each draw site. | §7 randomness | 1 |
| P5 | **Weighted pick.** `pick: weighted` puts one `double` weight column on a table that has no conditions. `PickWeighted(query, seed, site)` uses P4 to choose among the rows of the key run. A zero or negative weight is refused at load. | §3 weighted | 1 |
| P6 | **Clauses take their keys and inputs from sources.** A clause binds each key and each table input to a source: a state column, a machine input, an earlier clause's answer, or a literal. Inside a step the instance supplies them, not the caller. | §5 input resolution | 2 |
| P7 | **`create_machine`.** A command struct, written in YAML under `machines:`. It holds a state vector (named columns: `hash` with members and an initial member, or `int`/`double` with an initial value), the machine's inputs, and four ordered clause lists: `create`, `entry`, `step`, `exit`. A clause is `from` a table, `where` key = source, `given` input = source, then `set` state columns from value columns and/or `emit` the suffix. Transition and entry tables are clauses, not table kinds. | §6 definition, state vector, transition/entry tables | 2 |
| P8 | **Binding and instance.** A binding is definition × host resolver. It resolves every input name to a getter the host supplies, through one function and with no symbol table. An instance holds the current and next state vectors, plus the edge history (P13). | §6 lifetimes | 2 |
| P9 | **Values write the next vector, which starts as a copy.** A step first copies current to next. Value columns (`writes: <state column>`) write next. At the end, next becomes current. A hash value must name a member of its column's set, checked at load. | §6 next vector | 2 |
| P10 | **`Prepare` proves by laying out.** `Prepare(create_machine, store)` returns an immutable `Machine` holding the table versions it read. For each clause and each tuple of its key domain, it cuts the input ranges at every band edge and stores, per cell, the rows the pick would try, in pick order, ending at the first row with no script. A ranked table's cell holds its rows grouped by flag set, and `Step` ranks the groups against the input flags (the proof needs no flag enumeration: totality reads the default rows, and only equal flags shadow). **Totality** is "every list ends in a row with no script", and **dead rows** are "a row in no list". The load-time checks of 2026-10-17 become this one layout, which `Load` keeps using for tables no machine reads, with the runs present as the domain. **Key domains:** a key reading a state column is closed by its members; a key reading an earlier clause is closed by the values that clause's table can answer; a key reading a host input is open, and its run must have a catch-all row. | §4 totality, shadowed rows; §6 exactly one row | 2 |
| P11 | **Clauses name only earlier clauses.** The DAG holds by how a machine is written, so there is no cycle check. Two clauses setting one column are ordered by position: the later write wins. | §4 DAG, two writers | 2 |
| P12 | **Sinks.** The step hands each picked row's suffix text, along with the instance id and table, to a host `sink` callback, in DAG order. Tabular does not interpret it. | §5 sinks; §7 commands | 4 |
| P13 | **Edge condition.** A band column marked `edge` holds when the input crossed into the band between the previous step's value and this one, so a short band is never skipped. The instance keeps the previous value of every input an edge reads. | §3 edge | 5 |
| P14 | **Migration by name.** `Rebind(instance, new_binding)` maps each new state column to the old column with the same name. It keeps the value if it is still valid (for a hash, still a member), and otherwise resets it to the initial value. Old columns that are gone are dropped. Then the current state is **entered again** (the `entry` trigger runs). | §6 hot reload | 6 |
| P15 | **A table error holds the table.** If a pick fails (a condition raises, or a ranged input is NaN), that table writes nothing this step. The failure is reported once per (table, row) through a host `report` callback. | §5 failing condition | 4 |
| P16 | **Provenance (debug builds).** Each instance keeps a small ring of its last picks: table, row and input values. | §8 provenance | 7 |
| P17 | **Budgets.** One benchmark per row of `definition.md`'s Budgets table. Each fails when its measured number is exceeded, and the numbers are written into `definition.md`. | §8 budgets | 7 |

## 3. Consequences (to test, not to build)

Each row names the premises that must produce it. Its test uses only them.

| Consequence | ← Premises | Checklist |
|---|---|---|
| **Totality over finite key domains.** A state with no transition rows is a key run with no quiescent row. | P10 + the built per-run check | §4 |
| **"Exactly one row" for a machine's SELECT.** This is totality plus first-wins picking. | P10 + built totality + built dead rows + PickBest returns one | §6 |
| **The next state always names a member.** Cells are literals, and a value must be a member. | P9 | §6 |
| **Unwritten columns hold.** So does a table with no row that writes, and an explicit hold row is just a quiescent row with no values. | P9 | §6; c3 tension 4 |
| **A failing condition holds the step.** | P15 + P9 | §5 |
| **Transition tables.** A `step` table keyed on a state column, writing that column. | P6 + P7 + P9 | §6 |
| **Entry tables.** An `entry` table keyed on the state that was entered. | P7 + P6 | §6 |
| **Triggers** at creation, on entry, every step and when rows change (a change re-runs `Prepare`, then `Rebind`). | P7 + P2 + P14 | Consequences |
| **Tables run in DAG order, so a chain settles in one step.** | P11 + P9 (in-step reads see next) | Consequences |
| **State functions restart on reload.** The binding is dropped, the host aborts what it started, and entering the state again starts it with the new code. | P14 + entry tables + P12 | §6 |
| **Orthogonal regions.** Several machines on one object are several instances. | P8 | §6 |
| **Edges are machine state.** | P13 + P8 | Consequences |
| **A seed per instance and step.** The site salt is (machine, clause list, clause): a table alone would make two clauses on one table draw alike, and the row is what the draw chooses. | P4 + P8 | Consequences |
| **Delays are a message to self with `:delay`.** | P12 + no clock | Consequences |
| **Every query picks a row at run time.** | built totality + P10 + built clamp | Consequences |
| **A command cell can hold a comma.** | P3 | §2 |
| **Mutable-table writes from s-expressions.** The host's sink consumer turns them into `insert_row`. | P12 + the built Insert | §2 → host (§8) |

## 4. Worked example

The grazer from the C3 census (§7 of `2026-10-12-c3-bootstrap-fit.md`), cut down to four
states. It uses no machinery beyond §2.

```yaml
machines:
  - name: grazer
    state:
      - {name: mode, type: hash, members: [idle, walk, eat, panic], initial: idle}
    inputs:
      - {name: energy, type: double, range: 0..1}
      - {name: food_near, type: flags}      # host wire: bit 0 set when food is in reach
      - {name: message, type: hash}         # 0 on a step with no message (ruling R3)
    step:
      - from:  grazer_brain
        where: {mode: state.mode, message: input.message}
        given: {energy: input.energy, near: input.food_near}
        set:   {mode: next}
    entry:
      - from:  grazer_enter
        where: {mode: state.mode}
        emit:  command

tables:
  - name: grazer_brain
    read_only: true
    pick: best
    columns:
      - {name: mode,    type: hash, key: true}
      - {name: message, type: hash, key: true}
      - {name: energy,  type: band, input: energy}
      - {name: near,    type: flags, input: food_near}
      - {name: when,    type: condition}
      - {name: next,    type: hash, writes: mode}
  - name: grazer_enter
    read_only: true
    pick: best
    columns:
      - {name: mode, type: hash, key: true}
    suffix: command
```

```csv
# grazer_brain
mode,  message, energy,  near, when,        next
idle,  ,        0..0.3,  1,    ,            eat
idle,  ,        0..1,    0,    ,            walk
walk,  ,        0..1,    0,    ,            idle
eat,   ,        0..1,    0,    ,            idle
idle,  scared,  0..1,    0,    ,            panic
walk,  scared,  0..1,    0,    ,            panic
eat,   scared,  0..1,    0,    ,            panic
panic, ,        0..1,    0,    ,            panic
panic, calm,    0..1,    0,    ,            idle
...
# grazer_enter
mode,  command
eat,   (anim chew), (message food "eat")
panic, (anim run), (message this "calm" :delay random(0.4, 1.0))
idle,  (anim stand)
walk,  (anim walk)
```

Applying the method to this table:

- **The load proves the brain total** over every `(mode, message)` pair that has a run, and
  over every `mode` member, because P10 closes `mode`. Leave out the `panic,,…` row and the
  load is refused: `key (panic, 0): energy 0 has no row without a script condition or flags`.
  `message` is an input hash, so nothing closes it. Whether the brain must cover every message
  is ruling R3.
- **`grazer_enter` is an ordinary table** with an `entry` trigger and a suffix. The delay is a
  command (`:delay` is the sink consumer's business), so nothing about timers is built.
- **Hot reload** (P14): add a member `sleep`, and `mode` keeps its value. Delete the member
  `panic` while an instance is panicking, and `mode` resets to `idle` and `grazer_enter` runs
  for `idle`.

The blank `message` cell is hash 0, "no message", allowed only in a `domain: message` key
(R3, slice 4). The YAML above is the design sketch; the form the front ends read (each key's
`domain:`, the band and flags inputs declared on the table, `set:` in place of `writes:`, the
machine's `message: {type: message}`) is `kGrazerTables` / `kGrazerMachine` in
`test/machine_gtest.cpp`, which the step, effects, reload and parity suites run.

## 5. Slices

All slices run on one branch, `tabular-slice0`, one after another. Each is implemented,
independently reviewed with §9, fixed and committed. The owner reviews the whole branch at
the end (owner, 2026-10-18), and every ruling in §7 is accepted, so no slice is blocked.

0. **Premise repairs** (§10). Done on the branch.
1. **Relations completions:** P1–P5. These need no machine.
2. **Key domains:** every best table's key declares its domain (members, a relation's column,
   or `rows`). Totality is checked over the domain, orphans are reported, and mutable best
   tables are checked on insert and retract. The built per-run checks become one cell layout
   (the P10 layout), which `Load`, `Insert`/`Retract` and later `Prepare` all use.
3. **Machines:** P6–P11. `create_machine`, YAML `machines:`, `Prepare` (proof by layout over
   key domains, including clause outputs), `CreateInstance`, `Step` with the next vector and
   holds, `host_binding.resolve`. The worked example's brain.
4. **Effects:** P12, P15, messages as step inputs (R3), and the `exit` trigger (R6).
5. **Edges:** P13.
6. **Reload:** P14. Bridges and state functions are tested as consequences (R5(b), R6).
7. **Evidence:** P16, P17. The compiled step is `Prepare`'s layout. Parity tests run `Step`
   against the interpreted picks, which are the prior version. Compiling to machine code is
   struck if the budgets are met (R8).
8. **Checklist closure:** move host-adapter items (§8) and the rulings' outcomes into the
   checklist and `definition.md`. Land this spec with an As-built section.

## 6. Interfaces (sketch; the built interface is in As-built)

```cpp
// P1
class owner_handle { public: owner_handle(relation_store&, uint64_t owner); ~owner_handle(); /* move-only */ };
// P2
uint64_t relation_store::Generation(uint32_t table) const;
// P4, P5
uint64_t Draw(uint64_t seed, uint64_t site);
tl::expected<std::optional<size_t>, ErrorContext> PickWeighted(relation_store const&, uint32_t table, pick_query const&, uint64_t seed, uint64_t site);
// P7-P9, P12, P15
struct host_binding {
    std::function<tl::expected<input_getter, ErrorContext>(std::string_view name)> resolve;   // P8
    std::function<void(uint64_t instance, uint32_t table, std::string_view command)> sink;    // P12
    std::function<void(uint64_t instance, uint32_t table, size_t row, ErrorContext const&)> report; // P15
};
tl::expected<counted_ptr<Binding const>, ErrorContext> Bind(counted_ptr<Machine const>, host_binding&&);
instance CreateInstance(Binding const&, uint64_t id, uint64_t seed, void const* self);  // runs create + entry; cannot refuse
void Step(instance&, void const* self, uint64_t seed);                                 // step list, entry on change, swap
// self is the host object, passed per call: getters and conditions read it, and the instance
// stores no pointer that a moved host object would leave dangling.
tl::expected<void, ErrorContext> Rebind(instance&, counted_ptr<Binding>);                        // P14
```

`instance` is snake_case: it is composited into the host's object and never `new`ed.
`Definition` and `Binding` are heap objects shared by instances, held by `counted_ptr`.

## 7. Rulings needed (with recommendations)

Each blocks the slice named. Each option is phrased so that it can be decided by editing
`definition.md`.

- **R1. Query order and fan-out (§3)** are about host queries (`Query::Visible`, `esee`) and
  the commands that consume them, not about Tabular. Tabular's own `PickAll` already returns
  rows in row order.
  *Recommend:* move both to the host adapter's list (§8).
- **R2. Display relations (§3, ?).** One relation with several queries, or several relations.
  The display tables are mutable relations queried with `PickBest`, which is built.
  *Recommend:* one relation per question (window, pointer mode, camera), each total. Nothing
  new in Tabular; the first display table is the test.
- **R3. Messages (§7, ?; blocks slice 4).** *Recommend:* a message is one step with the
  message's type as a hash input and its arguments as inputs. A step with no message has
  type 0. The one place hash 0 is allowed is a blank cell in a message-key column, which
  means "no message". Messages arriving in one tick run as successive steps, in arrival order
  (already settled). A message type no row keys on falls through to the blank-message rows,
  and the host routes it to the object's script when the machine did not absorb it.
  A stale delayed message: the instance counts its state entries, `:delay` captures the count,
  and on arrival `message_stale` is an input flag the table can key on. That stays out of
  Tabular's core; it is the host's input.
- **R4. Two writers (§4; blocks slice 3).** Under P11, two tables with a path between them
  are ordered, and the later write wins deterministically. Two with no path between them make
  the result depend on the order, which promise 4 forbids.
  *Recommend:* refuse two writers of one column **only when no DAG path orders them**. That
  rule follows from P11 and determinism. The checklist's "refuse any two writers" is stricter
  than the contract needs. If you want the stricter rule, it is a contract edit.
- **R5. Bridges (?; blocks slice 6).** Two readings:
  - (a) A bridge is a transition row's suffix command, which the animation system plays.
    This is the definition's current wording.
  - (b) A bridge is states. The transition writes `mode = C, target = B`, and C, D and E have
    rows keyed on `target` that move along when the clip ends.

  (b) emerges from P7–P9 with nothing new, and Tabular's totality checks cover it. (a) keeps
  sequencing outside Tabular.
  *Recommend:* (b) for logic sequences (nod, chew, death poses). Keep (a) only for what the
  animation system alone needs.
- **R6. State functions (blocks slice 6).** *Recommend:* no dedicated table kind:
  - starting is an entry table's command;
  - "it returned" is a message (R3);
  - the host aborts a state's function when the state is left.

  The host learns that the state was left from an **`exit` trigger**, which is a new premise.
  It also gives the door its commands on abort (c3 tension 6). The function-chain table is
  then R5(b) applied to functions. If you want state-function tables as first-class
  instead, the definition stays as written and this becomes a premise.
- **R7. Totality for `weighted` tables.** Every key run needs a positive total weight. That
  is P5's load refusal, not a separate check. No ruling is needed unless you disagree.
- **R8. Compiling the step (§6, ?).** *Recommend:* defer until slice 7 has measured numbers.
  If the interpreted step meets its budget, the compiled path is struck.
- **R9. Compile at bind; `random(...)` inside a command (§7, ?).** Both are about how the sink
  consumer reads s-expressions, which is the host's business.
  *Recommend:* move them to the host adapter's list (§8).

## 8. The host adapter's list (not Tabular)

These are checklist items that `definition.md` already assigns to the host. The checklist
lists them under "Host adapter (not Tabular)" so they never read as Tabular's debt:

- halting proof and read-only checks;
- wire resolution and the snapshot rule;
- query order and fan-out (R1);
- s-expression writes into mutable tables;
- compiling commands at bind, and `random` inside commands (R9);
- the bundle file's container.

Tabular's part of the bundle is building tables and machines from named YAML and CSV texts
(`DocumentFromYaml` and the CSV loaders). The checklist's host list also carries the stale
delayed message (R3), script reading state by name, how a snippet sees wires, and running state
functions from the commands Tabular emits.

## 9. The emergence lens (for every slice's review)

Applied with `review-agentic-systems`. For every check, refusal, branch or mode a slice adds:

1. Is it a premise in §2? If it is not there, ask whether some other premise implies it.
2. If it is implied, find an input where the special path and the premises disagree. That
   disagreement is a bug in one of them.
3. Would deleting the special path, and fixing the premise, make the behaviour hold by itself?
4. The reverse: a premise that is right only because a later special case catches its bad
   outputs.

## 10. Premise repairs found in the built code

Reviewed at Tabular `cf9162b` (2026-10-18), with `review-agentic-systems` plus §9. These are
**slice 0**, built on `tabular-slice0` (Tabular `1f41590`, `02d3721`) together with the
version-totality and owner-overlap rulings. Two more reviews ran on it, and the full suite
passes (6698 passed, 3 skipped). Findings A, B, D and E were re-read against the code.
Each fix lands with the regression test named.

**Emergence defects: a feature built directly, so a premise stayed wrong.**

- **A. "A row-table call refuses a grid" is fifteen separate `is_grid` checks, and one is
  missing.** `OwnerOf` (`relation.cpp:649`) has no check. On a grid it indexes an empty
  `owners` vector, which is undefined behaviour reachable from the public API.
  *Premise:* one accessor decides what a grid answers, and every call that takes a table
  index goes through it.
  *Test:* `OwnerOf` on a grid, under ASan.
- **B. The pivot tie-break is a stored per-row number that only pivoted CSV sets.** Whether a
  load is accepted depends on which front end brought the row in. Owner 2 adds
  `label=1, req=A, window=0..0.5` to a table that holds owner 1's pivoted row `1, A, 0..1`:
  - through `rows_csv` (pivot 0) it outranks the survivor and is accepted;
  - through pivoted CSV (pivot 1) it is dead and refused.

  The tie can only arise because headers are checked for duplicate *names*
  (`pivot_csv.cpp:73`), not duplicate flag sets: `A|B` and `B|A` are two columns with one R.
  *Premise:* each header is a distinct flag set, refused by value. Then two rows can tie only
  by row order, and the `pivots` storage, `load_rows.pivot`, the rank term and the dead-row
  pivot sort all go. That is a contract edit to "Flag matching" (ruling R10).
  *Test:* the two-loader input above.
  Also: a later header spelled `none` silently becomes a flag bit.
- **C. "A band runs low to high" was folded into the range check,** so it holds only for
  ranged inputs. An or table accepts `0.5..0.2` and `nan..1`, and the row never contributes.
  *Premise:* a band cell is well formed (finite, start < end) in every table, checked where
  any band cell is read. The range check then only checks containment.
  *Test:* a backwards band and a NaN band on an unranged or-table input.
- **D. Blank cells are refused per type, and the command type was missed.** `rows_csv.cpp:250`
  stores an empty command. Grid CSV refuses one.
  *Premise:* one rule, "empty is refused unless the column is a condition", at the top of the
  cell loop. The per-type checks go.
  *Test:* a blank command cell in rows CSV.
- **E. A second owner's `LoadGrid` silently replaces the first owner's grid**
  (`grid.cpp:216`), and `Retract` of the first owner then does nothing. Every other load keeps
  other owners' rows.
  *Premise:* a grid has one contributor, and a load from another owner is refused while the
  grid holds rows. That needs a contract line (ruling R10).
  *Test:* two owners each call `LoadGrid` on the same grid.
- **F. The condition function is demanded even when every condition cell is blank**
  (`rows_csv.cpp:162`). Only a non-blank cell needs it.
  *Test:* an all-blank condition column, with no function given, loads.

**Declarations accepted and ignored** (the YAML front end's own rule forbids this):

- **G.** Refuse three declarations that do nothing:
  - an input or band column on a table with no `pick` (the band can be written but never read);
  - a ranking flags column on an all or or table;
  - `read_only: false` on a grid.

  *Test:* one refusal per declaration.

**Structural debt.** These are not defects. Fold them in only where slice 0 touches the code
anyway.

- **H.** The same logic is written more than once:
  - the slot layout three times (`relation_internal.h`, `rows_csv.cpp`, `pivot_csv.cpp`);
  - the 32-bit check four times;
  - the refusal-to-line mapping twice;
  - the `a..b` parse twice (`schema_yaml.cpp` and `ReadCell`).

  One of each. The front ends ask the store for slots.

**Session-claim drift:**

- **I.** `definition.md` order 2's YAML type list is missing `vec2`, `string`, `band` and the
  `inputs`, `pick`, `output`, `pivot` and `grid` keys.
- **I (cont.).** Checklist §5's first item is `[ ]`, but the mechanism it names is `[x]` in §2.
- **I (cont.).** The totality spec's "every other blank cell is refused" was false until D is
  fixed.

**Needing a ruling (added to §7):**

- **R10. Contract lines for B and E.** *Recommend:*
  - for B, headers are distinct flag sets and ties go to row order;
  - for E, a grid has one contributor.
- **R11. Totality is checked only on runs a load adds to.** That is so that `Retract`, which
  is written as an empty load, can never fail. But it also lets a *reload* strand a run.
  Owner 2's ranked row survives, owner 1 reloads without its default row for that key, the
  load is accepted, and the run picks nothing.
  *Recommend:* a load that brings rows (count > 0) also checks every run the owner's old rows
  left. Bare `Retract` stays unchecked (crash recovery must not fail), and P10 closes the gap
  for machine tables anyway.
- **R12. The NaN refusal (my review decision of 2026-10-17)** fires for every ranged input in
  every policy. Its stated reason, protecting proved totality, covers only best tables.
  *Recommend:* restate the reason as "a clamp has no place for NaN", and keep the refusal for
  every ranged input. That way one rule follows from the range declaration rather than from
  the pick policy.

## As-built

Built on `tabular-slice0`, Tabular `fa8832d` (slice 1) to `0fe82ab` (slice 7); slice 0 is
§10. Paths are under `Spehleon/Tabular/`; the gtest suites named are in `test/` and are listed
per item in `docs/feature-checklist.md`.

**Premises.**

| # | As built | Key file |
|---|---|---|
| P1 | `owner_handle`: move-only; dropping it or `Release()` retracts the owner from every table, grids included | `include/tabular/owner_handle.h` |
| P2 | `relation_store::Generation(table)` and `TableVersion::Generation()`: rises on every visible change, never on a refused one | `include/tabular/relation.h` |
| P3 | `suffix: <name>` / `create_table::suffix` adds one trailing command column; in CSV it is the rest of the line | `src/rows_csv.cpp`, `src/schema_yaml.cpp` |
| P4 | `Draw(seed, site)`: three rounds of Support's pcg3d, constexpr, avalanche-tested; `DrawUnit` to [0, 1) | `include/tabular/draw.h` |
| P5 | `pick: weighted` with `weight:`; `PickWeighted` on a version or the store; non-normal weights and overflowing totals refused at load | `src/pick.cpp` |
| P6 | `source`: a state column, a machine input, an earlier clause's value column, or a literal | `include/tabular/machine.h` |
| P7 | `create_machine` (state, inputs, message, create/entry/step/exit lists); YAML `machines:` read by `DocumentFromYaml`; `CheckMachine` is the definition build | `include/tabular/machine.h`, `src/schema_yaml.cpp` |
| P8 | `Bind` makes a shared `Binding` of one `input_getter` per input; `instance` is snake_case, move-only, composited into the host object | `include/tabular/instance.h` |
| P9 | A step copies current to next, clauses `set:` next, next becomes current. `writes:` was dropped (slice 3a): the clause names the state column | `src/instance.cpp` |
| P10 | `Prepare` reads each clause's key domain from its sources and runs the load's own check over it; one layout (groups, cuts, lists) is both the proof and the compiled pick | `src/machine.cpp`, `src/layout.cpp`, `src/version_checks.cpp` |
| P11 | A clause names only earlier clauses; position orders two writers, the later wins; one clause setting a column twice is refused | `src/machine.cpp` (`CheckMachine`) |
| P12 | `host_binding::sink(instance, table, command)`, after the swap, step then exit then entry | `src/instance.cpp` |
| P13 | A band column marked `edge`; `detail::Crossed` is the one crossing test for picks and `Step`; `pick_query::previous`; one previous value per machine input; a `cyclic` table input wraps and is crossed the shorter way around (owner, 2026-10-19) | `src/pick_internal.h`, `src/instance.cpp` |
| P14 | `Rebind`: old exit to the old sink, migration by name, new entry to the new sink; cannot be refused | `src/instance.cpp` |
| P15 | A clause that cannot answer holds; `host_binding::report` once per (clause, row), (clause, input), clause, input or extra arguments, from a bit set sized at creation | `src/instance.cpp` |
| P16 | `instance::Provenance()`: a ring of 16 clause runs, debug only, switched by a generated header | `include/tabular/instance.h`, superproject `CMakeLists.txt` |
| P17 | 19 benchmarks, gated at `-O2` by `scripts/test-tabular-budgets.sh`; `definition.md`'s table is held to them both ways | `bench/budgets.cpp`, superproject `scripts/test-tabular-budgets.sh` |

Beyond §2, slice 2 built key domains (`key_domain`: members, a foreign column, `rows`, and in
slice 4 `message`), orphans and the `run_note` report, and mutable best tables; slice 4 built
messages (R3) and the exit trigger (R6). Bridges, state functions and function chains are
consequences with tests and no code (slice 6). The compiled step is `Prepare`'s layout; nothing
compiles to machine code (R8, see Open owner decisions).

**Where the interface differs from §6's sketch** (`include/tabular/*.h`):

- The definition is the `create_machine` command struct (views into a `schema_document` when
  read from YAML). The sketch's prepared `Definition` is `Machine`: `Prepare(create_machine
  const&, relation_store const&, std::vector<run_note>* report = nullptr)` returns
  `counted_ptr<Machine const>`, and `Machine::Stale(store)` says when to prepare it again.
- `host_binding::resolve` returns an `input_getter` (`{read, data, release}`, read as
  `read(data, self, error_out)`), not a `std::function`, so a step's read is one indirect
  call. `report(instance, table, row, error)` carries the instance id and uses `kNoTable` /
  `kNoRow` for a getter failure or extra message arguments.
- `Bind(counted_ptr<Machine const>, host_binding&&)` returns `counted_ptr<Binding const>`, as
  sketched.
- `CreateInstance(Binding const&, uint64_t id, uint64_t seed, void const* self)` returns the
  `instance`, as sketched.
- `Step(instance&, void const* self, uint64_t seed, message const& = {})`: the sketch had no
  message; a step takes at most one (R3).
- `Rebind(instance&, Binding const&, void const* self, uint64_t seed)` returns `void`; the
  sketch's `tl::expected<void, ErrorContext> Rebind(instance&, counted_ptr<Binding>)` could not
  be refused in any case (slice 6), and the exit and entry lists need `self` and `seed`.
- Added to the sketch: `instance::State`, `Answer(list, clause)` (a `clause_answer`),
  `Absorbed()`, `Provenance()` (debug builds); `DrawSite(machine, list, clause)`; `DrawUnit`;
  `CheckMachine`; `DocumentFromYaml`; the `run_note` report on loads, inserts, retracts and
  `Prepare`; `PickWeighted` and the other picks on a `TableVersion` as well as on the store;
  `owner_handle::Release()` and `holds()`.

## Open owner decisions

The owner ruled on these on 2026-10-19; `definition.md` carries each ruling.

**Decided.**

- **Entry and exit may not set a hash state** (slices 3b, 4): kept. A change of state is a step
  row, written where it can be read.
- **An exit that holds sends no abort** (slice 6): kept. The host aborts a state's function on
  every change of the state, whatever exit emits, so nothing keeps running; this is on the host
  adapter list.
- **Cyclic inputs for edges** (slice 5): built. A ranged input may declare `cyclic: true`; a pick
  wraps it into the range, and a crossing takes the shorter way around, half a period upward.
- **Plain mutable table change cost ∝ table size** (slice 7): kept. Nothing changes a relation
  every frame.
- **Bounded repeat** (slice 6): not Tabular's. It is a command the host runs.

**Still open.**

- **Messages** (slice 4, R3): the owner is unhappy with the built form (type as a key,
  positional arguments every type shares, a missing one reading 0). A message is a kind of
  command, and commands turned out far more heterogeneous than expected, so the direction is
  that messages are opaque to Tabular. Open: whether and how a table reads a message's
  arguments (a collision's normal vector). Per-type arity and clause-to-message-key passing
  fold into this. The code stays as built.
- **The stranded-dependent rule** (slice 2): kept for now; the owner finds it bad with no better
  rule yet. Candidate: no load may add a gap, and gaps a bare retract left are reported but do
  not block.
- **R8, compiling the step**: the question is whether the searches to the tuple and the cell
  become a direct index built at `Prepare` (versions are immutable, the queries repeat). Load:
  about 2,000 machines in the full game at 1 ms frames, most asleep in state functions; no firm
  target. The 400 MHz budget is the slice's (2 of the game's 20 metarooms), a scaling test, not
  a target machine.
- **Arithmetic on data values**: a more general problem than `energy + 50`; the owner is not
  ready to decide it.

**Left as built, not raised:** `rows` key columns as sub-tuples (slice 2); clause outputs
supplied by the machine at `Prepare` (slice 3a); function-chain evidence resting on the bridge
test (slice 6); the grazer build budget at 1200 µs and the single-machine budget gate (slice 7).
