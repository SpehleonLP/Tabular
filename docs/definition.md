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
- **Not a scheduler.** It does not decide which instances step, when or on which thread.
- **Not file IO.** It receives text or builder calls, never paths.
- **Not the owner of wires, the bundle file or hot reload policy.** Those belong to the
  subsystem that has Tabular. Tabular cannot tell a wire from any other named input.
- **Not a mirror of the host's types.** A comparison like `energy < 100` is a script
  condition, never a native one. Folding numeric comparisons in was tried in the first
  version: it then needed to know what a wire is, inputs stopped being opaque, and it had to
  mirror AngelScript's types. Native conditions work only on Tabular's own input types:
  **hash, flag set, `int64_t` and `double`.** The host converts its values to these. A native
  comparison is one input against a literal (`energy < 100` is allowed as a band or comparison);
  anything that combines inputs, does arithmetic or calls is a script condition.
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
- **At most one weight** per row.
- **Values:** several allowed. A value is a **write** to the instance's own state vector.
  Writes always go to the *next* vector, which is swapped in at the end of the step. **A value
  cell holds no math:** a literal or a name, never an expression.
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
- **Input resolution** for every name a table reads that Tabular does not own.
- **Sinks** for emitted commands and messages.
- **A seed** per instance and step.

Closures are `{function, data, release}`. `release` is called when a binding is dropped.

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
- A pivoted value axis (the phase table sets every flag whose band holds): is each header
  evaluated as its own one-value table, or does the table pick `all`?
- Whether a command's consumer compiles its s-expression at bind time (as the host compiles
  snippets), so a step enqueues a prebuilt record rather than text.
- What a bridge step carries (state or clip, exit, commands on entry, bounded repeat).
- Counters and timers: a table primitive, or script?
- How a table absorbs a message, and in which step. (Settled: it can, one at a time, in
  arrival order.)
- The display relations: one relation with several queries, or several relations.
- Which use the first version serves.
