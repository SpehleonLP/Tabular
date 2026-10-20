# Commands and messages

**Status:** Parts 1 to 4 built (2026-10-19, branch tabular-commands; see "As built" under
each), except Part 4's substate membership (the pair-table domain), which waits for the owner
with the other Open questions; the spec stays active until those are decided. Taken from the owner's conversation of 2026-10-19, after slices
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

## Principle: many authoring surfaces, one internal table (owner, 2026-10-19)

Tabular should have more ways to write a table than kinds of table inside. Authors write
whatever reads most naturally, and each surface compiles to the same store, layout and picks.
`definition.md` already says the builder API is the core and YAML and CSV are front ends that
produce its commands. This extends that from file formats to authoring idioms.

Surfaces that already compile to one internal form, or are proposed here:

- rows CSV, pivoted CSV (flags as headers) and grid CSV, all loading into the same store;
- a command written with parentheses, without them (wrapped), or spread across cells (Part 2);
- an HFSM in one table with fall-through, or in two tables with an override clause (Part 4);
- a gambit list (conditions in row order) next to a ranked table (flags by bit).

**The test for a new feature is the authoring side of the premise/consequence method:** can it
be a front end that writes rows the core already has? If so, it gets a front end and no core
code. It reaches the core only when the internal form cannot express it. The proposed
non-ranking flags condition is that case: no existing row kind filters on wire bits in row
order without calling a script. It is deferred, so this spec builds no core code from the
principle.

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

**As built.** `src/csv_text.h` is the one reader for the rows, pivoted and grid front ends;
Spehleon/lib's `CSV::File` is untouched. Decided in building: a spreadsheet's padding is
ignored (trailing empty cells, and a row of nothing but separators is a blank line), while a
non-empty cell past the header is still refused; a row shorter than the header is still
refused. Tests: `CsvQuoting`.

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

**As built.** The CSV front end builds the list (`src/command_text.h`, `CommandList`); the
store checks the shape of every command column, whichever surface wrote it, and keeps what it
is given, so a builder caller writes the list itself. Decided in building:

- The wrap rule counts items, not whitespace: a cell holding only a string (`"a b"`, written
  `"""a b"""` in CSV) is one item and is not wrapped. A cell of several items that does not
  start with `(` and is not whole by itself (`message food (say`, `a) (b`) is refused rather
  than wrapped, and the store requires exactly one list, not just balanced text (review,
  2026-10-19: cells `a)` and `(b` balanced to `(a) (b)`). A cell that is exactly `()` adds
  nothing.
- A backslash inside a string escapes whatever follows it; only an unterminated string is
  refused.
- Every command column is a list of commands (2026-10-19, after review): a declared
  (non-suffix) command column's cell is read by the suffix's rule, so `anim chew` and
  `(anim chew)` both read as `((anim chew))` and `()` stays `()`. An empty one is still
  refused, as every empty CSV cell is. An empty command reaching the store is refused too: "do
  nothing" is `()`. A grid's command cells are not emitted by any clause and stay as written.
- Each result is `emitted{list, table, command}`; the list lets a host tell an exit's abort
  from an entry's start, and lets a rebind's results be attributed to the old or new binding.
- `CreateInstance` returns the instance, so creation's results are read from
  `instance::Emitted()`, the last run's span, which `Step` and `Rebind` also return.
- The buffer holds one entry per clause that can emit across all four lists, counted at `Bind`
  from the prepared machine; a rebind sizes it for the exit results already taken plus the new
  binding's emitters.
- A rebind's exit commands view the old binding's versions, so the instance keeps the old
  binding until its next run; a host may drop its own reference straight after `Rebind`.

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

**As built** (2026-10-19). `message_inputs`, `struct message`, `Step`'s message parameter, the
argument slots and their report, and `Absorbed()` are gone; every machine input is a getter's.
`Close(instance, self, seed)` is the host's "run the exit list and return its commands"; the
rebind's leaving half is the same code. Decided in building:

- **Spelling:** `fallthrough: true` on a column, in place of `domain:` (declaring both is
  refused). In the builder it stays a key domain, `key_domain::FallThrough()` (R3's `Message`
  kind renamed), so the domain machinery (blank twin, orphans, a foreign domain dropping the 0)
  carries over unchanged.
- **At most one per table** when Part 3 was built; Part 4 lifted it to eight, retried in key
  order (Part 4, As built).
- **Any hash source may feed it.** R3 bound a message key to the message type both ways; with no
  message concept that pairing is gone. A fall-through key stays open to `Prepare` whatever
  feeds it (a state column or literal does not close it), since a value with no run falls
  through rather than being a gap. An ordinary key still may not read an earlier clause's
  fall-through column (it can answer 0); a fall-through key may.
- **`clause_answer::fellThrough`** replaces `Absorbed()` as the per-clause record (it fits in the
  answer's padding, so no instance grows); provenance reads it, and keys keep the value as read.
- **`Close`** reads inputs without crossing an edge, as a rebind's exit does, and leaves the
  other lists' answers as they were; `provenance_run::Close` marks its entries.
- Tests rewrote R3's message tests as event inputs the host fills for one step; the extra-arguments
  and argument-previous-value tests went with the concept.

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

**Hierarchy within a package (owner, 2026-10-20).** A package does not see another package's
tables, so one package never overrides another's clause; an override is hierarchy inside one
package. Several contributors write the same tables only in an engine-owned system (dialog,
the display relations). If overrides are allowed there, they follow the level rule below.

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
- **It gives way level by level with that clause** (owner, 2026-10-20; see As built): at each
  blanking of that clause's fall-through keys the override is tried first, then that clause, so
  the more specific row wins and the override a tie. So it falls through on each of those keys,
  read from the same source, or is refused.

```yaml
step:
  - from:  grazer_brain          # the base package's table, total
    where: {mode: state.mode}
    given: {energy: input.energy}
    set:   {mode: next}
    emit:  command
  - from:  grazer_brain_detail   # the same package's second table, rows only where it changes something
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

**As built** (2026-10-19). Entry and exit per column, several fall-through keys in the retry
order above, and override clauses are built; the pair-table domain is not (below). Tests:
`test/hierarchy_gtest.cpp`, including the craft bench's modal-only change, the HFSM's
`walk → run` entry and its fall-through, the two-key retry order (`PickBest` and `Step` share
it), the override's miss, hit and coalesced dependent, its refusals, and
`ManySurfaces.OneTableAndTwoTablesWithAnOverrideRunTheSame`, which runs one machine written in
one table and in two through the same events and compares states and commands step by step
(`TheTwoFormsLeaveAndEnterTheSameAcrossModeChanges` repeats it with entry and exit lists across
superstate changes, and `Close`). Also covered: an override in an exit list and its group, under
`Close` too; an override that fails (reported, the base's answer stands); an override's own
fall-through feeding the coalesced reader; a ninth fall-through key refused.
Decided in building:

- **Entry and exit groups.** Clauses of one entry or exit list that read one another (a clause
  source), and an override with the clause it overrides, run as one group on the union of their
  `state.<col>` keys, so a step never runs a reader whose source it skipped (which would hold it
  silently). Creation, `Rebind` and `Close` run their lists whole. A skipped clause answers
  `NotRun` and is not recorded in provenance.
- **At most eight fall-through keys per table**, since a miss may try 2^n - 1 combinations; the
  set is a fixed array, so a lookup allocates nothing. A combination that blanks a key already
  blank repeats an earlier lookup and is skipped. A load requires the run with every
  fall-through key blank for each tuple of the other keys (the catch-all); a run keyed on values
  is in the domain when that all-blank twin is. `clause_answer::fellThrough` stays a bool (any
  retry hit), and the keys keep the values as read.
- **Overrides**, `overrides: <n>` / `add_clause::overrides`:
  - The override keys on the state its base read: a step's state sources read next, so the
    base's sets wait until the override has run, then write the columns the override did not
    (without that, `where: {mode: state.mode}` would read the base's own write).
  - It runs only when the base answered; a base that held stays held, with nothing to coalesce
    onto. An override that itself holds (a failed script, a NaN) is reported as any clause is,
    and the base's answer stands.
  - A miss answers the new `clause_outcome::Missed`: no write, no emit, no report, no hold.
  - It may emit only where its base emits, and neither may be an or table (an or pick answers
    every key, so it has no miss, and no one row to replace).
  - "May hold for want of a run" is read from the base's sources: a key other than a
    fall-through key, read from a machine input, an int or double state column or an or clause,
    closes nothing and is refused.
  - The coalesced answer is by column name: a dependent reading `clause.<base>.<col>` reads the
    override's `<col>` where it hit and its table has one (the same type, or refused). Reading
    the override directly is refused. `Prepare` closes the dependent's key over both tables'
    values.
  - `Prepare` cuts an override's domain to the tuples its rows have a run for and proves each as
    any best or weighted clause's run is proved.
  - "Need not be total" is the machine's rule, not an exemption from the table's own load
    check: a partial override table declares its keys `domain: rows`, so its domain is the
    keys it has. Declared over `members:`, it must have a run for every member, as any best
    table must (`OverrideClause.ANonTotalOverrideTableNeedsARowsDomain`).
  - A hit with no emit column keeps the base's command: the column-subset rule replaces only
    what the override declares, so the base's command goes out with the override's state
    (`OverrideClause.AHitWithNoEmitKeepsTheBasesCommand`). Whether a hit should instead
    silence the base's emit is an owner question (Open questions).
  - Provenance records the base as `Answered` with its own row, before the override's
    replacement; the override's entry, the next one, shows what replaced it.
- **The override and its base give way level by level (owner, 2026-10-20, after the
  2026-10-20 review).** As first built, the override ran all its own retries before its base
  answered, so a substate's row for no event beat a superstate's row for the event: asleep,
  `wake` snored on. That is the opposite of the retry order decided above, and the one-table
  and two-table forms agreed only with the event key before the substate key. Now a level is
  one blanking of the base's fall-through keys, in their retry order, and at each the override
  is tried first, then the base: the more specific row wins, the override a tie. The override
  falls through on each of the base's fall-through keys, read from the same source (refused
  otherwise), and its own fall-through keys give way first, inside each level. An override run
  less specific than the base's answers `Missed`. In the step, the override's retry order is its
  own fall-through keys, then the base's in the base's order, so its retry count shifted right
  by its own keys' number is comparable with the base's (`resolved_clause::ownFallThrough`).
  Tests: `ManySurfaces.OneTableAndTwoTablesWithAnOverrideRunTheSame` (now with keys
  (super, sub, kind)), `ManySurfaces.ASuperstatesRowForTheEventBeatsASubstatesRowForNone`,
  `OverrideClause.ItFallsThroughOnItsBasesFallThroughKeys`.
- **Not built: substate membership (the pair-table domain).** The rule above says it is
  "ordinary key-domain machinery", but it is not: a foreign-column domain gives one column's
  values, and a (super, sub) pair is a joint domain over two keys, which nothing has; and on a
  fall-through key a domain filters runs rather than generating tuples. That is a core change,
  not a front end, so it waits for the owner (Open questions).

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
- **A payload getter answers outside a dispatch too** (added 2026-10-19, from the Part 3
  review). `CreateInstance`, `Rebind` and `Close` read every input through its getter when no
  message is in flight, so a handler's getter returns a neutral value then (`ny` 0, an event
  input's blank 0) and must not fail. A failure would hold those lists' clauses and spend the
  input's report-once bit, hiding a real failure during a later dispatch.
- **The host resets a handler's payload and event inputs after the dispatch step** (added
  2026-10-19). "Held for one step" is the host's convention; Tabular reads what the getter
  answers. An event input left set fires its run again next step, and a `Rebind` or `Close`
  exit sees it; a test pins that.
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

**A gap, deferred (owner, 2026-10-19: script conditions are enough for gambits for now; maybe
revisited):** a gambit over host wires costs a closure call per tried row, because
conditions are scripts. A flags column declared as a plain condition (spelling to settle, e.g.
`rank: false`) would make a row count only when its flags are all in the input, `R & F == R`,
and leave the order to the rows. That gives first-match over wires with no script. The
contract already lists "flags subset" among condition kinds, but only the ranking column is
built.

## Open questions

None remain from the craft bench. Messages, the command runner and hierarchy are decided as
above; `:lock` is marked speculative.

- **Substate membership (for the owner).** Part 4's `(rest, walk)` check needs a domain of
  (super, sub) pairs read jointly from another table. A candidate, kept small: a fall-through
  key may also name a pair table, `fallthrough: {table: substates, with: {super: super}}`, and a
  run keyed on a pair the table lacks becomes an orphan, reported by `Load` and `Prepare` (the
  blank stays allowed). It would make the pair table a domain source, with the dependent
  re-checks that brings.
- **An override hit with no emit (for the owner).** Built: the column-subset rule, so the base's
  command goes out with the override's state (Part 4, As built). The alternative is that any
  hit silences the base's emit, so an override that changes only state also drops the base's command
  (an override wanting the base's command would then have to repeat it). Unchanged until
  decided.
- **Edges on a handler's payload inputs (for the owner).** An edge compares an input with its
  previous value, the last one read. For a handler's payload that is the previous *message's*
  value (or the neutral value a getter answered at `CreateInstance` or between dispatches), not
  the previous tick's. Whether such an edge means anything, and whether it should be refused,
  is undecided. Built behaviour is unchanged: it is allowed, and crosses from the last value
  read.
