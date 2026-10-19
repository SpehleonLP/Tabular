# Commands and messages

**Status:** design, not built. Taken from the owner's conversation of 2026-10-19, after slices
0–8 landed. It replaces the slice-4 message design (R3: the message type as a key, arguments
as positional input slots). **Contract:** `docs/definition.md`; each part below names the
contract text it changes.

**Worked example:** the Mansion craft bench (`game/Lifaundi/Development/Mansion/CraftBench/
CraftUX.as`) rewritten as a machine, at https://claude.ai/artifact/Hhb1VchjDqjXBWe54V1YQ9. It
sorts every message the script reads into what tables branch on and where its payload goes.
In that example no transition reads an argument's value. Three test something about a payload
(a double-click? which window closed? a count above zero?). The host can make each of those
tests before the step and pass the result as a type or a flag. The payloads that matter (which
item, how many) go only to commands.

There are four parts, built in this order: CSV quoting, commands as lists, messages out of
Tabular, then hierarchy (entry per column, fall-through order, override clauses). Part 2 depends
on Part 1 reading cells correctly, and Part 4's one-table form uses Part 3's fall-through
declaration.

## Part 1. CSV quoting

**Why.** Tables are authored in a spreadsheet editor, not a text editor. A spreadsheet's CSV
export wraps any cell that holds a comma, a double quote or a line break in quotes, and doubles
the quotes inside. So a cell holding `(message food "eat")` is saved as
`"(message food ""eat"")"`. `definition.md` says "CSV has no quoting", so today that cell is
read with its outer quotes and doubled quotes intact.

**Rule.** Every CSV front end (rows, pivoted, grid) reads RFC 4180 quoting in every column:

- A cell that starts with `"` (after leading spaces) is quoted. It runs to the matching closing
  `"`, `""` inside it is one `"`, and commas and line breaks inside it are text.
- Spaces outside the quotes are trimmed. Text inside the quotes is kept exactly.
- An unquoted cell is trimmed as today. A `"` inside an unquoted cell is refused, because a
  spreadsheet never writes one.
- A quoted cell that spans line breaks is one logical row. Refusals name the line the row
  starts on.
- A line is a comment only if its first unquoted character is `#`. Blank lines are still
  skipped, and a BOM is still dropped.
- An unterminated quote is refused, naming the line where it opened.

**Contract edits.** Replace "CSV has no quoting, so a command cell cannot hold a comma, except
the suffix…" (definition.md, the CSV cells paragraph) with the rule above.

**Tests.** A quoted command with a comma and doubled quotes; a quoted hash key; an unterminated
quote; a stray `"` in an unquoted cell; a quoted cell spanning two lines, with a refusal on the
following row naming the right line; the same tables saved by LibreOffice and by Excel
(fixtures).

## Part 2. Commands as lists

**Why.** `definition.md` already says "the suffix *as a whole* is the command: `(C, D, E)` is
passed, as an s-expression, to whoever consumes it". The build passes the raw rest-of-line
text instead (`(anim chew), (message food "eat")`), refuses a row with no suffix, and calls
`host_binding::sink` once per emit. The craft bench needed `(nop)` in a third of its rows for
that reason.

**Rule.**

- **Cell boundaries in the suffix mean nothing.** Authors spread commands across cells so the
  spreadsheet reads in neat columns. The suffix cells, after Part 1's unquoting, are joined with
  one space and read as a **sequence of s-expressions**. `(emit sound)` in one cell and `(emit` |
  `sound)` across two cells read the same.
- **Tabular wraps the sequence into one list,** so the row's command is
  `((emit sound) (play particle) (message target hit 3))`. **No suffix cells means `()`,** which
  is "do nothing". A row with an empty suffix is no longer refused.
- **A top-level item is a list or a single word.** A list names what it does with its head
  (`(message this calm)`, `(sound no__)`). A single word (`hard-land`, `idle`, `:lock`) stays a
  word, and the consumer decides what it means. The animation consumer reads a bare word as a
  clip and a `:word` as a directive to itself. This retires the animation bridge convention of
  telling messages from clips by quoting: a message is `(message …)` (see Bridge tables).
- **A cell written without parentheses is wrapped, not refused** (owner, 2026-10-19): its
  meaning is clear, the author just didn't write it lispy. Outside an open parenthesis, a
  suffix cell whose trimmed text holds whitespace and does not start with `(` is one command,
  and Tabular wraps the whole cell: `message this hard-land` reads as
  `(message this hard-land)`. A cell holding one word stays that word. So the joining rule above
  applies inside parentheses, where a command may run across cells. A cell that starts with `(`
  holds one or more s-expressions.
- **The shape is checked at load and nothing more.** Lexically there are parentheses,
  double-quoted strings (with `\"` and `\\` escapes) and atoms. Unbalanced parentheses or an
  unterminated string is refused, naming the line. Tabular still never interprets a command,
  and compiling one stays with the host (R9).
- **The wrapped list is what the store keeps.** It is built once at load, so a step never
  parses or builds text.

**Results are returned, not sunk.**

- `Step` (and `CreateInstance` and `Rebind`) returns the commands of the rows that answered, in
  the order already defined: step, then exit, then entry, each in clause order.
- Each result carries the table and the command, a view into the version the binding holds,
  valid while the binding lives.
- The most results one step can have is the number of emitting clauses across the three lists.
  That is known at `Prepare`, so each instance holds a fixed buffer of that size, and `Step`
  returns a span over it. A step still allocates nothing, and the `std::function` call per
  command goes away.
- `host_binding::sink` is removed.

**Contract edits.** The Suffix paragraph: replace "the rest of the line… commas and spacing kept
exactly… a row with no suffix text is refused" with the rule above. Emits (step order item 7):
"hands its row's command to the sink" becomes "adds its row's command to the step's results".
Update the interface in the As-built section.

**Tests.** One command across two cells equals the same command in one cell; three commands
give a three-item list; an empty suffix gives `()`; `message this hard-land` reads as
`(message this hard-land)` and `idle` stays `idle`; an
unbalanced paren and an unterminated string are each refused with a line; the step's result order matches step, exit,
entry; the allocation-free step test still passes with results returned; the budget gate is
re-measured.

### Bridge tables

The animation system already defines bridge tables (`docs/superpowers/landed/specs/
2026-05-05-pipeline-asset-format-design.md`, `2026-05-15-animation-state-design.md`): `(from,
phase, mode, to) → step list`, where a step plays a clip or sends a message. Its CSV marks a
message by quoting it and a clip by leaving it bare (`animation_pipeline_cook_string_table.cpp`,
`add_bridge_seq`). The owner calls that convention poor, and it was never really tested.

Under Part 2 a bridge table is an ordinary Tabular table with a suffix, and each step is a
command:

```csv
from,    phase, mode, to,    steps
falling, hard,  _,    idle,  message this hard-land, :lock, hard-land, :unlock, stand-from-hard-land, idle
_,       _,     _,    eat,   HeadDown
eat,     _,     _,    chew,  HeadUp,     (message this "chewed")
```

The first row reads as
`((message this hard-land) :lock hard-land :unlock stand-from-hard-land idle)`.

- The animation system is the consumer. Its timing idioms ("let X finish", "hard cut to Y",
  crossfade when no row matches), and `:lock` / `:unlock`, are how it interprets the list, not
  Tabular's business. See Integration for how the host runs such a list over time.
- A clip is a bare word and a message is `(message …)`. Quoting no longer carries meaning.
- Moving the animation cook onto Tabular tables is a follow-on, not part of this spec. Until
  then, the animation CSV keeps its own reader.

## Part 3. Messages out of Tabular

**Why.** A message's payload is like a one-row relation that exists only for that event. In
SQL, only the query that handles it joins against it:

```sql
SELECT r.contact FROM contact_rules r
JOIN collision m ON m.ny >= r.ny_lo AND m.ny < r.ny_hi
```

No other table carries an `ny` column. R3 instead made every message's arguments inputs of the
one machine, as shared positional slots: a missing argument read 0, a type's arity could not be
declared, and arguments could only filter rows, never carry data.

**Rule.**

- **Incoming: the host dispatches a message to a handler machine.** The handler's declared
  inputs are that message type's payload, named and typed. Getters read them from the message,
  and the host steps the handler once per message. Tabular sees an ordinary machine being
  stepped. Arity is the handler's input list.

  ```yaml
  machines:
    - name: on_collision
      inputs:
        - {name: ny,    type: double, range: -1..1}
        - {name: speed, type: double, range: 0..50}
      state:
        - {name: contact, type: hash, members: [none, floor, wall, ceiling], initial: none}
      step:
        - from: contact_kind      # ny 0.7..1 floor, -0.7..0.7 wall, -1..-0.7 ceiling
          given: {ny: input.ny, speed: input.speed}
          set:   {contact: kind}
          emit:  command          # (sound thud) on a hard hit
  ```

- **Outgoing: a message is a command,** such as `(message target type args…)`, in the row's
  list (Part 2).
- **Machines chain by state.** One machine writes state, and a command or a getter carries it
  to the next machine, which picks from it. Animation already works this way:
  1. the animation reports, and a machine picks a state;
  2. the picked state goes out as a command, which sets state;
  3. that state drives a machine that picks the clip.
- **Levels stay wires.** "Am I on the ground" or "am I against a wall" is a level that physics
  knows every tick. It should be a wire, not rebuilt from collision messages. A handler is for
  the event itself: an impact's sound, damage or stagger.
- **A type key without a message concept.** A machine that branches on many argument-free
  types (the craft bench's `select`, `back`…) keys on an ordinary hash input the host fills for
  one step. The fall-through becomes a declaration on any hash key column (spelling to settle,
  e.g. `fallthrough: true`): a value with no run retries the blank run. A blank cell (hash 0)
  is allowed only in such a column. Slice 4's rules carry over unchanged: 0 does not leak into a
  key without the declaration.

**Removed:** `message_inputs`, `struct message` and `Step`'s message parameter, the positional
argument slots and their extra-arguments report, `domain: message` (replaced by the
fall-through declaration), and `Absorbed()`. The host knows whether it has a handler for a type,
so it can route an unhandled message to the object's script without asking Tabular.

**Contract edits.** Rewrite R3's paragraphs (messages, the message key, arguments, Absorbed) as
above. Move "routing a message to a handler, and to the script when there is none" to the host
adapter list. Update the grazer worked example: `scared` and `calm` become the type input with a
fall-through, or each gets a handler.

**Tests.** The collision handler classifies floor, wall and ceiling from `ny` and emits on a
hard hit. A fall-through key: an unnamed type answers from the blank run, and a blank cell
outside a fall-through column is refused. The grazer and craft-bench machines step with no
message concept.

## Part 4. Hierarchy: entry per column, fall-through order, override clauses

**Why.** A hierarchical state machine (HFSM) is a left outer join with a coalesce: the
substate's own transition wins, and where it has none, its superstate's applies.

```sql
SELECT COALESCE(child.next, parent.next)
FROM state s
LEFT JOIN child_rules  child  ON child.sub    = s.sub   AND child.msg  = :msg
JOIN      parent_rules parent ON parent.super = s.super AND parent.msg = :msg
```

Tabular can express this in two ways: in one table with fall-through, or in two tables with
an override clause. Both need entry to run per column.

### Entry and exit run per column (decided, owner 2026-10-19)

Today entry and exit run their whole list when any hash state column changes. With two
columns, a change to one re-runs the other's rows. In the craft bench, `confirm → result`
changes only `modal`, but `page_leave` emits `(hide-page experiment)` and `page_enter` emits
`(show-page experiment)`, so the page is hidden and shown again in the same step. In an HFSM,
`walk → run` would re-run `move`'s entry.

**Rule.** An entry or exit clause runs only when a hash column it keys on as `state.<col>`
changed. This is inferred from its `where:` and needs no declaration. A clause that keys on no
state column runs when any hash column changed, as every clause does today. "Changed" keeps its
meaning: next differs from current at the end of the step list, so A → B → A changes nothing.

### Hierarchy in one table: fall-through on the substate key

Part 3 moves fall-through onto a declaration any hash key column can make. Put it on the
substate key:

```yaml
state:
  - {name: super, type: hash, members: [move, rest]}
  - {name: sub,   type: hash, members: [walk, run, sit, sleep]}
step:
  - from:  behaviour
    where: {super: state.super, sub: state.sub, kind: input.kind}   # sub and kind: fallthrough
```

```csv
super, sub,  kind,  next_super, next_sub
move,  ,     hit,   rest,       sit       # the superstate's rule: every move substate
move,  run,  hit,   move,       walk      # run overrides it
```

A `walk` that is hit finds no `(move, walk, hit)` run, retries with a blank `sub`, and answers
from the superstate's row. A `run` answers from its own row.

- **Retry order (decided): the substate gives way first.** The fall-through columns give way in
  key order. Treat them as bits, with the first fall-through column in key order as the lowest
  bit, and try each combination of blanks in counting order:

  | Try | Key |
  |---|---|
  | exact | `(move, walk, hit)` |
  | 1 | `(move, blank, hit)` |
  | 2 | `(move, walk, blank)` |
  | 3 | `(move, blank, blank)` |

  So the author lists key columns from the one that should give way first to the one that
  should hold out longest. Here the substate gives way before the message does.
- **Child runs are total (decided).** A fall-through happens only when there is no run at all,
  as today. A run that exists answers for every input in its domain. So an override replaces
  the superstate's rule for that key completely, and the totality proof per run stands.
- **Which substates belong to which superstate** is not expressed by the members lists: a
  `(rest, walk)` state would be legal. The fix is ordinary key-domain machinery. `sub`'s domain
  comes from a table of (super, sub) pairs, so `Prepare` reports or refuses mismatched pairs.
- A third level is a third state column with its own fall-through.

### Hierarchy in two tables: an override clause

**Why it is wanted beyond hierarchy.** Two owners' rows with equal flags may not overlap in
one key run (owner, 2026-10-18). So a package cannot override another package's rows in the
same table: it needs a table of its own, and a clause that lets that table win where it has a
row. That is the same outer join.

**Rule.** A clause may declare `overrides: <earlier clause>`:

- **Its table need not be total.** A miss is NULL: the clause writes nothing, emits nothing, is
  not reported and is not a hold.
- **A hit replaces the overridden clause's answer:** its sets and its emit, not as well as.
  That is the coalesce. Commands follow the same rule as state.
- **It must directly follow the clause it overrides, in the same list, and set a subset of that
  clause's columns.** A later clause that reads the overridden clause as a source reads the
  coalesced answer.
- **The machine stays total.** Totality is a property of the whole machine, not of each table:
  every override names a clause that is itself total, so for every reachable key and input
  something answers. `Prepare` refuses an override of a clause that is itself an override, or
  of one that may hold for want of a run.

```yaml
step:
  - from:  grazer_brain          # the base package's table, total
    where: {mode: state.mode}
    given: {energy: input.energy}
    set:   {mode: next}
    emit:  command
  - from:  grazer_brain_mod      # a mod's table, rows only where it changes something
    overrides: 0
    where: {mode: state.mode}
    given: {energy: input.energy}
    set:   {mode: next}
    emit:  command
```

**Contract edits.** Step order item 5 (Entry): entry and exit run per column, as above. The
clause definition gains `overrides`. The key-domain section gains the fall-through retry order.

**Tests.**
- In the craft bench, a modal-only change runs no page entry or exit row.
- In an HFSM, `walk → run` runs `run`'s entry and not `move`'s.
- Fall-through gives the superstate's row for `walk` and the override for `run`.
- Retry order with two fall-through columns.
- A `(rest, walk)` pair is reported from the pair-table domain.
- An override clause: a miss leaves the base clause's sets and emit; a hit replaces both; a
  dependent reads the coalesced answer.
- An override that does not directly follow its clause, sets a column its clause doesn't, or
  overrides an override is refused.

## Integration (the host side)

How the program uses Tabular, end to end. Everything here is the host's; Tabular's part is
named at each step. The first attempt at Tabular failed by trying to make it understand the
things in "The command runner" below. That stays out.

**Loading.** A `.tabular` file is YAML holding tables, machines and wires, plus the CSV texts
of the tables' rows.

- `DocumentFromYaml(text, label)` reads the tables and machines; each table's rows load
  through `LoadRows(csv_rows{…})`.
- **Wires** connect a machine's declared input names (`energy`, `food_near`) to engine data.
  They are not read by Tabular today. The integration layer reads a `wires:` section (spelling
  to settle, e.g. `energy: chem.glucose`) and turns each entry into an `input_getter` when
  `host_binding::resolve` asks for that name. Wire resolution is already on the host adapter
  list.
- **Conditions.** Tabular never interprets one. The loader's `condition` callback turns each
  condition cell's text into a `closure` (function, data, release), and a pick calls it with the
  object as `this_in`. The integration layer owns that compiler, e.g. compiling the cell as an
  AngelScript expression against the object type (owner, 2026-10-16: Tabular owning this went
  badly in the first version).

**Running.** There are two shapes.

```cpp
// A one-off pick on a plain table: the row, then its cells (a command is a view)
auto row = PickBest(store, table, {keys, inputs, self});

// A machine, set up once per object type
auto machine = Prepare(def, store);               // proves totality, builds the layout
auto binding = Bind(machine, {resolve, report});  // resolve: input name → getter (the wires)

// Once per object: the instance is a snake_case member of the object, never new'd
creature.brain = CreateInstance(*binding, id, seed, &creature);

// Every tick the object is awake
for (std::string_view list : Step(creature.brain, &creature, seed))   // Part 2's results
{
	runner.Start(creature, list);
}

// After a store change (hot reload)
if (machine->Stale(store)) { /* Prepare again, Bind again, Rebind each instance */ }
```

- A one-off pick has exactly one row, so an out-parameter view (`std::string_view&
  command_out`) fits it. A step can emit from a step clause, an exit clause and an entry clause
  in the same call, so it returns a span (Part 2).
- Results are views into the table version the binding holds, valid until a reload replaces
  the binding. A consumer that keeps a list past that copies it or holds the binding.
- The command consumer parses a row's list once, the first time that row emits, and caches it
  by (table, row) in the binding. That is "compile at bind", already on the host list (R9).

**The command runner.** A command list is a small program that the host runs over time, not a
set of instant effects. The bridge row above:

```
((message this hard-land) :lock hard-land :unlock stand-from-hard-land idle)
```

1. The machine's step moves the state from `falling` to `idle` at once. The table's state is
   authoritative and changes instantly.
2. The runner starts the list. `(message this hard-land)` takes no time: it goes into the
   object's Mailbox now.
3. `:lock` tells the host not to step this machine for now, so its tables don't run.
4. `hard-land` plays a clip, which takes seconds.
5. `:unlock` lets the host step the machine again.
6. `stand-from-hard-land` plays, then `idle`, which is the clip the state wanted all along.

Say the player jumps during `stand-from-hard-land`:

- The machine's state changes to `jumping`.
- The list is unlocked, so the runner cancels the rest of it.
- The runner looks for a bridge from `stand-from-hard-land` to `jumping`. There is no row, so it
  cuts straight to the jump. The character stays responsive.

Some items take zero ticks and some take seconds; some are directives to the runner itself; and
an interruption cancels the remainder. None of this is Tabular's business. Tabular hands over
the list, and the runner does the rest.

**Rules this gives the host** (owner, 2026-10-19):

- **A message a command sends goes into the object's Mailbox,** and the engine's ordinary
  delivery rules decide when it arrives. So the craft bench's `basket_filled` needs no Tabular
  rule. `Step` returns its commands to the caller, which runs them.
- **A message an instance's own command sends never comes back into that instance's machine.**
  In the hard-land example the state is already `idle` when `hard-land` arrives, and the
  transition that sent it must not run the tables again. Mechanically this is the host's to
  solve, for instance by tagging the message with the instance that sent it and skipping that
  machine when routing. Tabular needs nothing.
- **The runner chases the machine's current state.** The state changes instantly. The runner
  works toward it through the list, and when the state changes again it cancels the remainder
  and bridges from wherever it is, or cuts if no bridge row exists.
- **`:lock` and `:unlock` are commands to the host: "don't step this machine right now" and
  "step it again".** This is speculative (the owner came up with it on the spot) and the host's
  business entirely. While locked the machine's state doesn't change, so the runner has nothing
  to chase, and on unlock the next step decides what happens from there.
- **Leaving from any state means the host ends the instance.** Closing isn't a transition, so
  no exit row is needed for every state. Tabular needs one small API for it: run the exit list
  and return its commands. `Rebind` already does this internally.

## Authoring note: first match wins (gambits)

The craft bench's Back is a gambit list, as in FFXII: try each line from the top, and the first
that matches is the answer. Close the sort panel if it's open, else back out of the basket,
else leave. Tabular already has this ordering, through **conditions**:

```csv
page,       modal, kind, when,           next_page,  next_modal, command
experiment, none,  back, sort_open,      experiment, none,       (toggle-sort)
experiment, none,  back, can_back,       experiment, none,       (basket-back)
experiment, none,  back, ,               experiment, none,       (close)
```

- Rows with the same flags (here, none) are tried **in row order**. A row whose conditions
  don't hold falls through to the next, and the first that holds wins (definition.md,
  Picking: "ranked fall-through").
- `when` is a script condition column. The host's condition compiler turns `sort_open` and
  `can_back` into closures against the bench object.
- The last row has no condition, so the run is total. A row after it could never be reached,
  and the load refuses it as dead.

**This is not the flags rule.** Flags rank rather than filter. Among rows whose flags are all in
the input, the larger `R & F` wins, so the higher bit wins regardless of row order. Written
with flags, the bench's order would hold only if `sort_open` happened to be a higher bit than
`can_back`. Use flags when priority belongs to the input (the host decides which wire matters
most), and conditions when it belongs to the table (the author's line order).

**A gap, proposed:** a gambit over host wires costs a closure call per tried row, because
conditions are scripts. A flags column declared as a plain condition (spelling to settle, e.g.
`rank: false`) would make a row count only when its flags are all in the input, `R & F == R`,
and leave the order to the rows. That gives first-match over wires with no script. The
contract already lists "flags subset" among condition kinds, but only the ranking column is
built.

## Open questions

None remain from the craft bench. Messages, the command runner and hierarchy are decided as
above; `:lock` is marked speculative.
