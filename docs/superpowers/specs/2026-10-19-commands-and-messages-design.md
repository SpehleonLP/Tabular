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

There are three parts, built in this order: CSV quoting, commands as lists, then messages out
of Tabular. Part 2 depends on Part 1 reading cells correctly.

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
- **Every top-level item is a list whose head names what it does** (`(clip HeadDown)`,
  `(message this calm)`, `(sound no__)`). This retires the animation bridge convention of
  telling messages from clips by quoting (see Bridge tables).
- **A cell written without parentheses is wrapped, not refused** (owner, 2026-10-19): its
  meaning is clear, the author just didn't write it lispy. Outside an open parenthesis, a
  suffix cell whose trimmed text does not start with `(` is one command, and Tabular wraps the
  whole cell: `clip HeadUp` reads as `(clip HeadUp)`. So the joining rule above applies inside
  parentheses, where a command may run across cells. A cell that starts with `(` holds one or
  more s-expressions. *Open:* a cell holding a single word (`HeadDown`), which has no verb.
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
give a three-item list; an empty suffix gives `()`; `clip HeadUp` reads as `(clip HeadUp)`; an
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
from, phase, mode, to,    steps
_,    _,     _,    eat,   (clip HeadDown)
eat,  _,     _,    chew,  clip HeadUp,     (message this "chewed")
idle, _,     _,    walk,  (clip idle_to_walk)
```

- The animation system is the consumer. Its timing idioms ("let X finish", "hard cut to Y",
  crossfade when no row matches) are how it interprets the list, not Tabular's business.
- A clip and a message differ by their head, not by quoting.
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

## Open questions

These came out of the craft bench. Each needs an owner ruling before Part 3 is finished.

1. **A command that causes a message.** In the bench, `(basket-add)` can fill the basket, and
   the bench should then move to the confirm modal. Commands go out after the swap, so the step
   that added the item saw the basket before the add. Something has to step the machine again
   (`basket_filled`). Does such a message run in the same tick, after the current one, or wait
   for the next tick?
2. **Leaving from any state.** Tabular has no "any state" key. Closing the bench applies from
   every (page, modal) pair. Either each pair gets a row (about 12), or the host ends the
   instance and its exit list runs. Recommendation: the host ends it, because closing is not a
   transition.
3. **Entry per machine or per column.** Entry runs the whole list when any hash column
   changes. In a two-layer UI (page and modal), a modal-only change re-runs the page's entry
   rows. The options are entry clauses that run only when their own column changes, or one
   machine per layer.
4. **Flag ranking against authored priority.** On Back, the bench closes the sort panel first,
   then backs out of the basket, then leaves. With `sort_open` and `can_back` both set, check
   that the flag ranking (full match, then `R & F`, then `R`) gives that order. If it doesn't,
   the host sets `can_back` only while sort is closed.
