# Table catalogue and the emerging shape (2026-10-12)

This lists every table the game actually uses, what went wrong with each, and the shape of
Tabular that falls out of them. It follows the
[retrospective](2026-10-12-retrospective.md). The definition is checked against this
catalogue: every real use here must fit.

## 1. The catalogue

### 1.1 Animation

| # | Table | Keys | Value | Picks |
|---|---|---|---|---|
| A1 | Clip | animation name × kind (masc/femn/neutral) | animation address | exactly one cell |
| A2 | Variant | a | b (a subset of the a's, fed to A1), with a probability | one b, at random by weight |
| A3 | Phase | animation address × mode flag | a..b band of normalized time | flags whose band contains *t* |
| A4 | Lookup | animation name (before A2/A1) × current mode flags | animation name | most specific match, with fallback |
| A5 | Bridge | from × phase × to (…) | steps to get from `from` to `to` | the matching transition |

**A1, Clip.** Dense. It is a pivot table, but unpivoting it makes it worse, because every
row would repeat for every kind. The kind column is a dispatch dimension, so the table
implies polymorphism.

**A2, Variant.** Picks a random `a` for more variety. Conceptually this is the same as the
brain table (B1), with a probability where the brain has a condition. That implies brain
tables should be able to carry probabilities too.

**A1 + A2 form a small machine of their own.** It is a stateless database that resolves
names, fairly disjoint from the rest of animation. Tables that were meant to form one
machine did not. Static data relations and per-tick decisions are different categories.

**A3, Phase.** Sets flags that say where we are in the animation (strike, swing). Sparse
pivot. The flags across the top implicitly define an enum.

**A4, Lookup.** "If we are in some mode, which animation do we pick?" (crouching → the
crouch animation). Sparse pivot with fallback. Its rule was written as bit arithmetic:

    best = ((ref & mask) == mask) OR max(set{ref & mask})

The intent is **most specific match wins**: rows whose required flags are all set match,
and among them the most specific one wins (as in CSS specificity, or pattern matching with
fallback). Its row names share a spelling with A2/A1's names but mean something else: two
namespaces with one spelling.

**A5, Bridge.** Animation tables never update their own state vector about which state they
are in, but they can read the phase. The bridge can be rewritten as
`{from, to, condition, action}`. **It is a kind of command** (§2.1, step 4).

**Phase edges.** Animation needs events from phase flags, for example "left foot down,
rising edge → emit foot puff". An edge is a flag compared with its previous value
(`cur & ~prev`), so it needs one tick of history. Edges must be detected as **crossings
over [t_prev, t_now]**, including the wrap at a loop, not by sampling once per tick, or a
short band or a large time step skips straight over them.

**The enum knot.** The mode enum had to match the script. DRY put one definition in the
table. The cook produced it and `#pragma` pasted it into the script, so the script could not
compile until the cook had run, and nobody could see the enum in the script.

### 1.2 Grazer brain

| # | Table | Keys | Value | Picks |
|---|---|---|---|---|
| B1 | Grazer brain | state | state' | first row whose condition holds (fall through if false) |

This is not a simulated creature brain. It is a basic fall-through system, like the gambit
system in FFXII: `state, state', condition`.

**Some conditions are expensive.** A condition on speed needs speed:

    ground_speed = length(velocity × normal_force)

That is a **wire**, something Tabular needs, and it was the beginning of the end: computing
wires pulled an expression language, typing and calls into script into Tabular. One
variable can carry five conditions, so its cost has to be paid once. Animation reads the
same wires to drive blend spaces, so wires are a shared surface, not brain-specific.
**There is no sensible place in script to put them.**

**Messages.** There was also an implied table of responses to messages. It was never
thought through. Messages are discrete and carry arguments, while an animation table is
level-triggered, more like a watch.

### 1.3 Display tables

| # | Table | Rows come from | Query |
|---|---|---|---|
| D1 | Display / selection relations (displays, layers, owners, cursors, possessions, camera sources, voices) | inserted and retracted at runtime by whatever is live | outer join: which GUI with which control scheme |

These are **entirely different**: there is no authoring surface. They implicitly exist as a
small global database in the engine, and things add and remove themselves automatically.
Deciding which GUI to show with which control scheme becomes an **outer join**, which works
much better than pushing to a stack.

From the old arc (§6.2, still useful): every row carries `by`, the package or object that
owns it, and retracting `by` deletes that owner's rows. One operation then serves unload,
hot reload and crash recovery. Fallback needs no rule of its own: the less specific rows
never stopped matching. "Back out" goes to the most specific rows still live, so no history
is kept, and none is wanted, because history is a stack.

## 2. The emerging shape

### 2.1 Why tables at all

AngelScript is an update loop or a message response, and nothing in games works like that.
It is a square peg in a round hole. Nearly all game logic, about 90%, is reading the world,
running small state machines and writing to yourself. The tick:

    tables run            parallel; parallelizable script snippets run here
    update game objects
    run script engine     messages run here

| Step | May read | May write | Rules |
|---|---|---|---|
| Tables | the world (committed), its own state vector | its own state vector; emits commands and messages | Tabular's: bounded, no blocking, owner-only |
| Update game objects | — | the world | the engine's |
| Script engine | anything script may | anything script may | AngelScript's: a full Turing machine, message-driven |

The owner's first sketch had four steps ("update wires", "run HFSMs", "update script",
"commands"). Wires must be lazy (§2.3), so reading the world and running the HFSMs became one
step. That is fine: the state vector is private and nobody writes the world during it.

**Anything that needs a full Turing machine is a message to AngelScript**, which runs under
different rules in its own step. Bridges (A5) are commands. **Commands have an escape hatch:
message the real script object.**

**Commands (owner, 2026-10-12).** Commands go into lock-free queues and are consumed by their
own subsystems. Except for messages, they must commute: different objects emitting commands
cannot change the result by their order. That is all Tabular says about them.

**Messages into tables (owner, 2026-10-12).** A table can absorb a message when handling it fits
the table's rules: bounded, no blocking, writes only to self plus commands. The grazer's
collision, herd-panic, mate-request, activate, picked-up and drop-babies handlers all fit.
Messages are processed one at a time, in arrival order. Several messages to one object in one
tick are rare in practice, disjoint messages would have the same problem one step down, and
script objects already live with it (several handlers can fire in one tick). So no sorting or
combining rule. Message types no table absorbs go to the object's script.

**Sequences are bridges.** In state A, wanting B, the bridge is C, D, E: play C, then D, then
E, then start B on loop, so the object is in states C, D, E on the way. "Do this, then do
that" is already what bridging does, and it obeys the rules: the table writes only to itself.
CAOS's blocking sequences (`anim [...] over`, `wait N`, the grazer's nod, chew and death
poses) become bridges whose steps leave on clip-ended or timer-elapsed edges.

**What it buys: hot reload.** Reloading any one piece never forces a change elsewhere by
modus ponens. What happens is isolated, so each piece only has to care about itself. This is
also how scripts got pulled in: wires, conditions and actions reload with the tables, so
they were built with them. The tables must still see them as **opaque closures that came
from somewhere**.

### 2.2 What Tabular is

**The file is a bundle; Tabular is one of its owners (owner, 2026-10-12).** The old
`.tabular` file had a yaml header followed by csv subfiles separated by `---` markers, and that
container worked; the delimiter is not the problem. But the yaml header held more than table
metadata: it also held the wires. The wires are not related to the tables. They were there
because, for the engine, one subsystem owns {wires, Tabular, hot reload} as one blob, even
though Tabular does not own the other two. The file is that subsystem's unit of hot reload,
and its sections belong to different owners.

The test for what is Tabular: **can it be defined, run and tested without a world** (no
objects, threads or script engine)?

| Tabular | The larger subsystem that has it |
|---|---|
| tables and relations: keys, match operators, pick policies | the bundle file and its container; which section goes to which owner |
| a builder API at the core; parsing a table section is a front end that calls it | wires: names, lazy memos, owner-only forcing |
| Definition (per section), Binding (per definition × object type), Instance (per object) | hot reload: when it happens, swapping and migrating |
| a step function that touches only its own instance and emits opaque records | objects, the tick, `parallel_for`, barriers |
| deterministic random from a seed it is given | command queues, message routing, the script engine, stable ids |

Tabular reads its inputs (wires, fields, anything else) through names the host's symbol table
resolves, so **it cannot tell a wire from a field and does not know wires are lazy**. Earlier
in this note Tabular "owned the wires" and "orchestrated the steps". Both moved to the
larger subsystem. Tabular guarantees that its step is safe to run in parallel; something else
runs it.

### 2.3 Snippets are opaque; resolution is the host's

Script snippets are part of the `.tabular` file, and sometimes part of a table: a condition
cell can hold `length(a) < 0.5`. **Tabular passes the string to a compiler and gets back an
opaque function it can call.** The old design tried to link snippets into AngelScript
instead.

The old coupling did not come from laziness. It came from **name resolution**: to decide
whether `a` was a wire or a field, Tabular resolved names itself through AngelScript's types
and typing rules, which made it part of AngelScript. The fix is to turn it around:

    Tabular → host:  compile(text, kind, symbols) → closure or error
    host → Tabular:  symbols.lookup("a") → { slot, type, getter } or "unknown"

- The host compiler resolves the object's own fields in its own type system, then asks
  Tabular's symbol table. A name both know is a compile error naming both, with no silent
  precedence.
- The compiled code calls Tabular's getter. Tabular never sees the code.
- **Laziness lives behind the getter.** `get_wire` checks the memo and otherwise calls the
  wire's own closure, which is another host-compiled snippet. Memos are stamped with the tick,
  so there is no clearing pass.
- Tabular defines a small type set (bool, int, float, enum-as-hash, maybe vec). The host maps
  its types onto it.
- AngelScript has no built-in hook for unknown identifiers. The engine adapter makes one (for
  example, a generated context type with one property per wire). That glue stays in the
  adapter.

Closure shapes the owner sketched:

    bool (*)(void const*, ErrorContext* out);   // condition: read-only
    void (*)(void*, ErrorContext*);             // command: read/write

A bare function pointer cannot carry a snippet's identity, so a closure is likely a
`{fn, closure data, release}` triple, with `release` handing the closure back on hot reload.
The `void const*` a snippet receives is a context Tabular builds (object, wires, state), not
the bare object.

**Cost:** compile once per distinct (snippet text, object type), cached by hash, shared by
every instance of the type.

### 2.4 Visibility

- **Owner-only forcing.** During the step, an object forces only its own wires.
- **The state vector, wires included, is not visible to other objects.** Objects learn about
  each other through the world and through commands and messages. (The old design let a table
  read `lamp.brain.state`, which this drops.)

### 2.5 Deterministic random

Table random, whether explicit `random()` or implicit through weights, was a different
function from script random, and that caused a bug. It must be deterministic: other objects
existing, evaluation order and races must not change it. So it is a counter-based draw with
no stored state:

    r = hash(world_seed, object_id, tick, table_id, row_or_site)

`object_id` must be stable (not a pointer, and not spawn order if spawning is parallel).
Each draw site needs its own salt, or two picks on one object in one tick move in lockstep.
Script random should be the same primitive, with one definition provided by the host.

### 2.6 Building blocks seen so far

- **Relations** with N key columns. Pivoted or long form is presentation, chosen per table
  by density, never a different kind of table.
- **Match operators:** eq, flags subset, band (a range of time), edges (crossings).
- **Pick policies:** exactly one (A1), weighted random (A2), first whose condition holds
  (B1), most specific (A4, D1), all (D1's layers). A row can carry both a condition and a
  weight.
- **Vocabularies** (enums, flag sets) declared once, visibly, by the host. Tables bind to
  them by name, and the dependency points from tables to script.
- **Static versus per-tick:** A1/A2 can resolve at bind time (except the random draw);
  A3–A5 and B1 run every tick; D1 changes when rows are inserted or retracted.
- **Authored versus runtime rows:** A* and B1 rows come from a file and are fixed when it
  loads; D1 rows come from contributors at runtime, each owned by `by`.

## 3. Open questions

- **Who owns the state vector and the machines?** If Tabular is relations and queries, are the
  HFSMs (state, transitions, bridges, edges, timers) Tabular built on its own relations, or part
  of the larger subsystem?
- **What is the larger subsystem called?** It owns the bundle, the wires and hot reload; it
  needs a name in `CONTEXT.md`.

- **What a bridge step carries.** Nod repeats head-down/head-up 1–4 times at random; chew
  waits 20–50 ticks; each death pose fertilises the room. So a step probably needs a state or
  clip, an exit (clip ended, or N ticks, possibly random), commands on entry, and a bounded
  repeat count. Bounded repetition keeps it decidable.
- **Counters and timers.** The grazer's panic, courting and mate-cooldown countdowns and its
  energy all follow "set to N (maybe random), decrement, leave at 0". Table primitive, or
  script?
- **Growing up is "replace me".** The grazer grows by killing itself and spawning the next age
  with energy and position carried over. Respawn command, or is age just a state?

- **How a table absorbs a message.** Settled that it can and in what order (§2.1). Open: in
  which step it runs, and how a handler row is written.
- **Who reads wires besides the owner?** Animation reads them for blend spaces. If the state
  vector is invisible, is animation part of the owning object (reading after the barrier)
  rather than "another system"?
- **Is D1 part of Tabular?** It shares the query shape (most specific match, outer join) but
  has no authoring surface, runtime rows, and global scope instead of per-object.
- **Decidability.** Tables stay finite with no recursion. Snippets are opaque, so their cost
  and termination are the host's contract, which the host interface must state.
- **Which use does the first version serve?**
