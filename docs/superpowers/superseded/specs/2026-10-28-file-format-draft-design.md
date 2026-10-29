# Tabular file format: draft

Status: DRAFT (owner, 2026-10-28). Not approved, not implemented.

This is a new **authoring interface** for what Tabular already does. The semantics are the
ones `Spehleon/Tabular/README.md` describes: best picks in row order (gambits), fall-through
keys and overrides for hierarchy, clause lists that read earlier clauses, entry and exit per
changed column, weighted draws. A file in this format compiles to the same `create_table`
and `create_machine` declarations and the same rows as the YAML+CSV bundle it replaces. Where
something here would need a new semantic, it is listed under "Not interface" and not
designed around.

Worked example: `Lifaundi/Development/Grazer/grazer.draft.tabular`, which is
`grazer.tabular` rewritten in this format with the same compiled machine.

## The rule

**You can read any table without scrolling away from it.** What changes how a table's rows
are read is in the table's own section. The top of the file says where names come from.

## Kept (owner, 2026-10-28)

- **The command cell is a trailing suffix.** It is the last column and takes the rest of
  the row, commas included, so commands line up.
- **AngelScript snippets are inline.** One function hot-reloads without reasoning about
  suspended contexts, default values or data layout. A whole `.as` file does not.

## What the bundle costs today

Measured on `grazer.tabular`'s `grazer_brain`, 50 rows:

| Cost | Where it comes from |
|---|---|
| one 50-row table deciding everything | many decisions in one table; smaller tables make the per-row keys cheap |
| `-100..1000` on 39 of 50 rows | a band cell can't be blank, though "the input's whole range" is the usual answer |
| a death row in each of 8 modes | a row every mode needs must be copied into each mode, ahead of its own rows |
| a hold row in each of 9 modes, 3 "the dead hear nothing" rows | totality needs a row per key tuple, and that row is nearly always "keep the state" |
| `hp` declared three times | once as a gauge, once as a machine input, once as a table input |
| `clause.0.idle`, `where: {mode: state.mode}` | every binding is spelled out, even when one name can only mean one thing |
| YAML around the CSV | the configuration and the rows are two languages |

Each row of this table is one interface change below. None changes what compiles.

## File

Named sections of comma-separated rows, `#` comments. A `command` or `body` cell is always
last and takes the rest of the row; a `body` may continue on indented lines.

**A bracket line opens a section and sets how the reader reads it.** It is an s-expression
in square brackets, in the same syntax as a command cell: a head word, a name, then
optional `(option value...)` lists. Everything after it, up to the next bracket line, is
plain CSV: the header row, then the rows. A bracket line only opens a section; nothing
changes how rows read partway through one.

    [table grazer_brain]
    [table pour_chemical (domain count 0..10)]
    [table pick_target (pick or) (types level=band)]

A long bracket line may continue on indented lines, one option per line, up to its closing
`]`.

Options are lists rather than `:keyword value`, so each one is whole on its own: it is
never a question where a keyword's value ends.

| Section | Holds |
|---|---|
| `[class <Name>]` | the script class the machines run on |
| `[gauges]` | every input, once: where it comes from, its type and range |
| `[messages]` | which messages wake the machines, and how they are delivered |
| `[states <machine>]` | a machine's state columns |
| `[machine <name>]` | a machine's clauses |
| `[table <name>]` | a table: a header row, then rows |
| `[grid <name>]` | a grid: a header row of column keys, then one line per row key |
| `[fragment <table> ...]` | more rows of a table, with only the columns they use |

**One syntax, several file kinds.** An object's behaviour is `<name>.tabular`; its animation
graph is `<name>.anim_pipeline`. Both use this format and compile to Tabular tables resolved
by Tabular; they are separate files because each reloads on its own. A pipeline file adds
its own sections (slots, bone masks, descriptor sets) beside its tables. What crosses
between the two is the pipeline's outputs (phase flags, a slot's current label), read by
the behaviour file as host gauges, and the descriptor fields the behaviour writes.

**Ambient tables.** A table or grid need not be read by any machine. A file may hold tables
that just exist: data an object's script will read (a lookup, a list of variants), kept in
Tabular because it reloads as a record and AngelScript data does not yet. How a script
reads one is not this format's question; until that is designed, the port's header says
which script reads it and how.

### `[gauges]`

    name, source, type, range, body

`source` is `eager`, `lazy` (read only when a row's condition reaches it, at most once per
step), `host` (filled by the host, such as `event`), or `message <Name>` (an eager gauge
read from that message's payload). A gauge is declared once. A machine's inputs are the
gauges its clauses read, and a table's inputs are the gauges its band columns name. Both
are derived, with the type and range given here.

What a gauge is (the host interface) is not settled. This section carries whatever the host
design decides.

### `[messages]`

    message, from, p1, p1 type, p2, p2 type

Every message the machines take, with its signature: `from` is the sender's type, `p1` and
`p2` name the payload and give their types. A blank cell is a slot the message doesn't
carry. The Mailbox needs the types to route the message here at all.

There is no delivery column. If the script also handles the message, both run; if it
doesn't, only the machines do. That falls out of what each side declares.

### `[states <machine>]`

    state, type, initial, members

`members` is the last cell and, like `body`, may continue on indented lines, with `#`
comments between them, so a long list is grouped rather than written on one line:

    mode, hash, startup,
        startup
        # on the ground
        stance walk turn_around duck_stance duck_walk
        # in the air
        jump double_jump high_jump falling hit_ground

### `[machine <name>]`

    list, table, set, bind

`list` is `create`, `entry`, `step` or `exit`; clauses run in row order within a list.

- **Binding by name.** Each key and each band input of the table binds to the one name it
  can mean: a state column, a gauge, or a value column of an earlier clause in the same list.
  Exactly one candidate binds silently. Two candidates is refused until `bind` says which,
  and so is none. `bind` takes `key=source` pairs (`idle=grazer_brain.idle`,
  `mode=state.mode`, `event=input.event`, or a literal). Earlier clauses are named by table,
  not by position; this compiles to `clause.<n>.<col>`.
- `set` takes `state=column` pairs. It is never inferred, because it writes state.
- A table with a `command` column emits it.
- `overrides <table>` in `bind` marks an override clause.

### `[table <name>]`

**The header row is the table's declaration.** Everything that changes how the rows read is
in it:

- **`>name` is a key**, and `>name?` a key that falls through. Fall-through order is the
  order the keys appear, left to right.
- **`name = value` gives the column's default**, used for a blank cell. `value` may name a
  key, meaning that row's value of the key (`next = mode`: keep the state).
- The pick is `best`, or `weighted` when there is a `weight` column.

Only what the header can't say is an option in the bracket, and most tables have none:

| Option | Meaning |
|---|---|
| `(pick or <col>)`, `(pick all)`, `(pick none)` | a pick that isn't `best` or `weighted` (see "Stored tables") |
| `(domain mode a b c)`, `(domain mode table.column)` | a key's domain, where it isn't `rows` |
| `(types col=type ...)` | a column's type, where inference is wrong |

**Types are inferred** from the cells and the gauges: a column named for a gauge with a
range, whose cells are `a..b`, is a band on that gauge; `when` is the condition; `weight` is
the weight; `command` is the suffix; all-integer cells are `int`; the rest are `hash`.

**A blank band cell is the gauge's whole range.** A blank key cell is the blank that
fall-through retries with; a table with a fall-through key needs the row with every
fall-through key blank (`README.md` "fall-through"). Fall-through happens only when no row has the key values at all
(`README.md` "fall-through"): if rows with those keys exist and none holds, nothing falls
through, and the table is not total. So every group of conditional rows ends in a row that
always holds, usually a hold. A blank cell in any other column takes the column's default; with
no default it is refused, as today.

### `*` in a key cell

A `*` key cell matches every value of that key: the state's members, or the values the table
names elsewhere for that key. The row is copied once per value, **at the place it is
written**, so file order decides as it does for every other row:

```
>mode?, >event?,  hp,        next = mode
*,      OnTimer,  -100..0.5, dying     # at the top: the death row beats every mode's tick
dying,  *,        ,                    # the dead hear nothing
...
*,      ,         ,                    # at the bottom: every mode holds on what it doesn't claim
```

A copy that can never be reached, because an unconditional row with the same keys is above
it, is dropped instead of refused. That is what lets the last line above be a fallback.

Keys are written on every row. The rows compile one for one, in file order.

### Flags, ranking, edges and cyclic inputs

These come from animation (`README.md` "Picks", "Pivoted loads", "Edges").

- **A flags gauge** has type `flags`, and its `range` cell lists its flag names in bit
  order: `mode, host, flags, Crouching InAir Drunk, ...`. A flags cell is written with
  those names, `A|B`, or `None` for 0. The format translates the names to bits through the
  gauge's list; what loads is the decimal the CSV reader takes.
- **The ranking column** of a best table is the column named for a flags gauge, as a band
  column is named for a ranged gauge. Rows rank as `README.md` says: all flags present
  first, then the larger `R & F`, then the larger `R`, then row order; the `None` row last.
- **An edge band** is a band column written `^name` in the header: it holds on crossing
  into the band since the gauge's previous value, not on being inside it. A blank cell is
  "no edge".
- **A cyclic gauge** writes `cyclic` after its range: `phase, host, double, 0..1 cyclic, ...`.
  A pick wraps it into the range, and an edge crosses the shorter way around
  (`include/tabular/relation.h`, the input's `cyclic`; the README doesn't describe it yet).

### Pivots

A pivoted table is an ordinary table laid out as a grid: its keys down the side, one flags
column's names across the top, and one other column in the cells. `(pivot <flags> <cells>)`
in the bracket says which two:

    [table locomotion (pivot mode clip) (types clip=string)]
    >label,  None,           Crouching,       InAir
    Idle,    idle__standing, idle__crouching,
    Walk,    walk_2d,        crouch_walk_2d,
    Jump,    jump__idle,     ,                jump_takeoff_1d

- The header's first cells are the keys (`>label`); every later header cell is a flag name
  of `<flags>`, and `A|B` is several. `None` is 0 and only a best table's first header.
- Each non-blank cell is one row: its keys, that header's flags, and the cell as `<cells>`.
  A blank cell is no row. A best pivot is still total: every key needs its `None` cell (the
  `R == 0` default), or the load is refused with "no row with no flags". An `or` pivot needs
  none, since no holding row answers 0.
- In a best table `<flags>` is named for its flags gauge, so it is the ranking column. In
  an `or` table it is the output column, named in `(pick or <col>)`:

      [table footfalls (pick or bits) (pivot bits phase)]
      >clip,    LFootDown,  RFootDown,  Footstep
      walk__f,  0.10..0.15, 0.60..0.65,
      walk__f,  ,           ,           0.12..0.13

  The cells column is named for the gauge it bands, so its cells are bands on `phase`.

### Stored tables

A table no clause picks from, which the host reads or writes, says so in the bracket:

| Option | Meaning |
|---|---|
| `(pick none)` | no pick: a stored relation the host reads by key |
| `(pick or <col>)` | OR of the flags column `<col>` over the holding rows |
| `(order a b)` | the row order, where it isn't the keys (a picked table's keys lead it) |
| `(unique)` | a duplicate order key is refused, not overwritten |
| `(mutable)` | the host inserts rows at run time (`read_only: false`); only a best or unpicked table |

`String` columns are never inferred (a name reads as a hash); `(types clip=string)` states
one. `Vec2` is how a band is stored, and nothing else needs it.

### `[fragment <table>]`

A fragment adds rows to a table that is declared elsewhere in the same file, using only
the columns those rows need. The table's own section declares every column, its keys and
its defaults once, and it may have rows of its own. Each fragment then has a narrow header
and its rows. A sparse table can therefore be written as dense blocks, each with a comment
saying what it is:

    [table target_brain]
    >mode, >event?, >returned?, status = None, ^x, ^circle, on_surf, when, next = mode, command

    # The ground family. The state hook ranks first.
    [fragment target_brain (mode stance walk duck_stance duck_walk)]
    status,    ^x,   when,          next,       command
    HookWheel, 1..2, can_jump_flip, wheel_flip, (hook_exit)
    HookFlip,  1..2, can_jump,      high_jump,  (hook_exit)

    # Standing states test sliding before falling.
    [fragment target_brain (mode stance walk)]
    when,         next,       command
    should_slide, slide_down, (hook_exit)
    should_fall,  falling,    (hook_exit)

    # Ducking states test them the other way round.
    [fragment target_brain (mode duck_stance duck_walk)]
    when,         next,       command
    should_fall,  falling,    (hook_exit)
    should_slide, slide_down, (hook_exit)

- **A column the fragment leaves out is blank in its rows.** A blank key is the fall-through
  blank, a blank band is the whole range, a blank `when` is no condition, and a blank value
  takes the table's default. A fragment's header names only columns the table declares, and
  it gives no defaults: those are said once, in the table.
- **A constant in the bracket sets a column on every row.** `(event swim)` fixes the event.
  `(mode a b c)` copies each row once per value, in the order listed, and `(mode *)` copies
  it once per member. A column given as a constant is not also in the fragment's header.
- **File order is priority.** The compiled table is the table's own rows, then each
  fragment's rows, in the order the sections appear. For any key tuple, a pick tries a
  fragment's rows before the rows of any fragment below it. A fragment's position is
  therefore where its rows sit among a key's other rows: shared rows above a state's own
  rows, or below them.
- **Only the whole table is checked.** Totality and dead rows are proved on the compiled
  table, so a fragment's rows need not end in a hold.

Fragments only regroup rows. A fragment must be in the same file as its table, after it, so
a reload rebuilds the table with them. It compiles to the same rows in the same order, so it
is an interface change only.

### `[grid <name>]`

Tabular's grids (`README.md` "Grids") already exist: a dense 2D array keyed by `Hash` or
`Int` on each axis, every cell filled, one cell type (`String` or `Command`), loaded whole
by `LoadGridCsv` and read by the host through `GridVersion`. This section is only how a file
says "this is a grid".

    [grid craft_hint (columns modal)]
    page,     none,            numeric,       check_finish
    book,     (hint browse),   (hint amount), (hint confirm)
    details,  (hint describe), (hint amount), (hint confirm)
    fill,     (hint fill),     (hint amount), (hint confirm)

- The header's first cell names the row axis (`page`); the bracket's `(columns <name>)`
  names the column axis, whose values are the rest of the header.
- Each line's first cell is its row key; the other cells are that row's cells, one per
  column. Every cell is required.
- Types are inferred as for tables: all-integer keys are `Int`, the rest `Hash`. A grid with
  any cell starting with `(` is `Command`; otherwise `String`. `(cells <type>)` states it, as does `(types page=int)` for an axis.

A grid has no pick, keys, defaults or row order. It is not a clause's table.

## What a port leaves to script

Tabular is deliberately not a general-purpose language. Its tables are proved total, with
no dead rows, when they load. Anything whose expression in Tabular would make it possible to
write a program Rice's theorem applies to (a loop, waiting and then deciding again, reading
what a command just did, computing a string or a handle) is not added: a row emits a command
and the AngelScript class does it. A port expresses the **table-shaped part** of an object,
which is most of it, and lists the rest in its header as what stays in script. Those parts
are the design, not gaps:

- **Waits.** A handler that waits (`wait`, `over`, a `loop`) is the script's. When the
  decision after the wait is table-shaped, the row emits `(state x)` and a row keyed on
  `returned` decides when it comes back. A machine never hears its own messages
  (`README.md` "Messages are the host's"), so a self-message is not a way back.
- **Reads after a command.** Every input is read once per step. A decision that needs what
  a command just did is either the script's or the next step's.
- **Strings, handles, computed values.** Rows choose among values; they don't compute them.

Ports aim at the same result as their source, not the same order: like a compiler, a port
may reorder and restructure.

## Not interface (open semantic questions)

Table-shaped things Tabular can't yet hold:

1. **Several agents in one file.** A pod and its screens, a lift and its buttons. Needs a
   scope for `[class]`, `[gauges]`, `[messages]` and machines.
2. **A lookup made several times in one step** (the mixer's four slots). Four clauses
   on one table, with literal keys, already cover it.
3. **Two ranking conditions in one table.** An animation bridge gates on both the phase
   bits and the mode bits; a best table has one ranking column. The Synty port merges them
   into one gauge.
4. **Grid cells of any value type.** Grids hold only `String` or `Command` cells. The
   mixer's `(count, slot) -> chemical` is an ambient `Int` grid.

## Open (interface)

1. Whether `*` expanding over "the values the table names elsewhere" is clear enough or
   needs a declared domain, and whether dropping unreachable `*` copies hides mistakes.
2. Whether type inference stays, or every non-obvious column declares a type.
3. Whether binding by name is too implicit. The rule refuses every ambiguous case, but a
   reader can't see a binding without checking that nothing else has the name.
4. Defaults naming a key (`next = mode`): enough, or should holds be a bracket option?
5. Whether the YAML+CSV bundle stays accepted alongside, during migration.
6. Fragments: whether a row that a `(mode a b c)` constant copies into a key where it can
   never be reached is dropped (as `*` copies are) or refused (as written rows are).
