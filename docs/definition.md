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

## What it promises

1. **Totality.** Every query picks a row. Tables are outer-join oriented: the less specific
   rows never stop matching. Over finite key domains, totality is checked when a definition
   is built. A table whose rows are guarded by opaque conditions must end in an unconditional
   row or an explicit hold.
2. **Decidability.** Rows are finite, there is no recursion, and within one step the tables
   that feed each other form a DAG. Whatever the loader checks about a table, an author can
   also know by reading it.
3. **Isolation.** A step touches only its own instance. It reads inputs only through names the
   host resolves and writes only its own state. Everything else leaves as opaque records
   (commands, messages) handed to a host sink. So steps for different instances may run in
   parallel.
4. **Determinism.** Given the same definition, inputs and seed, a step produces the same
   state and records. Randomness is a counter-based draw from a seed the host supplies, salted
   per draw site, with no stored generator state.
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
| Machines | a state vector, transition tables, entry tables, bridges, edges |

**A row (owner, 2026-10-12).**

    [ keys … ] [ conditions … ] [ weight ] [ values … ] [ suffix … ]

- **Keys:** several allowed. They say which row this is. **A key is equality**, so keys can be
  indexed: finding a row's candidates is a lookup, not a scan.
- **Conditions:** whether this row counts. **Every match that is not equality is a
  condition** (flags subset, band, edge, script). **At most one script condition per row;**
  any number of the others. A row may carry several, of different kinds (for
  example a band, a flag and a script condition). If a row does not count, evaluation falls
  through to the next one.
- **At most one weight** per row, and only in a table with no conditions (see Picking).
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

**Flag matching (owner, 2026-10-12).** For an input flag set `F` and each row's flag set `R`:

1. a row with `R == F` exactly wins;
2. failing that, the row with the largest `R & F`, compared as an integer, wins. Higher bits
   outrank all lower bits together, so **the bit order of a flag set is its priority**;
3. if `R & F` is 0 for every row, the row with `R == 0` wins. **That row must exist:** it is the
   clear default. Every table needs a well-defined answer to "what happens if nothing matches".

Ties (two rows with the same largest `R & F`, neither exact) go to the **first in row order, or
column order if the flags are pivoted**.

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
- **A pivoted flag output ORs together.** The phase table writes every flag whose band holds,
  combined with OR.

**Pivot is presentation of any axis.** The clip table pivots a key (kind) into the column
headers, with values in the cells. The phase table pivots the *output* into the headers: the
flags across the top are the value column, and the cells are bands, i.e. conditions.

**When a table runs (triggers).** At bind or instance creation; on entering a state (entry
tables); every step; when a relation's rows change.

**Lifetimes.**

| Object | One per | Holds |
|---|---|---|
| Definition | table section of a bundle | tables, machines, snippet text; immutable |
| Binding | definition × host object type | compiled closures, resolved input slots, state layout |
| Instance | object | state vector, edge history |
| Relation | global relation | live rows, each owned by a contributor; RAII handles retract them |

**Surface.** A builder API is the core. Parsing a table section is a front end that calls the
builder. Public headers live in `include/`; everything else is private.

## The host interface

The host supplies:

- **compile(snippet, kind, symbols) → closure or error.** Tabular's symbol table answers name
  lookups for the names it owns (state). A name both sides know is an error.
  **compile rejects any snippet it cannot prove halts** (owner, 2026-10-12): loops over finite
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

Closures are `{function, data, release}`. `release` is called when a binding is dropped.

## Entity methods (owner, 2026-10-12; read-world form proposed)

The grazer translation fit mostly in tables, but not as "90% of objects fit". It was "about
10% of *each* object does not fit". So tables have to work alongside script inside one object.
The C3 bootstrap census (`docs/project-notes/2026-10-12-c3-bootstrap-fit.md`, §8) located most of
that 10%: **read from self, write to self** (accumulators, counters that run while a condition
holds) and **read from world, write to private** (a sticky target, several values from one scan,
a scan done only when needed). Neither is a command, and before this section neither had a place.

A bundle can carry a `--- script ---` section defining **entity methods**, such as
`Grazer::my_method()`. They are new methods that are not part of the type's definition as far
as AngelScript is concerned: closures owned by the bundle. Entity methods for different
entities can run in parallel.

Each method must behave in a constrained way, which the main script file does not have to:

- it writes only its own entity's private fields (host-object fields, which may hold handles
  and values; not Tabular state, so the state-from-state rule does not apply, just as it does
  not apply to energy);
- it reads its own entity, wires and committed world state (proposed; footprint reads `World`,
  writes `Instance`);
- **it never suspends** (owner): no `wait`, no `lock`, no state carried on a script stack
  between steps. It runs to completion and must provably halt, like any snippet. Behaviour
  that needs to suspend, or anything else outside these rules, leaves restricted land by a
  message to the main script engine;
- nothing outside the bundle refers to it, so removing it can never require removing anything
  else;
- all of a bundle's methods are replaced together on hot reload.

That is the point. If the bundle defines it, "if we remove this, we should also remove that, by
modus ponens" is not even a question, because a method cannot do the things that create that
confusion.

**A loop that spanned ticks in CAOS** becomes state plus rows plus a method: the body runs once
per step, the exit condition is a row, and where the loop had got to is machine state. If it
genuinely needs `wait`, it is a coroutine in the main script engine, reached by a message.
CAOS loops with no `wait` relied on preemption to make progress; without preemption they must
take one of these two forms.

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

## Undecided

These are open, not missing. Each is decided by editing this file.

- The condition column kinds, and how many of each one table may have. Whether a row's
  conditions all have to hold.
- Whether a command's consumer compiles its s-expression at bind time (as the host compiles
  snippets), so a step enqueues a prebuilt record rather than text.
- What a bridge step carries (state or clip, exit, commands on entry, bounded repeat).
- Random across the boundary. The oddity to remove: `random()` called from a script snippet
  inside a table uses a different generator from `random()` in ordinary script. Likely
  resolved host-side, with access masks choosing the deterministic generator in table context
  (owner: "doable"). Open: how `random(...)` inside a command's s-expression is drawn.
- Entity methods: confirm the read-world form. How they are scheduled: as step nodes in the
  DAG on the same triggers as tables (every step, on entry, on a message), rather than being
  called from rows. How the constraints are enforced (likely the engine's existing script
  footprints).
- The snapshot rule: wires and entity methods read other entities' **committed** state as of
  the start of the step (needed for parallel steps; census §8 strain 5).
- Private writes (owner, not fully thought out): if a write is known to go to a field no one
  else can see, it can count as no write for concurrency purposes, since no other reader can
  observe it. The footprint system does not track private versus visible writes today.
- Construction versus behaviour in a bundle: initial values and creation-time work separate
  from the methods that reload, as CAOS separated install scripts from event scripts.
- A delayed message that arrives after its state has already ended and been re-entered.
- How a table absorbs a message, and in which step. (Settled: it can, one at a time, in
  arrival order.)
- The display relations: one relation with several queries, or several relations.
- How a host-compiled snippet sees wires. Leaning (owner): free wire names become parameters,
  and the wirer's closure captures and forces them. See the shape note §3.
- Which use the first version serves.
