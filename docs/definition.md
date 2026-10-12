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
- **Not a Turing machine.** Anything that needs one is a message to script.

## Structure

**Two layers.**

| Layer | Contents |
|---|---|
| Relations | rows, key columns, match operators, pick policies; queries |
| Machines | a state vector, transition tables, entry tables, bridges, edges |

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

- What each column type is, and how many of each one table may have.
- What a bridge step carries (state or clip, exit, commands on entry, bounded repeat).
- Counters and timers: a table primitive, or script?
- How a table absorbs a message, and in which step. (Settled: it can, one at a time, in
  arrival order.)
- The display relations: one relation with several queries, or several relations.
- Which use the first version serves.
