# Tabular

Tabular is a small relational store plus a compiler for table-driven state machines. Game data and behaviour live in typed tables, usually authored in `.tabular` files: bracket declarations with CSV rows beneath. A *machine* is a fixed query over those tables. It is proved total and conflict-free once, when it is prepared. After that, each instance steps it with no allocation, no name lookups and no generator state.

The core (`relation.cpp`) depends only on the standard library and Spehleon/lib's `Errors` and `Support`. The file reader and the CSV front end add only Tabular's own parsers. Every fallible call returns `tl::expected<T, ErrorContext>`. Refusals are values, never exceptions or aborts, and they name the table, row, line or clause at fault.

The library and test sources are listed in this directory's `CMakeLists.txt`, which the engine adds with `add_subdirectory`. Public headers are in `include/tabular/`. Everything in `src/` is internal.

---

## Layers

| Header | What it gives you |
|---|---|
| `relation.h` | Value types, schema building, `relation_store` (rows, owners, versions, lookups) |
| `grid.h` | `GridVersion`, a dense 2D keyed array stored as a table |
| `owner_handle.h` | An RAII handle that retracts an owner's rows when it is destroyed |
| `pick.h` | Interpreted picks over a table version: `PickBest`, `PickAll`, `PickOr`, `PickWeighted` |
| `draw.h` | `Draw(seed, site)`, stateless randomness |
| `document.h` | `.tabular` file reader: tables, grids, machines and rows |
| `rows_csv.h` | CSV front end for rows, pivoted rows and grids |
| `machine.h` | Machine definitions, `CheckMachine`, `Prepare` → `Machine` |
| `instance.h` | `Bind` → `Binding`, plus `instance`, `CreateInstance`, `Step`, `Rebind` and `Close` |

The usual pipeline:

```
file ──ReadDocument──► schema + create_machine[] (+ rows)
                              │
                     relation_store(schema)
CSV ──LoadCsv / LoadGridCsv──►│   (owners load and retract rows)
                              │
          Prepare(machine, store) ──► Machine      (proved against current versions)
          Bind(machine, host)     ──► Binding      (one getter per input, resolved once)
          CreateInstance(binding) ──► instance     (embedded in the host object; commands in Emitted())
          Step(instance, self, seed) ──► span<emitted>   (per tick; the commands this step emitted)
          machine.Stale(store)? → Prepare again → Bind → Rebind(instance) ──► span<emitted>
          Close(instance, self, seed) ──► span<emitted>  (the object leaves: its exit commands)
```

---

## Values

Every value is an 8-byte `cell` union (`i`, `d`, or `v[2]` floats). The column's `column_type` says which member is live:

| Type | Stored as | Notes |
|---|---|---|
| `Hash` | `i`, a CityHash32 widened without sign | Can be a key. In CSV, the text is hashed. Hash 0 and collisions within one load are refused. |
| `Flags` | `i`, 32 bits | Can be a key. A flags column with an `input` is the ranking column. |
| `Int` | `i` | Can be a key |
| `Double` | `d` | Never a key, because NaN and -0.0 break equality |
| `Vec2` | `v[2]` | Written `a..b` in CSV |
| `Band` | `v[2]`, half-open `[lo, hi)` | A condition on a declared input. An *edge* band fires on crossing into the band, not on being inside it. |
| `Condition` | host closure | At most one per table. Called with the object under test. It may fail, and a failure is never read as false. |
| `Command` | text | One s-expression list of commands, `((anim chew) (message food "eat"))`, or `()` for none. Only its shape is checked; the host interprets it. |
| `String` | interned `counted_string` | Can be a key, through `Find(text)`. Cannot be in an order. |

Every 32-bit value (hash or flags) is range-checked to `[0, 2^32)` on every path in.

---

## The relation store

`BuildSchema(span<create_table>)` validates the table declarations and returns an immutable `schema`. Tables are numbered by their position in the list. `relation_store(schema&&)` holds the live rows.

### Ownership

Every row records the `uint64_t` owner that contributed it. Reloading a contributor means retracting its owner and writing its rows again.

- **Mutable tables** take `Insert(insert_row)`, one row at a time.
- **Read-only tables** take only `Load(load_rows)`, which atomically replaces every row that owner has in that table.
- **Grids** take `LoadGrid(load_grid)`, which replaces the whole grid.
- `Retract(retract_owner)` removes an owner's rows from *every* table. It is never refused.
- `owner_handle` runs that retract from its destructor. It exists for crash-style cleanup, not for reloads. It is move-only, and there should be one handle per owner.

Each command adopts the closures it is given whether it succeeds or not. A refused command releases them, so the caller never has to.

### Versions

A versioned table is immutable once built and refcounted. Versioned tables are read-only tables, mutable `best` tables, and mutable tables that some best table's key domain reads from. Every change builds a whole new version, checks it, and only then swaps it in. A refusal leaves the current version untouched. `Version(table)` gives a reader a `counted_ptr<TableVersion const>` whose row numbers stay valid as long as it is held. `Generation(table)` changes exactly when the rows change.

Within equal order keys, a versioned table sorts rows by owner id and then by position within that owner's load. The result therefore does not depend on which owner wrote last.

### Ordering and lookups

- `(order cols.. [unique])` sets the row order. A unique order refuses a duplicate key instead of overwriting it, so two contributors claiming one key becomes a visible error.
- `Find(table, keyColumn, value | text)` is an index lookup on key columns. It is not a scan.
- `FindOrdered(table, prefix)` binary-searches the leading order columns.

The store is single-threaded. A `Load` or `Retract` that races with a `Version()` call from another thread is a data race.

---

## Picks: how a table answers a query

A table can declare `inputs`, which are named `Double` or `Flags` slots that bands and the ranking column read. It can also declare a `pick` policy. A picked table's key columns must be the leading columns of its order. A `pick_query` supplies key values, which select the *key run*, plus the input values in declaration order. The caller resolves names before calling; Tabular does not look anything up at pick time.

A row *holds* when every band contains its input, every edge band was crossed since the previous value, and the row's script condition returns true. Ranged inputs are clamped to `[lo, hi)`. A NaN in a ranged input, or a flags input wider than 32 bits, refuses the pick.

| Policy | Answer | Notes |
|---|---|---|
| `best` | One row | Ranked like animation's `LookupBest`: rows whose flags are all in the input first, then the larger `R & F`, then the larger `R`. Ties go to row order. A row that matches no input flag is skipped. The `R == 0` default is tried last. With no ranking column, rows are tried in row order. **May be mutable.** |
| `all` | Every holding row, in row order | Read-only. Not usable from a machine. |
| `or` | OR of the `output` flags column over holding rows | 0 when no row holds, so no default row is needed. Read-only. |
| `weighted` | One row drawn in proportion to a `Double` weight column | No inputs and no conditions. Uses `Draw(seed, site)`. Read-only. |

### Totality: what a best table guarantees

Each key of a `best` table must declare a **domain**: the key values the table promises to answer. A domain is one of:

- `members`: a literal list.
- `{table, column}`: the distinct values another table's column currently holds. Domains that depend on each other in a cycle are refused.
- `rows`: the key tuples the table's own rows name.
- **fall-through** (`>name?` in the header, in place of a domain): an open hash key. A value no run is keyed on falls through to the blank run (key 0). Up to eight per table; a miss retries with fall-through keys blanked in key order, the first fall-through key being the lowest bit, so `(sub, kind)` tries `(0, kind)`, then `(sub, 0)`, then `(0, 0)`. The run with every fall-through key blank is required.

A change that brings rows is refused unless the version it would publish meets all of these conditions:

1. **Total.** Every key tuple in the domain has a run that answers every input. Bands must cover the input's whole range for every flag group, and a row with a script condition or an edge doesn't count toward that cover. Best tables therefore require every band input to be `ranged`.
2. **No dead rows.** No row is fully shadowed by an earlier unconditional row, so every row is reachable.
3. **No overlap between owners.** Rows from two owners with equal flags never overlap.

The same must hold for every best table whose domain is read from the changed table, checked against its new domain.

A bare `Retract`, or a `Load` of zero rows, is never refused. Afterwards, the optional `run_note` report lists every run that is now not total.

A run whose key lies outside the domain is an **orphan**. It is reported but never checked.

On refusal, `load_refusal` names the offending row of the load. It can also name the earlier row it clashes with, or the row that covers a dead row.

The proof is computed on a flat **run layout** (`src/layout.h`). Each run is grouped by flag set, and each group's input space is cut at every band edge into cells. Each cell lists the rows a pick would try there, ending at the first unconditional row. A machine keeps this layout as its compiled pick.

### Grids

A grid (`[grid g (rows r type) (columns c type) (cells type)]`) is a dense 2D array keyed by `Hash` or `Int` on each axis. Every cell must be filled. Its cells hold one value type, `String` or `Command`. It is loaded whole by one owner and read through `GridVersion` with `RowOf`, `ColumnOf` and `At`.

---

## Front ends

### The `.tabular` file (`document.h`)

`ReadDocument(text, label)` reads a file of bracket declarations; `ReadSchema` is the same, except that it refuses a file that declares machines or carries rows. A declaration is an s-expression in `[...]`, and the CSV lines beneath it are its rows. An unknown option fails the load, so a typo such as `uniqe` is never silently ignored. Every table a file declares is read-only: a reload loads a new version. A table that scripts insert into is built in C++ with `create_table` and `BuildSchema`.

```
[table grazer_brain
  (input energy double 0..1)
  (domain mode idle walk)
  (order mode)]
>mode, energy: band, next
idle,0..1,walk
walk,0..1,idle

[machine grazer
  (state mode hash (members idle walk) (initial idle))
  (input energy double 0..1)]
list,from,where,given,set,emit,overrides
step,grazer_brain,mode=state.mode,energy=input.energy,mode=next,,
```

A table's first body row is its **header**, and each cell declares one column: `>name` is a key, `>name?` a fall-through key, `^name` a band edge, `name: type` a typed column (a bare name is a hash), and `name = value` a column with a default (see Shorthand). `when` is a condition, `weight` a double, and `command` a command; a command cell that comes last is the suffix, every cell from it on read as one list of commands. A band, and a flags column that ranks, are named for the input they read. A table with a `weight` column and no `(pick ..)` is `weighted`.

With no `(pick ..)` a table is `best`. The bracket takes `(input name type [lo..hi] [cyclic])`, `(pick none|best|all)`, `(pick or col)` or `(pick weighted col)`, `(order cols.. [unique])`, `(domain key ..)` and `(pivot headers cells[: type])`. A pivot's header row is its keys, then its flag headers; the bracket declares its headers flags column and its cells column. A grid is `[grid g (rows r type) (columns c type) (cells type)]`. A machine's body is a CSV with the header `list,from,where,given,set,emit,overrides`; `where`, `given` and `set` cells are space-separated `name=source` pairs, and a non-blank `overrides` is the position of the clause it overrides in the same list.

Column types are `hash`, `flags`, `int`, `double`, `condition`, `command`, `vec2`, `string` and `band`.

A best table's key has its rows as its domain unless `(domain key a b ..)` lists its members or `(domain key table.column)` reads another table's column. A fall-through key's domain is open, so a `(domain ..)` for it is refused, and so is a fall-through key on a table that isn't `best`.

#### Shorthand

Shorthand is only an interface: the reader compiles it into the rows the explicit form could have written, and Tabular sees nothing else. A refusal in a compiled row names the line the author wrote.

- **A default.** A blank cell takes its column's default. A default naming one of the table's keys takes that row's key, so `next = mode` holds the state:
  ```
  >mode, next = mode, command      compiles to   mode,next,command
  idle, ,  (stand)                               idle,idle,(stand)
  walk, idle, (stop)                             walk,idle,(stop)
  ```
  A default on the last command column applies only to a row whose command cells are all blank.
- **A blank band** is its input's whole range; with `(input hp double -100..1000)`, a blank `hp` cell compiles to `-100..1000`. A blank band whose input has no range is refused, never filled with `0..0`. A blank edge is still no edge, and any other blank is left for the loader, as written.
- **`*` in a key cell** copies the row once per value of that key, where it is written: the key's `(domain ..)` members, or else the values the table's rows name for it, in the order they first appear. Two `*` give the product, the first key varying slowest. Defaults apply after expansion, so `next = mode` on a copy for `walk` is `walk`. A `*` row at the top beats each key's own rows. In a best table, a copy is dropped where an earlier row with the same keys and ranking flags already answers every input (no condition, no edge, every band the whole range), so a `*` row at the bottom yields to a key's own unconditional row and is the fallback of a key whose own rows are conditional. Rows written out are never dropped. A `*` on a key whose domain is another table's column is refused; list the members instead.
  ```
  [table t (domain mode idle walk) (order mode)]
  >mode, next = mode               compiles to   mode,next
  walk, idle                                     walk,idle
  *,                                             idle,idle
  ```
  (walk's copy of the `*` row is dropped: walk's own row answers first.)
- **`[fragment <table> (col v ..)..]`** adds rows to a table declared above it in the same file, after the table's own rows and earlier fragments; `*` and dropping run over that whole order. Its header names the table's columns, bare. A bracket constant `(col v)` sets a column on every row, `(col a b ..)` copies each row per value in that order, and `(key *)` copies it per key value as a `*` cell does. Columns it leaves out are blank, so the table's defaults and blank bands apply. A fragment of a grid or a pivot, of a table below it, or naming a column the table lacks is refused.
  ```
  [table brain (input hp double 0..1) (order mode)]
  >mode, hp: band, command
  [fragment brain (mode idle walk)]    compiles to   mode,hp,command
  command                                            idle,0..1,(look)
  (look)                                             walk,0..1,(look)
  ```

Machine sources are written as `state.<col>`, `input.<name>`, `clause.<n>.<col>` (an earlier clause in the same list) or `(literal <text>)`.

### CSV (`rows_csv.h`)

Tables are authored in a spreadsheet, so all three loads share one RFC 4180 reader: a cell holding a comma, a double quote or a line break is quoted, with `""` for a quote inside, exactly as a spreadsheet saves it. `(message food "eat")` is saved as `"(message food ""eat"")"`.

**`LoadCsv(store, csv_rows)`** reads a header row that names every column once, in any order. It replaces the owner's rows atomically. Its cell rules:

- Cells are trimmed. `#` lines, blank lines, rows of nothing but separators (a spreadsheet's empty row) and a leading BOM are skipped. Trailing empty cells are ignored; a non-empty cell past the header is refused.
- A hash cell is the CityHash32 of its text, matching a script literal of the same name.
- A flags cell is decimal or `0x` hex.
- A band or vec2 cell is written `a..b`. A blank cell in an edge column means "no edge".
- A condition cell is passed to the caller's `condition` compiler, unless it is blank, which means "no condition".
- Every other blank cell is refused.
- A command cell, declared or suffix, is read as a list of commands (below).

**Command cells.** The commas between cells carry no meaning; they only lay commands out in neat columns. A suffix is every cell from its column on, joined with spaces and read as a sequence of s-expressions; a declared command column is its one cell, read the same way. Then:

- A cell of several items that doesn't start with `(` is one command written without its parentheses, and is wrapped: `message this hard-land` → `(message this hard-land)`. A single word stays a word.
- A command starting with `(` may run across cells.
- A cell of `()` (or `( )`) is the explicit "do nothing" and adds nothing; no cells at all is `()`.
- The whole is wrapped into one list. So the bridge row

  ```
  falling, hard, _, idle, message this hard-land, :lock, hard-land, :unlock, stand-from-hard-land, idle
  ```

  emits `((message this hard-land) :lock hard-land :unlock stand-from-hard-land idle)`. A clip and a message differ by their head, not by quoting.
- Refused, naming the line: a cell of several items, not starting with `(`, that isn't whole by itself (`message food (say`, `a) (b`); a string still open at a cell's end (an unquoted comma inside it); and a result that isn't exactly one list.

The builder and the store take a command column's text already in this form (`((anim chew))`) and only check its shape; they wrap nothing.

**Pivoted loads** (`pivoted = true`) write the table as a grid. Key columns go down the side and flag headers go across the top. Each header takes the next bit, `A|B` ORs headers to its left, and `None` (a best table's first header only) is 0. Each non-empty cell becomes one row. `flag_names` returns the bit → name mapping.

**`LoadGridCsv`** takes column keys from the header row and a row key from each line's first cell. Every cell is required. Command cells are read by the same rule.

---

## Machines

A `create_machine` declares the following:

- **State columns** (`Hash` with members, `Int` or `Double`), each with an initial value.
- **Inputs**, which the host supplies through getters.
- Four **clause lists**: `create`, `entry`, `step` and `exit`. A clause reads one `best`, `or` or `weighted` table. It binds every key (`where`) and every table input (`given`) to a source. It writes value columns into state (`set`), and it can emit a Command column.

Clauses can only read *earlier* clauses in the same list, so each list is a DAG by construction. Entry and exit clauses may not write a hash state column, so entering a state never re-triggers itself. No ordinary key may read an earlier clause's fall-through key, since that can answer blank.

### Messages are the host's

Tabular has no message concept. The host dispatches a message to a **handler machine**, whose inputs are that message type's payload (read by getters from the message), and steps it once per message; a message with no handler goes to the object's script. A machine that branches on argument-free events keys on a fall-through key fed by a hash input the host fills for one step. Machines chain through state, as animation does: a handler writes a state column that another machine reads. A message a machine *sends* is just a command in a row's list.

Two host rules follow:

- A payload getter returns a neutral value when no message is in flight, and never fails then. Creation, `Rebind` and `Close` read every input too, and a failure there would spend the input's once-per-instance report.
- The host resets a handler's payload and event inputs after the dispatch step. Tabular doesn't; an event left set fires its run again on every step.

An instance's own messages never come back into its own machine; the host routes a command's messages through the Mailbox.

### Hierarchy

A hierarchical state machine is an outer join, and can be written two ways that compile to the same thing:

- **One table**, with fall-through on the substate key: a substate with no row of its own falls through to the superstate's blank row.
- **Two tables**, with the `overrides` cell of the second clause set to `<n>`. The override directly follows the clause it overrides (position `n` in the same list), sets only columns that clause sets, and emits only if it emits; neither reads an `or` table. It runs only when its base answered, keyed on the state the base read. On a **hit** its sets and its emit replace the base's; with no emit column, the base's command stands. On a **miss** it writes, emits and reports nothing (outcome `Missed`), and the base answers alone. So the override table need not be total, as long as its keys keep their rows domain (a `(domain key ..)` member list is checked for totality when the table loads). Later clauses read the coalesced answer through the base; reading an override directly is refused. The two **give way level by level**: at each blanking of the base's fall-through keys the override is tried first, then the base, so the more specific row wins and the override wins a tie. Asleep, `wake` finds the superstate's `(rest, wake)` before the substate's `(rest, sleep, _)`; that is the one-table order with keys `(super, sub, kind)`. So the override falls through on each of the base's fall-through keys, from the same source, and its own fall-through keys give way first. Overrides are hierarchy within a package: a package can't see another's tables.

**Entry and exit run per column.** An entry or exit clause runs only when a state column its `where:` reads changed. Clauses that read one another, and an override with its base, run as one group. Creation, `Rebind` and `Close` run their lists whole.

### `Prepare(definition, store)`

`Prepare` first runs `CheckMachine` (names, types, arity and the rules above). Then it proves every clause against the store's *current* versions, over a domain narrowed by its sources:

- A hash state column closes the key to its members.
- An earlier best or weighted clause closes it to the values its rows can answer.
- A literal closes it to one value.
- Any other source keeps the table's declared domain.

Over that domain:

- A best clause must be total, with no dead rows and no overlap between owners.
- A weighted clause needs a run for every tuple.
- An or clause answers 0 for a tuple with no run.

Every hash a `set` can write must be a member of its state column.

The resulting `Machine` holds the versions it was proved against, so later store changes never reach it. `Machine::Stale(store)` reports whether any watched table's generation has moved. The optional report lists runs outside each clause's domain as orphans.

### `Bind(machine, host_binding)`

`host.resolve(name)` is called once per getter-fed input. It returns an `input_getter`, which is a function pointer, data and a release function. This is the only place a name meets the host.

- `host.report(instance, table, row, error)` is optional. It receives each distinct failure **once per instance**.

Commands are not handed to the host through a callback; each run returns them.

The `Binding` is immutable and shared by every instance of a host object type.

### Instances

An `instance` is composited into the host object (it is never `new`ed) and is move-only. All of its buffers are sized at creation, so `Step` allocates nothing. The one exception is the first report of a given failure, which builds its error.

**`CreateInstance(binding, id, seed, self)`** does the following:

1. Sets the initial state and reads every input once.
2. Runs `create` and then `entry`.
3. Leaves the commands from `create` and then from `entry` in `instance.Emitted()`.

Creation crosses no edges.

**Results.** Every run returns a `span<emitted const>`, each `emitted{list, table, command}`: the list the clause is in (so a host tells an exit's abort from an entry's start), its table, and the picked row's command list. The span is a view into a fixed per-instance buffer sized at `Bind` (one slot per clause that can emit), and the command text views the table version the binding holds. Both stay valid until the instance's next run. `Emitted()` returns the same span.

**`Step(instance, self, seed)`** does the following:

1. Reads every input once, from its getter.
2. Copies `current` into `next`, so a column no clause writes keeps its value.
3. Runs `step` in order. Clauses read state from `next`, so they see earlier writes, and a later write wins.
4. If any hash state column changed (A → B → A counts as no change), runs `exit` and then `entry`, only the clauses entered on a changed column. `exit` reads the state being left and `entry` reads the new state. Entry's writes land last.
5. Swaps `next` into `current`, then returns the commands from `step`, then `exit`, then `entry`, so the host sees the new state already current.

A clause that cannot answer **holds**: it writes nothing and emits nothing. `Answer(list, clause)` returns a `clause_answer` whose outcome is one of:

| Outcome | Meaning |
|---|---|
| `NotRun` | The clause's list did not run, none of an entry/exit clause's columns changed, or an override's base didn't answer |
| `Answered` | The clause picked a row and wrote its sets |
| `NoRun` | The key tuple has no run in the proved domain, even after falling through |
| `Failed` | A script condition could not answer, or an input was refused |
| `SourceHeld` | An upstream clause or input had no value |
| `Missed` | An override had no run for its key tuple, or only a less specific one than its base's, so its base answered alone. Not a hold, never reported. |

`clause_answer::fellThrough` says whether the clause's own keys had no run and a retry with fall-through keys blanked answered.

**Edges** compare against each input's *previous value*. That is the last real value read, ignoring failed reads and NaN. A crossing is spent even if the clause reading it held, and a fast input that jumps across a short band still counts as crossing it.

**`Rebind(instance, newBinding, self, seed)`** is hot reload:

1. Runs the old `exit`. Its commands come first in the result.
2. Migrates state columns by name. A value is kept if the column has the same type and, for a hash, the value is still a member. Otherwise the column resets to its new initial value; int and double are never converted into each other.
3. Runs the new `entry`. Its commands follow exit's.

`create` does not rerun. Edge history carries over for inputs that kept their name, their type and their getter. Rebind cannot be refused. The old exit's commands view the old binding's tables, so the instance holds the old binding until its next run. The host may drop its own reference right after `Rebind`, but then that next run releases the old binding, so its getters' release functions run inside that `Step`, on the stepping thread.

**`Close(instance, self, seed)`** ends the instance when the host object leaves. Closing is not a transition, so no exit row per state is needed: it runs the exit list against the current state, crossing no edge, and returns its commands, so the host hears the aborts of what the state started. The host runs them and destroys the instance. It cannot be refused and allocates nothing beyond a report's first occurrence.

**The command runner is the host's.** A returned list is a program that runs over time: zero-tick commands, clips that take seconds, host commands like `:lock`/`:unlock`. The runner chases the machine's current state; if the machine moves on mid-list, the rest is cancelled, and with no bridge row the host simply cuts. Tabular knows nothing of any of it.

**Randomness** for a weighted clause is `Draw(seed, DrawSite(machine, list, clause))`. That is a three-round pcg3d hash with no generator state. A draw is therefore reproducible across threads, independent of ordering, and stable across reloads that keep the machine's name and the clause's position. The host varies `seed` per instance and per step.

**Provenance** is a debug aid. When `TABULAR_PROVENANCE` is on (the Debug config, through the `<tabular/build_config.h>` that Tabular's `CMakeLists.txt` generates), each instance keeps a ring of its last 16 clause picks. Each entry records the keys, the inputs and the previous values that clause read. In builds with it off, the ring and `Provenance()` don't exist. An ABI tag (or MSVC's `detect_mismatch`) makes a mixed build fail to link.

### Threading

Instances that share a `Binding` may step concurrently. The host's getters, conditions and report must then be safe to call that way. The store itself is single-threaded, but a stepping instance reads only the versions its `Machine` holds, never the live store.

---

## Tests and benchmarks

- `test/` holds the GoogleTest units (`*_gtest.cpp`). They are compiled into the host binary's test build through `cmake/sources_tests.cmake`.
- `bench/` holds the budget benchmarks. `RunBudgets` measures definition build, bind, instance creation, step cost per row and per machine, and insert, retract and query on mutable and best tables. It holds each number to a hard-coded budget.
  - `tabular-budgets [--scale S]` is the optimized driver. It exits 1 when a budget is broken and 2 when a workload fails to build. `scripts/test-tabular-budgets.sh` builds it.
  - The Debug test build runs the same code at small sizes, only to keep it compiling.

## Conventions you'll see in the code

- Commands (`create_table`, `load_rows`, `create_machine`, …) are **views**, valid only during the call that consumes them.
- `PascalCase` types (`TableVersion`, `Machine`, `Binding`, `GridVersion`) are heap-allocated and intrusively refcounted. `snake_case` types (`relation_store`, `instance`, `owner_handle`) are only ever composited into something else.
- Comments cite promise and rule numbers (P12–P17, R2–R6) from `docs/definition.md`, the spec this code was built against.
