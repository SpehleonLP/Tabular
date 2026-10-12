# Retrospective: the first Tabular (engine `src/Tabular`, 2026-10-07 to 2026-10-10)

Tabular is starting over in this repository. This note records why, so the restart does not
repeat it. Commit hashes refer to the Kreatures-Engine superproject. Paths are relative to
that superproject's root.

## The shared cause

**No definition of Tabular ever sat above the slices.** The arc
(`docs/arcs/tables-as-queries.md`) held a model that was still being designed. It labels
its own authoring form "(speculative)" and goal/progress "(tentative)". Each spec cut from
it answered "what does this slice add?" and none answered "what is Tabular?". Every problem
below follows from that.

## Timeline

43 commits in four days, about 14.9k lines (including tests), built as three slices. Each
slice's spec and plan were written the same day as its code:

| Date | Slice | Spec |
|---|---|---|
| 2026-10-07 | Cook: one table to flat data, checked against the animation lookup | `docs/superpowers/landed/specs/2026-10-07-tabular-cook-and-lookup-parity-design.md` |
| 2026-10-08 | Attach: paths, wires, loopbacks, `Tabular@` on a script object | `docs/superpowers/landed/specs/2026-10-08-tabular-attach-design.md` |
| 2026-10-09 | State and rules: tables own state, names are hashes, conditions compiled to machine code | `docs/superpowers/landed/specs/2026-10-09-tabular-state-and-rules-design.md` |

In those four days the core changed underneath itself:

- **File format:** v1, then v2 (`95ea066b9`, day 2), then v3 (`8093b3484`, day 3,
  "every name carries its hash").
- **State ownership:** slice 2 put state in a record declared by a script. Slice 3
  (`181420ef2`) reversed this because the script had to mirror the table's layout.
- **Conditions:** slice 3 made conditions AngelScript expressions, parsed with the fork's
  parser, typed by `as_compiler.cpp`'s rules, and compiled with asmjit (`e21d97dd7`,
  `cc9d9b8d5`). That was decided and built in one day.

## What went wrong

### 1. Built by accumulation

There were specs, but only slice specs. With no contract above them, each slice could
redefine the core, and each one did (see the timeline).

### 2. Too tightly coupled to the engine

The coupling was concentrated, not spread everywhere. About 3k lines needed nothing from
the engine beyond `Errors/` and `Support/`: the cook, the file format and loader, the table
graph, the source parser and the codegen emitter. The coupling was in two places:

- **The condition language.** It used the AngelScript fork's `asCParser`
  (`Cook/tabular_condition.cpp`), typed expressions by AngelScript's compiler rules
  (`Compile/condition_typing.cpp`), resolved paths through the engine's `CompiledPath`, and
  called into script through `TabularCall`. AngelScript was not plugged into Tabular; it
  *was* Tabular's type system.
- **`tabular_instance.cpp`.** It had 11 engine includes: Packages, WorldDescription,
  ThreadPool, the script-engine mutex and AngelScript registration. Tabular knew how the
  engine hosted it, instead of the engine knowing how to host Tabular.

The question that was never decided: **is AngelScript part of what Tabular is, or one host
it plugs into?** The code decided it one slice at a time.

### 3. No public/private split

There was no `include/` versus `src/` boundary. That follows from 1 and 2: you cannot draw
a public line without a public contract. The de facto API was whatever
`tabular_instance.h` and `tabular_binding.h` exposed, including `CompiledPath` and the
asmjit-compiled rules (`CompiledRules`).

### 4. Reviews checked code against plans, not plans against a contract

The two "final-review fixes" commits (`7d188584f`, `c5edabaeb`) are local: missing refusals,
bracing, comment wording, extra loader checks, added tests. Each review checked code
against a plan written in the same session. No review could ask "why are we on format v3
by Thursday?" or "should a language that promises to stay decidable call into script?"
There was nothing outside the plan to judge it against.

### 5. Cost decisions were made without measurements

- **The cook** was justified as "parsing takes time, and with around 800 game objects that
  becomes a real share of load time" (cook spec). No measurement backs this.
- **Bind** was the likely real cost: path resolution through reflection, AngelScript type
  lookup, and one asmjit compile per binding. The notes defer sharing compiled code
  "when a measurement shows bind time or code memory matters". That measurement was never
  taken.
- **The JIT** rests on "conditions turn out to be most of the game logic". That is
  plausible, but it was never compared with a plain interpreter over flat nodes.

As a calibration, a simple hand-written lexer reads roughly 0.2–1 GB/s. A table file is a
few KB, so parsing it should take microseconds. When loading or binding takes
milliseconds, the time is going into per-element work: an allocation per node, generic
trees converted into real structs, strings used as identity, and pointer-chasing through
reflection.

### 6. The cook caused the AngelScript dilemma and saved the wrong cost

A cook runs offline, where the engine's types do not exist, yet it still had to
understand conditions. That left two bad options: put AngelScript's parser in the cook
(what happened, which is coupling) or write a parallel parser (two definitions of one
language that drift).

The cook also made the cheap step cheaper. Parsing a few KB of text costs microseconds,
and binding, the expensive step, can never be done offline. Its price was:

- a binary format that went through three versions in three days;
- staleness checks (`should_rewrite`) and a failed-cook marker (`82c894c32`);
- hooking the cook into the file watcher (`da9ef8a5e`);
- a loader that re-validated every section the cook had already checked (`329245710`);
- names stored in the file both as strings and as hashes.

The mesh cook (CBOR to a custom binary, about 1000 ms to 70 ms) is not a precedent. Meshes
are megabytes, and their binary layout *is* the GPU layout. Tables have neither property.

### 6a. Native numeric conditions were the slippery slope

Folding comparisons like `energy < 100` into Tabular looked cheap and decidable. One thing
led to another: Tabular then needed to know what a wire is, inputs were no longer opaque, and
it had to mirror AngelScript's types. In the restart they are script conditions.

### 7. Generic versus specific

The arc set out to make "one shape for all behaviour tables" (§1) that served animation
lookups, brain decisions and global relations. Each use pulled in its own features: a grid
layout for lookups plus a separate rules layout for decisions (slice 3 kept both), a
growing list of match operators and pick policies, and state owned by a table. "Generic"
became a union of specific features, with no single rule for whether a feature belonged
in the core.

## What to keep

- **Hashes as runtime identity.** Interning names when you bind is cheap and fast. It does
  not need a file format.
- **The oracle idea.** Holding generated or compiled behaviour to a reference
  implementation's answers (`test_rules_oracle.cpp` against the AngelScript VM).
- **The decidability invariant (arc §2).** Tables stay below the line where Rice's theorem
  applies: finite rows, no recursion, a DAG within a tick.
- **The engine-free parts as reference material** (the cook's checks, the graph, the
  source parser), to mine once a definition exists, not before.

## What this means for the restart

1. **Write the definition before any code.** What a table is, what it reads, what it
   produces, what it promises (decidability, determinism, hot reload) and what it refuses
   to be. Anything still speculative is either decided or explicitly out of scope.
2. **Decide the host boundary first.** Tabular owns table structure. The host owns the
   expression language and is reached only through an interface Tabular defines.
3. **No cook.** Parse text at load, and put the effort into fast binding: flat data,
   hashes instead of strings, no allocation per node, binding work shared per type rather
   than per object.
4. **The public header is the contract.** `include/` comes from the definition before any
   implementation, and the engine is a consumer that implements the host interface.
5. **Cost budgets are part of the definition**, as numbers (load, bind per type, evaluate
   per row), with a benchmark that fails when one is broken.
6. **Review against the contract.** A review asks "does this keep the definition's
   promises and budgets?" The first thing reviewed is the public header, not a diff, and the
   reviewer is not the plan's author.
7. **A tiny generic core.** Rows, key match, pick a row, produce a value, plus the
   invariants. A feature that only one use needs lives in that use's adapter.

## Open questions

- **Which use does the first version serve?** Brain-style ordered rules, animation-style
  grid lookups, or both from day one?
- **Who guarantees decidability?** If conditions are host-language text, Tabular cannot
  guarantee it alone. The host interface has to promise it, for example side-effect-free
  reads and no loops.
- **What happens to the engine's `src/Tabular`?** It is still built and still has tests
  listed in `cmake/sources_tests.cmake`.
