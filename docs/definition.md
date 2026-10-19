# Tabular: definition

This is the contract. Every change to Tabular is reviewed against it: does the change keep
these promises? Changing the contract is a deliberate edit to this file, made before the
code that depends on it. The reasoning behind each line is in `docs/project-notes/`. Ruling and
premise numbers (R1–R12, P1–P17), "the spec" and its worked example (§4) refer to
`docs/superpowers/landed/specs/2026-10-18-tabular-remainder-design.md`, which also lists what
was built per premise and the decisions still open for the owner. `docs/feature-checklist.md`
maps every feature to the tests that hold it.

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
   Built for best tables (owner, 2026-10-18): a published version is total over its key
   domain (see Key domains). A change that brings rows (a load of one row or more, or an insert
   into a mutable best table) checks every key tuple in the domain of the version it would
   publish, not only the runs it adds rows to: the rows with no script condition and no edge (and, when the
   table ranks, no flags) cover every value of the band inputs' declared ranges, a tuple with no
   run is refused, and a row the rows tried before it always cover is refused as dead. A run
   another owner's bare retract stranded therefore refuses the next change that brings rows,
   and retract-then-load agrees with a one-step reload. A bare retract (or a load of no rows)
   is never checked, so crash recovery cannot fail; it reports what it leaves instead. Two
   owners' rows with equal flags may not overlap in one run (see Flag matching).
   A machine clause's key is proved by the machine's `Prepare` over the domain its source closes
   (see Machines). An all or an or table may answer nothing.
2. **Decidability.** Rows are finite, there is no recursion, and within one step the tables
   that feed each other form a DAG. Whatever the loader checks about a table, an author can
   also know by reading it.
3. **Isolation.** A step touches only its own instance. It reads inputs only through names the
   host resolves and writes only its own state. Everything else leaves as opaque records
   (commands, messages) returned to the host that ran the step. So steps for different instances may run in
   parallel.
4. **Determinism.** Given the same definition, inputs and seed, a step produces the same
   state and records. Randomness is a counter-based draw from a seed the host supplies, salted
   per draw site, with no stored generator state. Built: `Draw(seed, site)` in
   `tabular/draw.h`, a pure hash (three rounds of Support's pcg3d), and `DrawUnit` to turn a
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
| Machines | a state vector, inputs and four clause lists (create, entry, step, exit) over picked tables; transition and entry tables are clauses; bridges, state functions and function chains are states and clauses (R5(b), R6); edges |

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
  ranking column (it ranks, and never fails a row), an edge (a band marked `edge`, see Edges)
  and the one script condition. A row with no script condition carries a closure with no
  function; a blank condition cell in CSV is one. A row with no edge holds `NoEdge()` in the
  edge column; a blank edge cell in CSV is one.
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
  command is not a value. The suffix *as a whole* is the command: `C D E` is passed, as one
  s-expression list `(C D E)`, to whoever consumes it, and that consumer interprets it. Tabular
  does not.
  Bridge steps are such a command.
  Built (2026-10-18): a table declares `suffix: <name>` (YAML) or `create_table::suffix`
  (builder), which adds one Command column of that name after its declared columns, so the
  store and the picks see an ordinary command column. **Commands as lists** (built
  2026-10-19, commands-and-messages spec Part 2). In CSV the suffix is the row's cells after the
  declared ones, each read as any cell is (quoted or trimmed). Cell boundaries mean nothing: the
  cells are joined with one space and read as a sequence of s-expressions, so `(emit sound)` in
  one cell and `(emit` | `sound)` across two read the same, and Tabular wraps the sequence into
  the one list the store keeps: `((emit sound) (play particle) (message target hit 3))`. No
  suffix cells (or only empty ones) is `()`, "do nothing", and is not refused. A top-level item
  is a list, which names what it does by its head, or a single word (`hard-land`, `idle`,
  `:lock`), which the consumer reads (the animation consumer: a clip, or a directive to
  itself). Outside an open parenthesis, a cell of more than one item that does not start with
  `(` is one command written without its parentheses and is wrapped: `message this hard-land`
  reads as `(message this hard-land)`. A string is one item, so a cell holding just a string is
  not wrapped. A cell of several items that does not start with `(` and is not whole by itself
  (`message food (say`, `a) (b`) is refused, naming the line, since wrapping it would guess
  where its parenthesis was meant to go; a command that runs across cells starts with `(`. A
  cell that is exactly `()` is the explicit "do nothing" and adds nothing. **Every command
  column is a list of commands** (2026-10-19): a declared command column's one cell is read by
  the same rule, so `anim chew` and `(anim chew)` both read as `((anim chew))`, `()` stays
  `()`, and a host reads every result the same way whichever kind of column it came from (a
  blank declared cell is still refused, as every blank CSV cell is). The store checks every
  command, whichever surface wrote it, and nothing more: it is exactly one list, lexically
  parentheses, double-quoted strings (`\"` and `\\` escape inside one; a backslash escapes
  whatever follows it) and atoms. A `)` that closes nothing, a `(` or a string never closed,
  text that is not one list (`(a) (b)`, a bare word), or an empty command is refused, naming the
  line in CSV. The list is built once, at load, so a step never parses or builds text. A builder caller
  writes the list itself, as the store keeps it. The header names
  the declared columns and ends with the suffix column's name (the reader needs that last
  header cell to know where the suffix cells start); a header that names anything else
  extra, or puts the suffix column anywhere but last, is refused. A grid or a pivoted table cannot have a suffix
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
every key run of the domain a change checks the `R == 0` rows with no script condition and no edge must
cover every input: they are the clear default (see promise 1). Every table
needs a well-defined answer to "what happens if nothing matches".
Several `R == 0` rows in one run are tried in row order; animation's `LookupBest` keeps the
last, but its lookup grid has one None column, so no table it reads has two.

**Equal flags never overlap between owners (owner, 2026-10-18).** In one key run, two rows
from different owners with equal flags (every row of an unranked table has the same flags)
whose boxes meet (every band input's intervals intersect; with no band inputs any two rows
meet) are refused at load or insert, naming both owners and describing a kept row by its key, flags and band values, scripted or not: that is two
contributors claiming the same inputs, which only an order among owners could decide. The check precedes the
dead-row check. One owner's rows may overlap; their order within the owner's load is their priority.

**Row order is a function of the loads (owner, 2026-10-18).** Reloading identical rows is
idempotent: a version depends on which owners hold which rows, never on who loaded last. Rows
sort by the table's order columns, then by owner id, then by index within that owner's load.
So rows with equal order keys sit grouped by ascending owner id, and an owner reloading the
same rows leaves every row where it was; PickAll's order, a weighted run's cumulative order and
the order rows are tried in all follow it. In a versioned mutable table (a best one, or one a
best table's key domain comes from) an insert takes the index after the owner's other rows, so an owner's rows keep its insertion order, and a retract
followed by the same inserts gives the same version.

**The None header (2026-10-18).** A pivoted best table's flag headers are read left to right.
`None`, in any case, names the 0 value and is only the first header of a best table; spelled
anywhere else, or on an or table, it is refused, because it would otherwise become a flag bit
no one meant.

**What the schema refuses on a pick.** An input is read only by a pick, a band is read only
through an input, and only a best table ranks:

- a table with no pick that declares `inputs` (or a band or ranking column that would need
  one) is refused: it is written and never read;
- an all or an or table with a ranking column (a flags column reading an input) is refused;
- an all, or or weighted table, or a grid, declared writable (`read_only: false`) is refused.
  A best table may be mutable (2026-10-18): the display relations are engine data that
  contributors insert into and that a pick must always answer (R2). No case is known for a
  mutable table of the other picks, and lifting the refusal later breaks no file.
- a domain on a column that is not a key, or on a key of a table that is not best, is refused,
  and a best table's key that declares none is refused (see Key domains).

**Key domains (owner, 2026-10-18).** Every key column of a best table says where its domain
comes from; nothing is inferred. The builder takes `add_column::domain` (`key_domain`), YAML
the column's `domain:`:

- `members` (`domain: {members: [idle, walk]}`): a literal list, on a hash key (texts, hashed
  as a CSV cell is) or an int key (integers). A duplicate, an empty list, or members on any
  other key type are refused.
- a foreign column (`domain: {table: display, column: display}`): the distinct values that
  column holds in the other table's current rows, read-only or mutable. A missing table or
  column, or a column of another type than the key, is refused. Tables whose domains come from
  other tables form a DAG; a cycle, a table naming itself included, is refused naming it. A
  foreign column that is the other table's message key gives its types without its 0: the
  blank is no message, not a type the dependent key could name (and a hash 0 is refused in its
  CSV), so the dependent is total over the real types alone.
- `rows` (`domain: rows`): the key's own table's rows name its values.
- `message` (`domain: message`): the key is the type of the message a step is given (see
  Machines, Messages), on a hash key, at most one per table. It contributes the blank, 0 (no
  message), so every tuple of the other keys demands a blank run; a run keyed on a type is in
  the domain when its blank twin is (so it is checked because it is there), and an orphan when
  its other keys are outside theirs. A blank CSV cell in a message key is that 0, in a rows file
  or a pivoted one; every other blank value cell is still refused. The fall-through is the
  table's: every lookup of its runs, `PickBest`'s as much as a machine step's, retries a type
  with no run once with 0 in the message key, and only a miss of both finds no run.
- A machine clause's output is the fifth kind. It is not declared on the table: a machine's
  `Prepare` closes a clause's key by its source (see Machines), and the table keeps one of the
  four kinds above for its own loads.

A key's domain is a set of key tuples. The key columns declared `rows` together contribute
the distinct joint sub-tuples the table's rows hold in them, never a product of each column's
values, which would close one column's domain from another's values; every other key column
contributes its declared domain (its members, or the foreign column's values). The domain is
the cross product of the `rows` sub-tuples with the other columns' domains. So a key all of
whose columns are `rows` is exactly the runs present: no tuple is missing and no run is an
orphan. With `a` over its rows and `b` with members 1 and 2, every `a` the rows name demands a
run for (a, 1) and (a, 2). A run whose key tuple is outside the domain is an **orphan**: it is
not checked, never refused, and reported, so a retract needs no refuse or cascade rule.
When a table changes, every best table whose domain comes from it is checked again against
the domain the change would leave, and a change that would leave one of them not total (or
with a dead row or an overlap in a run that just entered the domain) is refused, naming the
dependent table and key and the change that found it; a bare retract is not, and reports
instead. So a display is added by inserting its default owner row first (an orphan, allowed),
then the display row. A change that brings rows proves the whole version, including the
tables whose domain it feeds, so a dependent left not total by a bare retract refuses every
change to its domain's source that brings rows, until it is made total again; the refusal
says the dependent is not total, since its gap may be older than the change. A mutable table
some best table's domain comes from is versioned like a mutable best table, so a refused
insert into it is a version never published, as every refused change is.

**The layout is the proof (2026-10-18).** The checks read one layout of each key run: its rows
grouped by flag set, each group's band inputs cut at every band edge of the group and at the
ranges' ends, and per cell the group's rows whose bands can hold there, in try order (owner,
then index), ending at the first row that is not **conditional**. A row is conditional when a
pick may fall past it where its bands hold: it has a script condition, or an edge (see Edges).
That is one predicate of the layout, so an edge and a script are one kind of row to the proof.
Totality is "every list of the default group (flags 0) ends in a row that is not
conditional"; a dead row is "a row in no list of its group". The
lists are flat arrays and offsets, the form a machine's `Prepare` keeps as its compiled pick.
One layout holds any number of runs laid one after another, each a span of groups, and names
rows by their version row numbers; `Load` lays one run at a time into a layout it reuses, and
`Prepare` keeps every run of a clause's domain in one.

**The report.** `load_rows`, `insert_row`, `retract_owner` and `csv_rows` take an optional
`std::vector<run_note>* report`. It is cleared, and if the change is accepted filled with
every orphan run of each table the change touched and of their dependents, and after a bare
retract (or a load of no rows) every in-domain run of those tables that is not total too. It
is the state of those tables, not a delta: an orphan the change did not make is listed again.
Notes are ordered by table index, then key, whichever change made them. A refused change
leaves it empty: the refusal says why. One list of one note type (table, status, key) is all
a caller needs to find what to fix or retract; there is no callback.

**Machines.** A machine is one query, prepared
once. `create_machine` (`tabular/machine.h`) is a command struct like `create_table`; YAML
writes it under `machines:` beside `tables:`, read by `DocumentFromYaml` (`SchemaFromYaml`
refuses a document with machines rather than drop them). It holds:

- a **state vector**: named columns, each a `hash` with members and an initial member, or an
  `int` or `double` with an initial value;
- the machine's **inputs**, named and typed like a table's (a hash or an int too, since an
  input may feed a key);
- four ordered **clause lists**, `create`, `entry`, `step` and `exit`. A clause reads one
  picked table that answers one value per column (a best, an or or a weighted table): `from` the table, `where` each key column = a source, `given` each table input = a
  source, then `set` state columns from the picked row's value columns and/or `emit` a command
  column (the suffix included). Transition and entry tables are clauses, not table kinds.

A **source** is a state column (`state.mode`), a machine input (`input.energy`), a value column
of the row an earlier clause of the same list picked (`clause.0.mood`), or a literal
(`{literal: scared}`, read as a cell of the target's type; a literal is a scalar). A literal
given to a ranged double input must be finite and inside the range (R12: a pick's clamp has no
place for NaN, and a constant outside the range is a mistake, not a value to clamp). A clause
may name only earlier
clauses, so a list is a DAG by how it is written and there is no cycle check; a clause naming
itself or a later one is refused. Two clauses setting one state column are ordered by
position, and the later write wins (R4); one clause setting a column twice is refused.

The definition build (`CheckMachine`, which `DocumentFromYaml` runs on every machine and
`Prepare` runs first) refuses: an unknown table, column, input or state column; a clause on a
table with no pick, or on an all table (an all pick answers every row that holds, not one value
per column); a key or table input left unbound, or
bound twice; a `where` naming a column that is not a key; a source whose type is not its
target's; a `set` from a column that is not a value, or of another type than its state column;
an `emit` on a table with no command column, or naming one that is not a command; a literal
outside its ranged input's range, or not finite; on an or table, a `set` from, or a later
clause reading, any column but its output, and any `emit` (several rows may hold); an `entry`
clause setting a hash state column (entry runs once per step, after the step list, so a state
an entry write moved to would get no entry of its own and its R6 state function would never
start: a transition belongs in the step list; int and double writes are data and stay
allowed), and an `exit` clause setting one (exit runs once the step list has chosen a state,
keyed on the one being left, so its write would overrule that choice); a message whose type is
not a declared hash input, an argument that is not a declared input or is the type or repeated,
or arguments with no type; a key that reads the message type on a key the table does not
declare `domain: message`, a message key bound to anything but the message type, and a key that
is not a message key reading an earlier clause's message-key column (`clause.N.<column>`: that
clause can answer from its blank run with 0, no message, which only a message key holds); a hash
state with no members, a duplicate member, or an initial value that is not a member. Both the
YAML front end and the definition build name a clause the same way: `machine 'grazer' step
clause 0 (table 'grazer_brain')`. In YAML, members
and literals are hashed as CSV cells are, so within one machine a name hashing to 0 or two names
with one hash are refused.

**`Prepare(create_machine, store, report)` proves by laying out.** It returns an immutable,
reference-counted `Machine` holding the version of every table its clauses read. For each
clause it reads the **clause's key domain** from the sources. A source that closes its key
gives the values it can hold:

- a hash state column, the column's members;
- an earlier best or weighted clause, the distinct values of the column read over the rows the
  earlier clause can answer from (the rows a best clause's layout reaches, every row of a
  weighted clause's in-domain runs);
- a literal, its one value.

Every other source closes nothing, and **one rule covers them all**: the key keeps the table's
own declared domain for that column (members, a foreign column's values, or `rows`). That is a
machine input, an `int` or `double` state column (it holds any value), and an or clause's
output (the OR of any subset of its rows' flags may come out, a set exponential in the rows, so
it is not enumerated). An or or a weighted table declares no key domains, so such a key is over
the table's rows. Keys declared `rows` then contribute the sub-tuples the rows name, crossed
with the other keys, as in a load. **An input-bound key is proved only over the table's
declared domain**: an input value with no run at run time holds the clause in `Step` (see
Holds), not something `Prepare` can rule out.

What the domain must satisfy depends on the table's pick:

- **best**: the one check a load makes (totality of the default group over the table's band
  inputs, no dead row, no two owners overlapping), with the domain as its parameter: `Load`
  passes the table's declared domain, `Prepare` the clause's. A tuple with no run is a run of no
  rows, refused with the load's message, ending "and the key is in the clause's domain"; a run
  `Load` never proved (outside the table's domain, inside the clause's) is refused naming its
  first row. The layout of every run is kept in one set of flat arrays, with a directory from
  each key tuple to its run's groups and lists naming version row numbers, plus, per clause,
  each layout axis's table input and the ranking column's input; the proof-only arrays are
  dropped.
- **or**: an or pick always answers one value, the OR of its holding rows' flags, 0 when none
  hold, so nothing is proved beyond the domain resolving. A run directory (each tuple's version
  rows) is kept; a tuple with no run is an empty run, answering 0.
- **weighted**: every tuple must have a run, refused as "has no row to draw; no row has this
  key, and the key is in the clause's domain" (a load already refuses non-positive weights and
  non-finite totals). The run directory is kept for the draw, seeded per instance and step.

Every refusal names the machine, the clause list and the clause. Every hash a `set` can write
(a value in a row the clause can answer from) must be a member of its state column, checked
here because rows load after the schema. `report`, if given, is cleared and, if the machine is
accepted, holds every run outside a clause's domain as an orphan, as a load's report does,
clause by clause in list order (two clauses on one table may note one run twice); a refused
`Prepare` leaves it empty.

A `Machine` records the generation of every table it read, taken from each version as the
proof takes it: those its clauses pick from, and those a clause's key domain was read from (a
foreign column behind an input key), since a
dependent's generation does not move when its domain changes. `Machine::Stale(store)` says
whether any of them has moved, so the machine must be prepared again.

A clause's table inputs are open over their declared ranges even when the source is a literal
in range.

**`Bind(machine, host_binding&&)`** (`tabular/instance.h`) makes a `Binding`: the machine
plus one `input_getter` per machine input, shared by every instance of an object type and
held by `counted_ptr`. `host_binding::resolve(name)` is called once per input, in declaration
order, and is the only place a name meets the host: there is no symbol table and no per-step
lookup. A getter is `{read, data, release}`, like a closure: `read(data, self, error_out)`
returns the input as a cell (`i` for a hash, flag set or int, `d` for a double), and `self` is
the host object the instance belongs to, which the host passes to every `CreateInstance` and
`Step` call, so a getter reads that object's own state with one indirect call and no lookup by
id, and the instance keeps no pointer that a moved host object would leave dangling. The same
`self` is the `this_in` a row's script condition is tested against. `release` runs once, when
the binding is dropped. `Bind` refuses, naming the machine and the input, when `resolve` is
missing (a machine with no inputs needs none), refuses an input, or returns a getter with no
`read`; it adopts every getter `resolve` returns, so a refusal releases those already
returned. Message inputs (below) are filled by `Step`, not by a getter, and are never
resolved. Emitted commands are not handed to the host but returned (P12; built 2026-10-19,
commands-and-messages spec Part 2): `Step` and `Rebind` return a `std::span<emitted const>`,
and `instance::Emitted()` is the same span for the last run, which is how creation's commands
are read. An `emitted` is `{list, table, command}`: the list the clause is in (so a host tells
an exit's abort from an entry's start), its table, and the row's command, a view into the
version the binding that ran the clause holds. The span is valid until the instance next runs,
is moved or destroyed; a rebind's exit commands view the old binding's versions, so the
instance keeps the old binding until its next run. The buffer behind it holds one entry per
clause that can emit, across every list, sized when the instance is created (and again by a
rebind), so a step that emits allocates nothing and makes no indirect call per command.
`host_binding::report(instance, table, row, error)` takes a failure once (P15); it is optional,
and without it holds still hold, silently. A binding keeps its `Machine`, and so the versions it was proved against: a store change never
reaches a running binding. The host asks `binding.machine().Stale(store)` (an instance gives
its `binding()`), and a stale machine is prepared again, bound, and its instances moved over with
`Rebind` (see Reload).

**An `instance`** (snake_case: composited into the host's object, never `new`ed; move-only)
holds its binding, its id, the current state vector, the next one, each input's previous value (see Edges), and what each clause last
answered (`Answer(list, clause)`: `Answered` with the row picked, or for an or clause its OR;
`NoRun`, `Failed` or `SourceHeld` when it held; `NotRun` when its list did not run). Every
buffer is sized at creation or rebind, so **a step allocates nothing**, holds included: a step builds
no error of its own and adds no context to a getter's or a condition's failure, except at the
first occurrence of a failure the instance reports (below), whose report carries the host's
error from the step's one asking. The interpreted picks, and the report, are the callers that
name the table and row. Tests count the global `operator new` calls of steps that answer,
raise in a script condition, see NaN, lose a getter, find no run, run an or → best → weighted
chain, emit, take a message (absorbed or falling through), run exit, cross edges, and repeat a
reported failure (a raise, a NaN, a key with no run and a lost getter, each with `report`
set): zero, after a first round in which each report builds its error once.
A host's own failure message longer than the small-string size allocates on the host's side,
when the host writes it; Tabular adds nothing. `CreateInstance(binding, id, seed, self)`
returns the instance (it cannot be refused: a clause that cannot answer holds, as in a step),
with every state column at its initial value, after running the `create` list and the
`entry` list as a step runs a list (creation enters the initial state).

**`Step(instance, self, seed, message)`** runs the prepared query:

1. Every input is read once, so every clause of the step sees one value of it. A getter that
   fills `error_out` leaves its input without a value this step. The message's type and
   arguments are inputs too, read from the `message` passed (default: none). An edge crosses
   from its input's previous value (see Edges); the values read become the previous values at
   the end of the step.
2. Next is set to a copy of current. **A column no clause writes holds** because of this copy,
   and a clause that holds writes nothing; neither needs a path of its own.
3. The `step` clauses run in order. Each reads its sources: a state column from **next**, as
   the step has written it so far; an earlier clause's answer from this step; an input from
   the step's read; a literal. So a chain settles in one step from clause order and in-step
   reads of next, with no settling loop. A clause then finds its run by binary search over its
   sorted key tuples and picks: a **best** clause clamps each band input as a pick does (the
   shared clamp), ranks the run's flag groups against the input flags as `PickBest` ranks rows
   (full match, then `R & F`, then `R`; a group matching nothing skipped, the `R == 0` group
   last; an unranked table has only that group), finds the input's cell in each group tried by
   binary search over its cuts, and walks the cell's list testing each row's edges and then
   its script condition until one holds; an **or** clause ORs the output of its run's holding rows (0 for an empty run); a
   **weighted** clause draws `Draw(seed, site)` over its run. Script conditions, bands, edges, the
   clamp, the input checks, the flag rank, the OR and the draw are the interpreted picks' own
   code, so the
   compiled step and its oracle cannot drift apart there. The clause's `set`s then write next;
   within a list a later write of a column wins (R4).
4. **Holds.** A clause writes nothing this step, and its dependents that read it hold too,
   when a source has no value (`SourceHeld`: an input whose getter failed, an earlier clause
   that held), when an input is one a pick refuses (`Failed`: NaN for a ranged input (R12), a
   flag set wider than 32 bits), when its key tuple has no run (`NoRun`: an input-bound key's
   value outside the domain `Prepare` proved), or when a script condition cannot answer
   (`Failed`). Inputs are checked before the run is found, as a pick checks its query first.
   A clause keyed on a message type with no run of its own retries once with the blank, 0
   (the fall-through); only a miss of both is `NoRun`.
5. **Entry.** "Entered" means a hash state column of next differs from current at the end of
   the step list, so A → B → A within one step enters nothing. Then the `entry` list runs once
   against next, its writes landing on top of the step list's (so across lists, step writes,
   then entry writes). An entry clause may not set a hash state column (the definition build
   refuses it), so entry never enters again, and a change of state is always a step row, written
   where it can be read (owner, 2026-10-19). Int and double state columns are data the machine
   keeps, not states: a change to one enters nothing, and entry may write them.
   **Exit** (R6) runs on the same test, before entry: its clauses read state from **current**,
   the state being left, and write next, so where exit and entry write one column entry's
   write lands last. When nothing was entered both lists answer `NotRun`. Creation runs no
   exit: nothing is left.
6. Next becomes current (the two vectors are swapped).
7. **Emits.** Every clause of the step, exit and entry lists (in that order, each in clause
   order) that answered and has an `emit` adds its row's command to the step's results, a view
   into the version the binding holds; the step returns them, with the step's state already
   current. A held clause emits nothing. Creation's results are the create list's, then
   entry's.

**Reports (P15).** A failure reaches `report` once per instance: a getter failure once per
input (`table` is `kNoTable`, `row` `kNoRow`, the error the getter's, naming the machine and
input); a message carrying more arguments than the machine declares once (`kNoTable`,
`kNoRow`, naming the machine and both counts); a bad table input (NaN, an over-wide flag set)
once per (clause, input); a key with no run once per clause, naming the key as it came (a
message type, not the blank it retried); a script condition that cannot answer once per
(clause, row), `row` being the failing row. A hold caused by another hold (`SourceHeld`) is not
reported: its cause was. The error names the clause as the definition build does. A condition
is asked **once** per evaluation: the step passes its own `ErrorContext` down to the row's
test, and the report carries the host's error from that one asking, with the table, row and
clause added only when it is the first report. The memory is a bit set in the instance, sized
at creation: one bit per machine input, one for extra arguments, and per clause one for no
run, one per table input and one per row that has a script condition (`Bind` ranks those rows
per clause; only they can fail, so a large table of unscripted rows costs nothing, and a
weighted table, which has no conditions, no row bits). Only a first occurrence builds an
error, so a repeat allocates nothing.

**Messages (R3; under review, see Undecided).** A machine takes one message per step: `message: {type: <hash input>,
arguments: [<input>, ...]}` in YAML, `create_machine::message` in the builder. The type and
the arguments are machine inputs filled from `Step`'s `message{type, arguments}`, not from
getters. Arguments are positional slots every message type shares; a type's own arity is not
declared. An argument not passed reads 0, the value every argument has on a step with no
message, and is no failure. More arguments than the machine declares still step (a step cannot
be refused), the extra ones unread, and the instance reports it once. 0 is no message. A
table keys on it through a `domain: message` key, and a clause must bind that key to the type,
both ways. A type with no run falls through to the blank run, as in `PickBest`. `instance::Absorbed()` says the
last step's message was answered by a run keyed on its type, so the host can route a message
nothing absorbed to script; a fall-through, or a clause that held, absorbs nothing. Passing a
message allocates nothing: the arguments are a span the host owns for the call.

**Reload (P14).** `Rebind(instance, binding, self, seed)` moves an instance onto a new binding. It
is a step that enters a state with migration where the step list would be, and it uses the step's
own pieces (the input read, the exit and entry lists, the results), so a reload has no list or pick
path of its own:

1. **Leaving, under the old binding.** Every input is read once with the old getters (no message),
   next is set to a copy of current, the old `exit` list runs against current, and next becomes
   current. What exit emitted comes first in the results, so the host hears the abort of what a
   state started (R6) from the code that started it, before the new code starts it again.
2. **Migration by name**, from the state exit left (an exit's int or double write is kept like a
   step's). Each state column of the new machine takes the old column of the same name. The value
   is kept when the type is the same and, for a hash, the value is still a member; otherwise the
   column takes its new initial value. An int that became a double, or the reverse, is reset:
   converting it would be arithmetic the machine never wrote. A column no longer declared is
   dropped. Every buffer is sized for the new binding (a step after a rebind allocates nothing,
   tested), the report-once memory is cleared, since a new binding is a new set of clauses and
   rows, and every clause answers `NotRun`.
3. **Entering again, under the new binding.** Every input is read with the new getters, the new
   `entry` list runs against the migrated state, next becomes current, and what entry emitted
   follows exit's in the results. The `create` list does not run: the instance is not new. Afterwards only the new
   entry clauses have answered, and `Absorbed()` is false.

A reload is no movement of an input, so it **crosses no edge and spends none**. The exit and entry
lists see each input's previous value as the value read for them, as creation's lists do.
Afterwards an input the old machine also had, with the same name and type, and which both
machines read from a getter, keeps its previous value, so the next step crosses from the last
value a step read. An input new to the machine, retyped, or a message argument on either side
(its previous value is another message's, not a getter's), or one with no previous value yet,
takes the value read at the rebind, as at creation.
Previous values are raw, so a changed range is applied by the clamp where they are used.

`Rebind` cannot be refused, and returns nothing. Everything a binding reads, it holds (its
`Machine` and the versions that machine was proved against). Migration always lands on a valid
state, and a clause that cannot answer holds as in a step. The binding may be the one the
instance already has, which restarts its state, or one of a machine with a different name.
An exit clause that holds (a condition raising, a getter failing) sends nothing: the host
learns of it through the report, in a step or a reload alike, and the new entry still runs.
Nothing keeps running because of it, since the host aborts a state's function whenever the state
changes, whatever exit emits (see State functions). It allocates (it sizes the new buffers); it is not
a step.

**The reload cycle is the host's composition**, and Tabular has no reload manager. Rows change (a
load, which bumps the generation), `machine().Stale(store)` says so, the host calls `Prepare` again
(with a new `create_machine` when the definition changed too: a definition change alone moves no
generation, and the host that rebuilt it already knows), then `Bind`, then `Rebind` on each
instance, and drops the old binding. A refused `Prepare` changes nothing, so the old binding keeps
running (promise 5). The worked example (spec §4) is tested this way: adding the member `sleep`
keeps `mode`, and deleting `panic` while an instance panics resets `mode` to `idle`; the old exit
aborts the run and the new entry starts `idle`'s animation.

**Bridges are states (R5(b)).** A transition row writes `mode = bridge_c, target = walk`; the
bridge states have rows keyed on `target` and on a message (`clip_done`) that move along when
the clip ends, and the last one moves to the target. Each bridge state's entry emits its clip,
and the machine holds the bridge state while the clip plays. This needs nothing beyond two hash
state columns, a step clause keyed on both and the message, and an entry clause; the machine's
proof covers it (a bridge state with no run for some target is refused by `Prepare`). A bridge
that only the animation system needs stays a suffix command it plays (R5(a)).

A weighted clause's draw site is `DrawSite(machine name, list, clause)`: the machine name's
CityHash32 in the high word, then the list and the clause's position. Two weighted clauses in
one step, or two machines on one object stepped with one seed, draw different numbers, and a
reload that keeps the names and places keeps the sites; the host varies the seed per instance
and step. The picked row of every clause stays readable through `Answer`, which is where an
`emit` clause's command comes from.

**Parity: the interpreted picks are the oracle** for every list a machine runs.
Randomized tests compare every clause's answer, and the state it leaves, with `PickBest`, `PickOr`
and `PickWeighted` on the version the binding holds, through cuts, the range's ends and beyond,
NaN, over-wide flags and scripts that fail:

- the step list: the spec grazer (keys from a state and a message input, a message type that
  falls through, a ranked flags input, scripted rows), the sketch's grazer (two bands, a
  script), an or → best → weighted chain, each term of the flag rank made the deciding one, and
  signed two-key int tuples where key 0 ties and key 1 decides (`StepParity`);
- edges: random walks over one and two inputs, a ranked edge row that does not hold, and a
  pivoted or table with an edge column (`EdgeParity`);
- the other lists, on a machine whose lists hold every clause kind (`ListParity`): creation's
  create (a weighted draw) and entry lists; a step's exit, reading the state left, with a band,
  a script and an emit, and its entry chain (or → best keyed by its answer → weighted); and
  `Rebind`'s old exit, against the old version and tagged with the old binding, and new entry, against the
  new versions on the migrated state, over hundreds of reloads that change the rows both read.

What parity tests is `Step`'s own code: the tuple search, the laid-out groups, cells and lists,
the list order and what each list reads (current or next, earlier answers, the migrated state).
It cannot test the helpers both sides share, since both run the same code: the clamp, the input
checks, the condition call, the flag rank and which groups it ranks, the OR, the draw, the
crossing and the message fall-through. Those rest on the pick and machine suites: each was
mutated (14 mutations, 2026-10-19) and each turned at least one test outside the parity suites
red (for example the clamp's top `PickBest.ARangedInputIsClampedAndNanIsRefused`, the draw's
weights `PickWeighted.RowsLandInProportionToTheirWeights`, the full-match term
`PickBest.AFullMatchBeatsALargerPartialOne`, the R term `PickBest.RanksLikeAnimationsLookupBest`,
the OR `PickOr.OverlappingBandsOrTogether`, the crossing
`EdgePick.PickBestHoldsAnEdgeOnlyWhenTheInputCrossedIntoItsBand`, the fall-through
`MachineMessage.PickBestFallsThroughToTheBlankRunToo`).

**Provenance (P16; debug builds).** With `TABULAR_PROVENANCE` set (Tabular's
`CMakeLists.txt` sets it to 1 in the Debug config and 0 in the release family, in a header it
generates, `tabular/build_config.h`, that only `tabular/instance.h` includes; a build that
defines it itself keeps its own, and one with neither has it off), each
instance keeps a ring of its last `kProvenanceDepth` (16) clause runs, and `instance::Provenance()`
returns them oldest first. An entry (`provenance_entry`) names the run it belongs to (0 for
creation, then one more per `Step` or `Rebind`, and which of the three), the list, the clause
and its table, the clause's answer (outcome, row, OR bits, refused input), whether a message
type fell through to the blank run, and up to `kProvenanceValues` (8) values the clause read:
its keys, the message type as read; its table inputs, as read before the clamp; and, for a table
with edges, the previous value each input crossed from. A wider clause keeps the first eight
and says it was cut. A clause held by a source read nothing whole and keeps no values; one
refused on an input keeps its keys and inputs. A list that did not run records nothing. The
ring is sized at creation and kept across `Rebind`, so recording allocates nothing (tested, with
the step's allocation tests also running with it on) and a reload shows the old exit's picks
then the new entry's. Without `TABULAR_PROVENANCE`, the ring and `Provenance()` do not exist:
release instances carry no bytes for them, and no caller can read an empty ring as "no picks".
Every unit must be compiled with one setting; when it is on, the instance type carries an ABI
tag (and on MSVC each object records the setting through `#pragma detect_mismatch`), so a unit
compiled the other way fails to link rather than reading another layout.

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
tables); every step; when a relation's rows change (the machine is stale, prepared again, bound,
and its instances rebound, which runs the old exit and the new entry: see Reload).

**Lifetimes.**

| Object | One per | Holds |
|---|---|---|
| Definition | table section of a bundle | tables, machines (`create_machine`), snippet text; immutable |
| Machine | definition × the table versions it was proved against | the proof's layout (the compiled pick) and those versions; immutable, `Stale` when a table it read changed |
| Binding | machine × host object type | the machine, one resolved input getter per input, the report |
| Instance | object | state vector, each input's previous value (the edges' history), the last run's results |
| Relation | global relation | live rows, each owned by a contributor; RAII handles retract them (`owner_handle`) |

**Owner handles (P1).** An `owner_handle` (`tabular/owner_handle.h`) holds a store
and an owner id; dropping it, or calling `Release`, retracts every row the owner holds, and a
moved-from handle retracts nothing. It retracts from every table, read-only tables and grids
included, because "RAII handles retract them" says nothing of how a row arrived and the handle
is for crash recovery: loaded rows that outlive their contributor have no one left who would
retract them. A reload does not use a handle; it loads again. Retract is never refused, so a
destructor always finishes.

**Generation (P2).** Each table has `relation_store::Generation(table)`, a counter
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

**Cyclic inputs (owner, 2026-10-19).** A ranged double input may declare `cyclic: true`
(`add_input::cyclic`): its range is a period, as an animation phase's `0..1` is. A pick wraps
the input into `[lo, hi)` instead of clamping it, so 1.52 on `0..1` is 0.52 and 7 is 0, and a
value that is not finite is refused, since a period has no place for an infinity either. A band
on a cyclic input does not wrap: one that should run across the period's end is two rows.
`cyclic` on an input with no range is refused, and so is `cyclic` on a machine input: the table
input whose bands read it declares it, and the machine's would be accepted and ignored.

**A band is well formed (2026-10-18).** A band cell has both ends finite and start before end,
in every table whatever its input's range, checked where the cell enters a table. The range
check then only tests containment.

**Edges (P13).** A band column marked `edge` (`edge: true` in YAML,
`add_column::edge` in the builder) holds when its input **crossed into** the band between its
previous value and this one, not when the input is in it. Both values are clamped as a band
input is. With the band `[lo, hi)`, previous `p` and current `x`, the edge holds when `p < lo`
and `x >= lo`, or when `p >= hi` and `x < hi`: `p` lay outside, and `x` is at or past the end
`p` came from, inside the band or through it and out the other side, so **a short band is
never skipped by a fast input**. So landing exactly on `lo` from below enters, reaching `hi`
from below passed through, reaching `hi` from above never entered (hi is not in the band), a
previous value on `hi` is outside, and leaving from inside, or staying inside, crosses nothing.
NaN crosses nothing. On a linear input crossing is along the line, so an input that wraps (0.95 to
0.05) passes through every band between the two values and fires the downward edges there. On a
**cyclic input** the move goes the shorter way around the period, and exactly half a period is
taken upward. It crossed when, on the way, it reached the end of the band it travelled toward (`lo`
going up, `hi` going down): a walk cycle's footstep band `0.5..0.55` fires as the phase passes 0.5
either way, not when the phase wraps from 0.95 to 0.05, and a band at the top of the period is
entered downward across the wrap (0.05 to 0.95 enters `0.9..1`). A move that leaves a band and
comes back into it the short way round has entered it. One function (`detail::Crossed`) is the
test, and the interpreted picks and a machine's step both call it through one row test
(`detail::EdgesHold`).

- **Declaration.** `edge` on anything but a band is refused. A band reads an input only in a
  picked table, so best, all and or tables carry edges and a weighted table (no inputs) or a
  table with no pick cannot; no further rule is needed. The band's input range and the
  well-formedness rules are a band's. A cell holding `NoEdge()` (both ends NaN; a blank CSV
  cell) is no edge: the row counts whatever its input did. No other band takes it.
- **Proof.** An edge row is conditional (see The layout is the proof): it closes no list, so
  totality needs a row after it with no script and no edge, and it covers no later row. Load
  and `Prepare` share the check, so both get this. A totality refusal on a table with edges says
  "no row with no script condition and no edge". On its axis an edge's box is where a crossing
  can end, not the band: a crossing up ends in `[lo, H)`, possible when some clamped value lies
  below `lo`; one down ends in `[L, hi)`, possible when some lies at `hi` or above (the range
  is `[L, H)`). The box is the union over the possible directions, so an edge band covering
  the whole range holds for no input and is refused as never picked, and a plain row covering
  only a short edge band does not shadow it. On a cyclic input both directions are possible from
  any value outside the band, and a crossing ends within half a period of the end it reached, so
  the box is the whole range unless the band is. The overlap between owners reads the same boxes.
- **Picks.** `pick_query::previous` is parallel to `inputs`. A pick of a table with an edge
  refuses a query whose `previous` is not as long as the inputs (reading no previous values as
  "the input sat still" would answer silently wrong) and checks the previous value of each input an edge
  reads as it checks inputs (NaN for a ranged input, an over-wide flag set), naming "the previous value"; the
  previous values of inputs no edge reads are not looked at.
  For a table with no edge, `previous` is ignored.
- **Machine state.** The instance keeps one previous value per machine input. At creation the
  values read are their own previous values, so the create and entry lists cross nothing and
  **the first step sees a crossing only if the input moved since creation**. Each step's read
  becomes the next step's previous value **whatever the clauses did with it**, so a crossing on
  a step whose clause held is spent (inputs are read once per step, R3's snapshot). A getter
  that failed, or a NaN, is no value read: the previous value stays the last real one, so the
  crossing is measured from it on the next step and is not skipped. Until one value has been
  read the previous value is NaN, which crosses nothing. An edge reads **only a machine input
  with a getter**: the definition build refuses a literal (it never moves), a state column or
  an earlier clause's answer (values of the step's own making, with no previous value kept),
  and a message argument (one message's value, 0 between messages, so its "previous" would be
  another message's). The buffers are sized at creation or rebind, so a step with edges allocates
  nothing (tested). A rebind crosses no edge and spends none (see Reload).

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
- **Input resolution** for every name a table reads that Tabular does not own (built: a
  machine's inputs, through `host_binding::resolve` once per input at `Bind`; see Machines).
- **Running the commands** each `Step`, `CreateInstance` and `Rebind` returns (Tabular never
  interprets one) and, optionally, **a report** for failures (`host_binding::report`); see
  Machines.
- **The message** a step is given, if any (`Step`'s `message`).
- **A seed** per instance and step (the `seed` of `CreateInstance` and `Step`).
- **Running state functions:** start a named function for an instance on the command an entry
  clause emits, abort it on **every change of the state** (owner, 2026-10-19: it reads the
  instance's state after `Step` and `Rebind`, and does not depend on an exit clause answering),
  and send the message that says it returned (see State functions).

Closures are `{function, data, release}`. A condition's `release` is called when no version
holds its row; an input getter's (the same shape) when its binding is dropped.

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

Neither is a table kind (R6). The state-function table is an `entry` table whose command starts
the function (`(start graze_fn)`). The host aborts a state's function on every change of the
state, so an exit command is not what stops it; an `exit` table's command (`(abort graze_fn)`)
is the state's own cleanup, and an abort of one that already returned is the host's no-op. "It returned" is a message (R3) a step row keys on;
until it arrives the state's blank run holds the state. The function-chain table is bridge states
(R5(b), see Reload) whose entry commands start functions and whose rows move along on the return
message instead of `clip_done`. Tests drive both with nothing but entry, exit and step clauses.

A state function **may suspend** (`wait`, `lock`): waiting is often its point. Halting is
required only of code that runs inside a step (conditions, wires) and of commutative writes. In
exchange it **reads and writes only its own entity**, which the host checks as an asserted
footprint on the function. Anything it needs from the world comes through wires, read fresh, so
it never acts on a view that went stale while it waited.

**Reload.** The functions belong to the host's script and reload with it. When a binding is
dropped or rebuilt, a running state function is aborted and its state is entered again, so it
restarts with the current code. Whatever it had already done stays done. A function for which
that matters must be safe to restart, or its state's entry rows must decide what happens.
Built as a consequence of `Rebind`: it runs the old binding's exit list, whose command aborts the
function the old code started, and then the new binding's entry list, whose command starts the
function as the new code names it; both come back from the one `Rebind` call, exit's first.

## Budgets

Each cost has a number and a benchmark that fails when the number is broken (P17). The numbers are measured; each budget is the measurement with a margin, so the
gate catches a regression. Whether a number is small enough for the runtime budget is a separate
judgement: see the last paragraph of this section. That budget (~400 MHz, ~150 MB) is the
vertical slice's: 2 metarooms of the full game's 20, counting what scales with the world (tables,
machines) and fixed costs alike. It is a scaling test, not a target machine: a slice over it cannot
grow 10× into the game (owner, 2026-10-19).

| Cost | Paid | Budget |
|---|---|---|
| build a definition from text | per section, on load and reload | 1.8 µs per CSV row loaded (YAML, CSV and `Prepare`); 1200 µs for the spec grazer whole; 260,000 bytes for the 9,000-row brain's prepared machine |
| bind | per definition × object type (shared by every instance) | 150 µs and 47,000 bytes for the 30-clause brain; 1.8 µs and 1,300 bytes for the grazer |
| create an instance | per object | 0.9 µs; 1,400 bytes (brain), 820 bytes (grazer), release build |
| step | per instance per tick | 250 ns per row evaluated; 550 ns per grazer step |
| relation insert, retract, query | per change or query | plain mutable table of 10,000 rows: insert 25 µs, retract (an owner's 10 rows) 420 µs, query 280 ns; mutable best table of 200 rows: insert 110 µs, retract (an owner's 50 rows) 45 µs, pick 380 ns |

**The gate** is `scripts/test-tabular-budgets.sh` in the superproject. It builds Tabular and
`Spehleon/Tabular/bench/` standalone at `-O2 -mavx2 -mfma` with asserts live (as shipped) and
`TABULAR_PROVENANCE=0`, since the engine's `Build/` is Debug (`-O0`); runs one benchmark per row
below; and exits 1 when a measurement is over its budget, measured nothing, or the benchmark
table below names other benchmarks than the gate runs or another budget than it holds (`--scale
S`, 0 < S ≤ 1, tightens every budget to show it failing). The budgets are absolute times taken
on the one machine below, so the gate is a regression check for that machine, not a portable
one. Run it on a quiet machine: it is not part of `test-e2e-all.sh` or a CTest shard, since a
timing taken beside other work is noise. The Debug suite runs the same benchmarks small
(`TabularBudget`), only to keep them building and measuring, and holds the benchmark table to
`bench/budgets.cpp` both ways.

**How measured** (2026-10-19): AMD Ryzen 5 1600 (6 cores, 3.2 GHz base), Ubuntu 24.04, glibc
2.39, g++ 13.3, the machine otherwise idle. A time is the fastest of 7 repeats (noise only adds
time), each repeat many operations; three to five runs of the gate gave the range shown. A timing holds
only the operation it names: a build repeat frees the previous repeat's workload before its
clock starts, and an insert repeat's rows are retracted (to put the table back for the next) on
a clock of their own, which is the retract row. Memory is
glibc's heap in use (`mallinfo2`) before and after making many objects, malloc's overhead
included, plus `sizeof(instance)` (328 bytes, 40 of them the last run's results and the binding a rebind left) for an instance; Windows' heap differs only in its
overhead. **Margin:** times 2×, except the grazer's whole build, which varies most (0.2 ms of
allocation-bound parsing; its runs here have reached 536 µs), held to about 2.2× that slowest
run; bytes 1.25×; rounded up.

**Workloads.** The *grazer* is the spec's (§4, as the step tests run it): one step clause over a
21-row best table with a message key that falls through, a ranked flags input, a band and two
script conditions, and an entry clause that emits. The *brain* is synthetic and larger: a hash
state of 30 members and 30 chained step clauses, each over its own 300-row best table keyed on
the state and writing it, half of them with a ranked flags input of two groups. No row has a
script and every input point lies in one row of its cell, so each clause tries exactly one row:
a *row evaluated* is a clause run, and a scripted row adds its host condition's own cost. Steps
run 1,000 instances × 50 ticks, inputs varying per instance and tick from a table.

| Benchmark | Measured | Budget |
|---|---|---|
| build a definition: grazer | 194 to 258 us | 1200 us |
| build a definition: per row | 0.85 to 0.89 us | 1.8 us |
| build a definition: brain machine | 204,200 bytes | 260000 bytes |
| bind: grazer | 0.89 to 0.91 us | 1.8 us |
| bind: brain | 71 to 73 us | 150 us |
| bind: grazer binding | 996 bytes | 1300 bytes |
| bind: brain binding | 37,100 bytes | 47000 bytes |
| create an instance: grazer | 0.43 to 0.45 us | 0.9 us |
| create an instance: brain | 0.40 to 0.42 us | 0.9 us |
| create an instance: grazer instance | 728 bytes | 820 bytes |
| create an instance: brain instance | 1,140 bytes | 1400 bytes |
| step: per row evaluated | 124 to 126 ns | 250 ns |
| step: grazer | 251 to 275 ns | 550 ns |
| relation insert: mutable | 11.3 to 12.3 us | 25 us |
| relation retract: mutable | 202 to 206 us | 420 us |
| relation query: mutable | 125 to 140 ns | 280 ns |
| relation insert: best | 49.9 to 50.4 us | 110 us |
| relation retract: best | 21.8 to 22.1 us | 45 us |
| relation query: best | 184 to 189 ns | 380 ns |

The relation rows are the first version's shapes: a plain mutable table like `spawn_points`
(100 owners' rows over 500 int keys, one `FindOrdered` per query) and a mutable best table like
the display owners (50 displays × 4 owners' ranked rows, one `PickBest` per query). Each costs
in proportion to the table, not the change: an insert into an ordered plain table moves the rows
after it, a retract rebuilds the key indexes (a retract is a reload, not a per-tick operation:
nothing changes a relation every frame, owner 2026-10-19),
and a best table's change builds and proves a new version.

**What the step costs, and compiling it (spec R8).** 125 ns per row evaluated is about 400 cycles here, so about
1 µs at 400 MHz; a grazer step is about 0.7 µs there. A profile of the gate's steps (`perf`, mostly the
brain's) splits a step's time into the two searches the layout needs, the binary search to the
key tuple (25%) and to the cell (23%); the clause's own code around them (`RunList` with the
pick inlined: source reads, the rank loop, the sets, 33%); and the input checks and clamp (7%),
on a working set of 30 tables. Compiling the step to machine code could shrink only the third
around the searches, not the searches themselves, so it is not the lever these numbers point
at; a cheaper search to the tuple (a state member's index in place of a binary search) would be. That
is the question the owner asks of "compiling" (2026-10-19): a prepared machine holds immutable
versions (RCU) and runs the same queries every step, so can the searches to the tuple and the cell
become a direct index built at `Prepare`, as a `switch` is a jump table? The load is about 2,000
machines in the full game at 1 ms frames, most of them asleep in a state function on most ticks
rather than querying their tables, so no firm target is set; it stays open (Undecided).

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
   unique}`, an optional `read_only`, and for a picked table `inputs` (`{name, type, range, cyclic}`),
   `pick` (none, best, all, or, weighted), a key column's `domain` (see Key domains),
   `output` (an or table's flags column), `weight` (a
   weighted table's double column), `suffix` (the name of a trailing command column, see Suffix)
   and `pivot: {headers, cells}`. A grid is `grid: {rows, columns, cells}` instead of columns. A grid and a picked
   table are read-only when the key is left out (it is read as `true`); `read_only: false` is
   refused on either, except on a best table, which may be mutable. The builder takes `read_only` explicitly. Any diagnostic fails the definition, an unknown key included, because a
   misspelled `unique` silently ignored is a table that is not unique.
3. **CSV front end** (built: `tabular/rows_csv.h`): rows loaded from CSV text into a read-only
   table. The header names the columns in any order, every non-condition column exactly once.
   A hash cell is `CityHash32` of its text; flags are unsigned decimal or `0x` hex below 2^32;
   an int is signed decimal; a double is read whole; a command column's cell, declared or suffix, is read as one list of commands (see Suffix). A cell
   must parse whole, and an empty cell is refused in every column type, except a blank
   condition cell, which is no condition. A condition cell is handed, as text, to a
   function the caller passes to the loader (`csv_rows::condition`, returning
   `tl::expected<closure, ErrorContext>`), which returns the compiled condition; that is the whole of Tabular's part in compiling
   conditions (owner, 2026-10-16: expanding Tabular to own this went badly in the first
   version). The function is needed only when a non-blank condition cell appears.
   **Quoting** (built 2026-10-19, `src/csv_text.h`, one reader for the rows, pivoted and grid
   front ends): RFC 4180, as a spreadsheet exports it. A cell holding a comma, a double quote or
   a line break is quoted whole, each quote inside written twice, so `(message food "eat")` is
   saved as `"(message food ""eat"")"` and read back as written. Spaces outside the quotes are
   trimmed and text inside them is kept exactly; an unquoted cell is trimmed. A `"` inside an
   unquoted cell, text after a closing quote, and a quote never closed are refused (the last
   naming the line it opened on). A row is a comment only when its first unquoted character is
   `#`, so a quoted `"#1"` is a cell. A spreadsheet pads every row to its widest with empty
   cells and writes an empty row as its separators: trailing empty cells are ignored (a
   non-empty cell past the header is refused), and a row of nothing but separators is a blank
   line. Spehleon/lib's `CSV::File` is not used, since it has no quoting and the engine shares
   it. Blank lines and comment rows are skipped; a
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
4. **Machines** built from tables (definition, YAML `machines:`, `Prepare`, `Bind`,
   instances and `Step` built: `tabular/machine.h`, `tabular/instance.h`, `DocumentFromYaml`;
   see Machines).

**Two kinds of table (owner, 2026-10-15).**

- **Read-only tables** are loaded from files (CSV) and never updated. A reload builds a whole
  new version of the table, checks all of it, and only then replaces the old one. The old
  version is never modified, so a reader holding it keeps a consistent view until it lets go.
  Machines are made from these.
- **Mutable tables** are engine data that script expressions and s-expressions add to:
  who owns the main screen's state, programmable gambits, spawn tables. They are never loaded
  from CSV. No case is known for a table that is both loaded from CSV and mutated, so each
  kind refuses the other's writes. A mutable best table is held as versions too, so a pick
  holds one while contributors insert: an insert is the owner's rows so far plus the new one,
  merged and checked exactly as a load is (2026-10-18). So is a mutable table some best
  table's key domain comes from, so a refused insert into it is never published rather than
  undone. A retract stays unchecked.

**A machine is a SELECT** over immutable table versions, checked to always produce exactly one
row, then compiled to run fast: the compiled form is `Prepare`'s layout, which `Step` runs.
Whether to compile further, to machine code, is undecided (see Budgets). Immutability is what
makes that check and that compilation hold.

## Undecided

These are open, not missing. Each is decided by editing this file. The spec's Open owner
decisions lists them with the provisional rulings the built code follows, which the owner may
overrule the same way.

- Messages as inputs (owner, 2026-10-19). The built form (R3: a message's type is a key, its
  arguments positional slots every type shares, a missing one reading 0) is under review. The
  owner's direction: a message is a kind of command, and outgoing commands have turned out far
  more heterogeneous than expected, so messages are likely opaque to Tabular, as a bounded repeat
  is. Open: whether and how a table reads a message's arguments (a collision's normal vector, for
  one). The code stays as built until this is decided.
- Turning the step's searches into a direct index (R8; see Budgets): no load target is set.
- Arithmetic on data values (see A row): a more general question than `energy + 50` in a value;
  not built, and the owner is not ready to decide it.
- The stranded-dependent rule (see Key domains): the owner finds blocking every load into a
  source while a dependent has gaps a bad rule, with no better one yet. A candidate: no load may
  add a gap, and gaps a bare retract left are reported but do not block.

## The host's part

These belong to the subsystem that has Tabular (What it is not; The host interface), and are
open there, not here:

- the `.tabular` bundle file's container;
- the halting proof and the read-only check on snippets;
- how a host-compiled snippet sees wires (leaning, owner: free wire names become parameters,
  and the wirer's closure captures and forces them; shape note §3), and the snapshot rule,
  that wires read other entities' committed state as of the start of the step (needed for
  parallel steps; census §8 strain 5);
- query order for unordered results, and fan-out over a query result (R1);
- whether a command's consumer compiles its s-expression at bind, so a step enqueues a
  prebuilt record (R9);
- random across the boundary: `random()` in a table's script snippet using the deterministic
  generator ordinary script uses (likely access masks choosing it in table context; owner:
  "doable"), and how `random(...)` inside a command is drawn (R9);
- a delayed message that arrives after its state ended and was re-entered: the instance's
  entry count captured by `:delay`, and `message_stale` an input flag the host supplies (R3);
- mutable-table writes from s-expressions: the host, running a returned command, turns it into
  `insert_row`;
- script reading a machine's state by name (`instance::State` reads by index);
- a bounded repeat (a head nod 1-4 times) is a command the host runs, such as
  `(anim nod :repeat random(1, 4))`, not rows (owner, 2026-10-19);
- aborting a state's function on every change of the state (see State functions).

The display relations are decided (R2): one relation per question (window, pointer mode,
camera), each a total mutable best table; nothing new in Tabular.
