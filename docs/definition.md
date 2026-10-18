# Tabular: definition

This is the contract. Every change to Tabular is reviewed against it: does the change keep
these promises? Changing the contract is a deliberate edit to this file, made before the
code that depends on it. The reasoning behind each line is in `docs/project-notes/`.

## What Tabular is

A library of **small, total, decidable tables** and the **state machines** built from them.
Given its inputs, a table always picks a row. A machine uses tables to decide its next state
and to resolve what a state means when it is entered.

Tabular can be defined, run and tested **without a world**: no objects, no threads, no script
engine, no files.

**Why it exists: the hot-reload line (owner, 2026-10-12).** The main script engine can already
express all of this. It is the full Turing machine, with `wait` and `lock` (cooperative, no
preemption). What it does not do is cooperate with hot reload, because too much is going on:
suspended scripts, references between modules, and constructors that are methods, so
construction and behaviour share one reload unit. (CAOS kept install scripts separate from the
event scripts that were replaced on live agents.) The goal is to **hot reload a `.tabular` file
without thinking much**, so as much behaviour as possible goes into it. The restrictions below
are the price of that line.

**Reload order (owner, 2026-10-15).** The host's whole-world reload comes first: save state, rebuild
the modules, restore objects by field name without running constructors. Tabular is a compartment
carved from that, a part that can reload on its own because its constraints make that safe. It
does not have to make reload safe by itself, because the whole-world reload is always the fallback.

**The rule every other line answers to (owner, 2026-10-15).** The engine's assumptions and a
scripting library's assumptions are opposed. A scripting library assumes code owns data: one
heap, references anywhere, closures, callbacks, constructors that are methods, state kept on a
suspended stack. The engine assumes it owns the data and the code can be replaced, so code
must hold no state, nothing may refer into it, and what it touches must be known. So
**everything that crosses the boundary is in the engine's terms**: values (`int64_t`, `double`,
hashes, handles as tombstones), records (commands, messages) and names the host resolves. It is
never a reference, a callback or script state. When a feature needs something else to cross,
the feature is wrong, not the rule.

## What it promises

1. **Totality.** Every query picks a row. Tables are outer-join oriented: the less specific
   rows never stop matching. Over finite key domains, totality is checked when a definition
   is built. A table whose rows are guarded by opaque conditions must end in an unconditional
   row or an explicit hold.
   Built for best tables (owner, 2026-10-18): a published version is total. A load that
   brings rows checks every key run of the version it would publish, not only the runs it adds
   rows to: the rows with no script condition (and, when the table ranks, no flags) cover every
   value of the band inputs' declared ranges, and a row the rows tried before it always cover
   is refused as dead. A run another owner's bare retract stranded therefore refuses the next
   load that brings rows, and retract-then-load agrees with a one-step reload. A bare retract
   is a load of no rows and is never checked, so crash recovery cannot fail. Two owners' rows
   with equal flags may not overlap in one run (see Flag matching).
   Hash key domains are closed by a machine, which relates its keys to each other, so a key
   with no run is not checked until machines exist. An all or an or table may answer nothing.
2. **Decidability.** Rows are finite, there is no recursion, and within one step the tables
   that feed each other form a DAG. Whatever the loader checks about a table, an author can
   also know by reading it.
3. **Isolation.** A step touches only its own instance. It reads inputs only through names the
   host resolves and writes only its own state. Everything else leaves as opaque records
   (commands, messages) handed to a host sink. So steps for different instances may run in
   parallel.
4. **Determinism.** Given the same definition, inputs and seed, a step produces the same
   state and records. Randomness is a counter-based draw from a seed the host supplies, salted
   per draw site, with no stored generator state. Built: `Draw(seed, site)` in
   `tabular/draw.h`, a pure hash (Support's pcg3d, two rounds), and `DrawUnit` to turn a
   draw into [0, 1).
5. **Replaceability.** A definition is immutable once built. Hot reload builds a new one, binds
   it and migrates instance state by name. A failed rebuild leaves the old one running.

## What it is not

- **Not a language.** Conditions and other snippets are opaque text, compiled by the host into
  closures Tabular calls. Tabular never parses, types or links the host's language.
- **Not a scheduler.** It does not decide which instances step, when or on which thread, and
  it does not decide whether a table needs to run again. Delays (timers) resolve outside it
  too.
- **Not file IO.** It receives text or builder calls, never paths.
- **Not the owner of wires, the bundle file or hot reload policy.** Those belong to the
  subsystem that has Tabular. Tabular cannot tell a wire from any other named input.
- **Not a mirror of the host's types.** A comparison like `energy < 100` is a script
  condition, never a native one. Folding numeric comparisons in was tried in the first
  version: it then needed to know what a wire is, inputs stopped being opaque, and it had to
  mirror AngelScript's types. Native conditions work only on Tabular's own input types:
  **hash, flag set, `int64_t` and `double`.** The host converts its values to these. A flag
  set (AngelScript `uint32`) and a hash (`CityHash32`) are carried as `int64_t` and differ only
  in how a condition reads them (subset versus equality), so a wire of either needs no extra
  type. A native
  comparison is one input against a literal (`energy < 100` is allowed as a band or comparison);
  anything that combines inputs, does arithmetic or calls is a script condition.
- **No timers or counters (owner, 2026-10-12).** Tabular has no clock. A delay is a command to
  the outside, a message to self that arrives later:
  `(message :who this :from this :what "panic_cooldown" :delay random(0.4, 1.0))`.
  The table absorbs it when it arrives. Accumulators such as energy are object state the host
  owns; tables read them as inputs.
- **Not a Turing machine.** Anything that needs one is a message to script.

## Structure

**Two layers.**

| Layer | Contents |
|---|---|
| Relations | rows, key columns, match operators, pick policies; queries |
| Machines | a state vector, transition tables, entry tables, bridges, state-function and function-chain tables, edges |

**A row (owner, 2026-10-12).**

    [ keys … ] [ conditions … ] [ weight ] [ values … ] [ suffix … ]

- **Keys:** several allowed. They say which row this is. **A key is equality**, so keys can be
  indexed: finding a row's candidates is a lookup, not a scan.
- **Conditions:** whether this row counts. **Every match that is not equality is a
  condition** (flags subset, band, edge, script). **At most one script condition per row;**
  any number of the others. A row may carry several, of different kinds (for
  example a band, a flag and a script condition); **all of a row's conditions must hold**
  (owner, 2026-10-16). If a row does not count, evaluation falls through to the next one.
  Built: a band (`a..b`, half-open, compared in float as animation's phase bands are), a flags
  ranking column (it ranks, and never fails a row) and the one script condition; edges stay
  to be built. A row with no script condition carries a closure with no function; a blank
  condition cell in CSV is one.
- **At most one weight** per row, and only in a table with no conditions (see Picking).
  Built: a `pick: weighted` table names its one `double` weight column (`weight:` in YAML,
  `create_table::weight` in the builder).
- **Values:** several allowed. A value is a **write** to the instance's own state vector.
  Writes always go to the *next* vector, which is swapped in at the end of the step. **What
  is forbidden is computing a state from a state** (owner, 2026-10-12): a machine row like
  `state, state'` = `a, a+1` makes the state space arithmetic instead of an enumerated set.
  A next state always names a member. Arithmetic on data, such as `energy + 50`, is not what
  the rule was avoiding, and may be allowed (tentative).
- **Within a step, tables read in DAG order.** A table may read a value another table wrote
  earlier in the same step, so a chain (phase → lookup → bridge) settles in one step. State
  kept across steps becomes visible at the swap.
- **Suffix:** N trailing cells that are not declared columns, as in the bridge table's C, D, E.
  It is what the row does when picked: **a command, which is enqueued, not written.** A
  command is not a value. The suffix *as a whole* is the command: `(C, D, E)` is passed, as an
  s-expression, to whoever consumes it, and that consumer interprets it. Tabular does not.
  Bridge steps are such a command.
  Built (2026-10-18): a table declares `suffix: <name>` (YAML) or `create_table::suffix`
  (builder), which adds one Command column of that name after its declared columns, so the
  store and the picks see an ordinary command column. In CSV the suffix is the rest of the
  line: the text from the first cell after the declared ones to the end of the line, commas
  and spacing between its parts kept exactly, trimmed only at its two ends. The header names
  the declared columns and ends with the suffix column's name (the reader needs that last
  header cell to know where the rest of each line starts); a header that names anything else
  extra, or puts the suffix column anywhere but last, is refused. A row with no suffix text, or
  no cell for it, is refused naming its line. A grid or a pivoted table cannot have a suffix
  (a pivot cell holds one value), and a pivoted load into a suffix table is refused because the
  table has no pivot.

**Flag matching (owner, 2026-10-12).** For an input flag set `F` and each row's flag set `R`:

The order as built, the same as animation's `LookupBest`:

1. a row whose flags are all in the input (`R & F == R`) comes first;
2. then the larger `R & F`, compared as an integer. Higher bits outrank all lower bits
   together, so **the bit order of a flag set is its priority**;
3. then the larger `R`.

Pivot headers are distinct flag sets (two spellings of one set, such as `A|B` and `B|A`, are
refused by value), so rows tie only when their flags are equal, and **ties go to row order**,
whichever front end loaded them. A row with `R != 0` and
`R & F == 0` never wins. The row with `R == 0` is tried last, after every ranked row, and in
every key run a load checks the `R == 0` rows with no script condition must cover every
input: they are the clear default (see promise 1). Every table
needs a well-defined answer to "what happens if nothing matches".
Several `R == 0` rows in one run are tried in row order; animation's `LookupBest` keeps the
last, but its lookup grid has one None column, so no table it reads has two.

**Equal flags never overlap between owners (owner, 2026-10-18).** In one key run, two rows
from different owners with equal flags (every row of an unranked table has the same flags)
whose boxes meet (every band input's intervals intersect; with no band inputs any two rows
meet) are refused at load, naming both owners and describing a kept row by its key, flags and band values, scripted or not: that is two
contributors claiming the same inputs, which only an order among owners could decide. The check precedes the
dead-row check. One owner's rows may overlap; their order within the owner's load is their priority.

**Row order is a function of the loads (owner, 2026-10-18).** Reloading identical rows is
idempotent: a version depends on which owners hold which rows, never on who loaded last. Rows
sort by the table's order columns, then by owner id, then by index within that owner's load.
So rows with equal order keys sit grouped by ascending owner id, and an owner reloading the
same rows leaves every row where it was; PickAll's order, a weighted run's cumulative order and
the order rows are tried in all follow it.

**The None header (2026-10-18).** A pivoted best table's flag headers are read left to right.
`None`, in any case, names the 0 value and is only the first header of a best table; spelled
anywhere else, or on an or table, it is refused, because it would otherwise become a flag bit
no one meant.

**What the schema refuses on a pick.** An input is read only by a pick, a band is read only
through an input, and only a best table ranks:

- a table with no pick that declares `inputs` (or a band or ranking column that would need
  one) is refused: it is written and never read;
- an all or an or table with a ranking column (a flags column reading an input) is refused;
- a picked table or a grid declared writable (`read_only: false`) is refused.

**Picking (owner, 2026-10-12).**

- **Ranked fall-through.** Keys select the candidate rows. Flag matching ranks them (rows
  without flag conditions keep row order). Evaluation takes the best-ranked row whose other
  conditions hold; if they do not, it falls through to the next-ranked row, down to the
  default.
- **A weighted table has keys, a weight and outputs, and no conditions** (owner, 2026-10-12).
  It is a transform with a probability: keys (any number, equality) choose which
  distribution applies, and the draw is across the selected rows. So a weight reads as its
  probability, and nothing is tested before drawing. When a roll needs a condition, an
  earlier table in the DAG computes a key for it. This restriction can be lifted later
  without breaking files; adding it later could not.
  Built (2026-10-18): `pick: weighted` with `weight: <double column>`. The schema refuses a
  condition column and any declared input (a band or a ranking flags column can only read an
  input, so refusing inputs refuses them), a missing weight or one that is not a double, a
  `weight` on a table that is not weighted, and a weighted table that is not read-only or
  whose order does not start with its keys. A load refuses a weight that is not a normal positive number
  (zero, negative, subnormal, infinite or NaN), and a key run whose weights total more than a double holds, so every non-empty
  key run has a positive finite total. `PickWeighted(version | store, table, query, seed,
  site)` lays the run's rows end to end in row order and takes the row holding the point
  `DrawUnit(Draw(seed, site)) * total`; it answers nothing only for a key with no run.
- **A pivoted flag output ORs together**, as animation's phase tables do today
  (`PipelineClosure::eval_phase_bits`). The phase table writes every flag whose band holds,
  combined with OR: each row whose keys match contributes the flag of every cell whose band
  holds, rows sharing a key accumulate, and a blank cell contributes nothing. Bands are
  half-open, `start <= x < end`. When nothing holds the answer is 0, so such a table needs no
  default row.

**Pivot is presentation of any axis.** The clip table pivots a key (kind) into the column
headers, with values in the cells. The phase table pivots the *output* into the headers: the
flags across the top are the value column, and the cells are bands, i.e. conditions.
A key by key pivot is a **grid**, stored densely with every cell filled (a blank is an
error). **A grid has one contributor** (owner, 2026-10-18): it is read-only, and a load from
another owner is refused while the grid holds a version owned by someone else. A flags-of-key pivot and an output pivot are ordinary rows sorted by key, which the CSV
front end expands.

**When a table runs (triggers).** At bind or instance creation; on entering a state (entry
tables); every step; when a relation's rows change.

**Lifetimes.**

| Object | One per | Holds |
|---|---|---|
| Definition | table section of a bundle | tables, machines, snippet text; immutable |
| Binding | definition × host object type | compiled closures, resolved input slots, state layout |
| Instance | object | state vector, edge history |
| Relation | global relation | live rows, each owned by a contributor; RAII handles retract them (`owner_handle`) |

**Owner handles (built 2026-10-18).** An `owner_handle` (`tabular/owner_handle.h`) holds a store
and an owner id; dropping it, or calling `Release`, retracts every row the owner holds, and a
moved-from handle retracts nothing. It retracts from every table, read-only tables and grids
included, because "RAII handles retract them" says nothing of how a row arrived and the handle
is for crash recovery: loaded rows that outlive their contributor have no one left who would
retract them. A reload does not use a handle; it loads again. Retract is never refused, so a
destructor always finishes.

**Generation (built 2026-10-18).** Each table has `relation_store::Generation(table)`, a counter
that rises on every change a reader can see (an accepted insert, a published load, a retract
that removed rows, a grid load) and never otherwise (a refused change, a retract of an owner
with no rows there, a load of no rows for an owner with none). "The rows changed" is the
counter moving; there is no callback.

**Column types beyond the four native ones (owner, 2026-10-16).** A string column holds a
`counted_string`. A `vec2` column holds two floats, one 8-byte cell like every other value;
it is the only vector type (no `vec3` or `vec4`). A band `a..b` is a `vec2`. There is no
position or `Location` type: a 2D position is a string column naming the Section plus a
`vec2` column, and a 3D one adds a second `vec2` or a `double`. A handle is a `uint64_t` stored in an `int64_t` column: punning, not a type.

**Input ranges (owner, 2026-10-17).** A double input may declare `range: lo..hi`, half-open;
a best table's band inputs must. A pick clamps the input into the range (the top end to the
largest float below `hi`, so the top value is in the top band), and a band reaching outside its
input's range is refused. A pick given NaN for a ranged input is refused, in every policy: a
clamp has no place for NaN (owner, 2026-10-18).

**A band is well formed (2026-10-18).** A band cell has both ends finite and start before end,
in every table whatever its input's range, checked where the cell enters a table. The range
check then only tests containment.

**Surface.** A builder API is the core, fed command structs. YAML (definitions) and CSV (rows) are
front ends that produce those commands. Public headers live in `include/`; everything else is private.

## The host interface

The host supplies:

- **The condition function** given to the loader: cell text in, compiled condition out. It
  takes only text; Tabular hands it no symbol table. It is for script conditions, which may
  be as complicated as the host allows, wires included. A native condition (a band such as
  `a..b`, a flag set) is read by Tabular itself and never reaches it (owner, 2026-10-16).
  **It rejects any snippet it cannot prove halts** (owner, 2026-10-12): loops over finite
  query results or bounded counts are allowed; anything Rice's theorem applies to is not. A
  read-only snippet (a condition, or a wire behind an input) may call only read-only functions.
  This is how decidability survives opaque host code: the host promises it by refusing.
  **Functions that take a script callback are out of bounds** (owner, 2026-10-15), including
  Query's `where`, `random`, `count`, `find`, `closest`, `min`, `max` and `sortBy`. As declared
  they run arbitrary script. To allow them, a function's footprint would have to depend on the
  function passed to it. That is the script engine's problem, not Tabular's.
- **Input resolution** for every name a table reads that Tabular does not own.
- **Sinks** for emitted commands and messages.
- **A seed** per instance and step.
- **Running state functions:** start a named function for an instance, report when it returns,
  and abort it.

Closures are `{function, data, release}`. `release` is called when a binding is dropped.

## State functions (owner, 2026-10-15)

The grazer translation fit mostly in tables, but not as "90% of objects fit". It was "about
10% of *each* object does not fit". The C3 bootstrap census
(`docs/project-notes/2026-10-12-c3-bootstrap-fit.md`, §8) located most of that 10%: **read from
self, write to self** (accumulators, a head nod repeated 1-4 times, sequences that wait) and
**read from world, write to private** (a sticky target, a scan done only when needed). Both
become state functions: in the second case a wire does the world read, and the function writes
the result into its own entity.

**A state function is an ordinary script function in the host's script, named by a table.**
Tabular holds only its name, resolved at bind like any other name it does not own. It is not
part of the bundle, and nothing about it is compiled by Tabular. Two tables refer to them:

- **The state-function table** maps a state to the function that runs while the machine is in
  it, as the clip table maps a state to a clip. Entering the state starts the function. While
  it runs, the machine holds the state. When it returns, the machine steps again, and its
  transition rows can read that it finished.
- **The function-chain table** maps a move from one function to another onto the chain of
  functions that runs between them, as the bridge table does for clips. It is the hierarchical
  machine's transitions between functions.

A state function **may suspend** (`wait`, `lock`): waiting is often its point. Halting is
required only of code that runs inside a step (conditions, wires) and of commutative writes. In
exchange it **reads and writes only its own entity**, which the host checks as an asserted
footprint on the function. Anything it needs from the world comes through wires, read fresh, so
it never acts on a view that went stale while it waited.

**Reload.** The functions belong to the host's script and reload with it. When a binding is
dropped or rebuilt, a running state function is aborted and its state is entered again, so it
restarts with the current code. Whatever it had already done stays done. A function for which
that matters must be safe to restart, or its state's entry rows must decide what happens.

## Budgets

Each cost has a number and a benchmark that fails when the number is broken. The numbers come
from measurement before code that depends on them lands.

| Cost | Paid | Budget |
|---|---|---|
| build a definition from text | per section, on load and reload | TBD |
| bind | per definition × object type (shared by every instance) | TBD |
| create an instance | per object | TBD |
| step | per instance per tick | TBD per row evaluated |
| relation insert, retract, query | per change or query | TBD |

## First version (owner, 2026-10-15)

**Plain data tables: the Relations layer alone, with no machines.** For example:

    spawn_points(owner, package, section, where)

Every row carries the **owner** that contributed it, so reloading a contributor is "retract
every row it owns, insert the new ones". That is the install script done as data: what an
install placed is recorded as rows, so it can be found again and replaced, which a constructor
could not offer. What happens to objects already spawned from a retracted row is the reload
policy of the subsystem that owns the relation, not Tabular's. Tabular reports that the rows
changed through the table's generation counter (see Generation).

**Order of work (owner, 2026-10-15).**

1. **Table definitions are command-pattern structs** (built: `tabular/relation.h`). Creating a
   table, declaring its columns, inserting and retracting rows are each a flat command record
   handed to the builder. The core is complete and testable with no parser.
2. **YAML front end** (built: `tabular/schema_yaml.h`): a table definition written in YAML
   becomes those commands, through the `Spehleon/lib/Yaml` markup walker. A `tables:` sequence;
   each table has `name`, `columns` (`{name, type, key, input}`, type one of hash, flags, int,
   double, condition, command, vec2, string, band), an optional `order_by: {columns: [names],
   unique}`, an optional `read_only`, and for a picked table `inputs` (`{name, type, range}`),
   `pick` (none, best, all, or, weighted), `output` (an or table's flags column), `weight` (a
   weighted table's double column), `suffix` (the name of a trailing command column, see Suffix)
   and `pivot: {headers, cells}`. A grid is `grid: {rows, columns, cells}` instead of columns. A grid and a picked
   table are always read-only: the key may be left out (it is read as `true`), and
   `read_only: false` on either is refused. The builder takes `read_only` explicitly. Any diagnostic fails the definition, an unknown key included, because a
   misspelled `unique` silently ignored is a table that is not unique.
3. **CSV front end** (built: `tabular/rows_csv.h`): rows loaded from CSV text into a read-only
   table. The header names the columns in any order, every non-condition column exactly once.
   A hash cell is `CityHash32` of its text; flags are unsigned decimal or `0x` hex below 2^32;
   an int is signed decimal; a double is read whole; a command is the text verbatim. A cell
   must parse whole, and an empty cell is refused in every column type, except a blank
   condition cell, which is no condition. A condition cell is handed, as text, to a
   function the caller passes to the loader (`csv_rows::condition`, returning
   `tl::expected<closure, ErrorContext>`), which returns the compiled condition; that is the whole of Tabular's part in compiling
   conditions (owner, 2026-10-16: expanding Tabular to own this went badly in the first
   version). The function is needed only when a non-blank condition cell appears. CSV has no quoting, so a command cell cannot hold a comma, except the suffix of a table that
   declares one, which is the rest of the line. Cells are trimmed; blank lines and `#` lines are skipped; a
   leading BOM is dropped; text is not Unicode-normalised, so a hash cell hashes the bytes a
   script literal of the same name hashes. A refused load names the line, and for a unique
   clash the line it clashes with. Within one load, two texts with one hash, or a text
   hashing to 0, are refused, naming both lines.
   **Column set** (built: string, `vec2` and condition columns, in `tabular/relation.h`): a string
   cell is an interned `counted_string` (a caller may pass one it holds, or a view), and a string
   key column is found by text; a `vec2` is two floats, written `a..b` in CSV; read-only tables
   hold conditions, each released once when the last version holding its row lets go.
   **Picking and grids** (built: `tabular/pick.h`, `tabular/grid.h`): inputs, bands and a ranking
   column; `PickBest`, `PickAll`, `PickOr`; pivoted CSV; grid tables and grid CSV.
4. **Machines** built from tables.

**Two kinds of table (owner, 2026-10-15).**

- **Read-only tables** are loaded from files (CSV) and never updated. A reload builds a whole
  new version of the table, checks all of it, and only then replaces the old one. The old
  version is never modified, so a reader holding it keeps a consistent view until it lets go.
  Machines are made from these.
- **Mutable tables** are engine data that script expressions and s-expressions add to:
  who owns the main screen's state, programmable gambits, spawn tables. They are never loaded
  from CSV. No case is known for a table that is both loaded from CSV and mutated, so each
  kind refuses the other's writes.

**A machine is a SELECT** over read-only tables, checked to always produce exactly one row,
then compiled to run fast (possibly to machine code; undecided). Immutability is what makes
that check and that compilation hold.

## Undecided

These are open, not missing. Each is decided by editing this file.

- Whether a command's consumer compiles its s-expression at bind time (as the host compiles
  snippets), so a step enqueues a prebuilt record rather than text.
- What a bridge step carries (state or clip, exit, commands on entry, bounded repeat), and
  whether a function-chain step carries the same.
- Random across the boundary. The oddity to remove: `random()` called from a script snippet
  inside a table uses a different generator from `random()` in ordinary script. Likely
  resolved host-side, with access masks choosing the deterministic generator in table context
  (owner: "doable"). Open: how `random(...)` inside a command's s-expression is drawn.
- The snapshot rule: wires read other entities' **committed** state as of
  the start of the step (needed for parallel steps; census §8 strain 5).
- A delayed message that arrives after its state has already ended and been re-entered.
- How a table absorbs a message, and in which step. (Settled: it can, one at a time, in
  arrival order.)
- The display relations: one relation with several queries, or several relations.
- How a host-compiled snippet sees wires. Leaning (owner): free wire names become parameters,
  and the wirer's closure captures and forces them. See the shape note §3.
