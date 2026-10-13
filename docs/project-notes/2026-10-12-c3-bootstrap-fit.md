# Creatures 3 bootstrap against the Tabular model

Produced 2026-10-12 by a Sonnet workflow (run `wf_a40a10ed-2c0`): eight readers each classified about 9,200 lines of the C3 bootstrap (`~/Applications/Creatures 3/Bootstrap/`, 195 files) against `docs/definition.md` and the proposals then under test, and one synthesizer wrote sections 2 to 7.

**Coverage.** All 195 files were classified. The headline table below sums all eight readers. The synthesizer's prompt received only seven of the eight result blocks: the batch with medical pod, Lifts, kingfisher, creatureDecisions, hoppity, keyboard handler, wasp, gnats, cuttlefish, Hatchery, gene pod, gumin grass and 12 smaller files is under-represented in sections 2 to 7. Its per-file shares match the rest (critters 12 to 26% not fitting, gene pod 59%, CAOS command line 81%), so the conclusions are unaffected. The raw per-file classifications are in the run's journal.

**Treat every number as a reader estimate.** Readers estimated line counts per file and drew the lines differently: some counted scan loops (find, hunt, count) as `wire`, while the woodpigeon and dragonfly readers put much of the same shape into `script_engine`. This widens the spread between agents.

## 1. Headline

Summed reader line counts across all 195 files:

| Category | Lines | Share excluding infrastructure | Share including infrastructure |
|---|---|---|---|
| table | 11,185 | 23% | 16% |
| wire | 6,519 | 14% | 9% |
| command | 14,746 | 31% | 21% |
| entity_script | 6,022 | 13% | 9% |
| script_engine | 9,579 | 20% | 14% |
| infrastructure | 21,501 | n/a | 31% |
| **Total** | 69,552 | 48,051 behaviour lines | |

- **Fits the model.** About two thirds of behaviour lines (table, wire and command, 68%) land in the model's declarative parts. Entity scripts take another 13%, which is arithmetic on own data and is allowed.
- **Needs the engine.** About 20% is genuinely `script_engine`. Most of that is GUI, text and tool code, plus non-commutative world edits.
- **Infrastructure.** Nearly a third of all lines is installers, removal scripts, copy-pasted species parameter blocks, GUI part construction and world maps. `!map.cos` alone is about 1,600 infrastructure lines.
- **Command is inflated.** It is the largest category because readers counted spawn templates and `new: simp ... setv ov..` blocks there when they ran at play time. Corridor Doors (690 command lines) and the Norn seed launcher (650) are the clearest cases. The readers say these collapse to a species-keyed spawn record plus a spawn command.

**Per-object view.** For behaviour-bearing agents, the share of non-infrastructure lines that is `entity_script` plus `script_engine`:

| Group | Typical share not fitting tables | Agents |
|---|---|---|
| Typical critter | about 15-30% | grazer ~24%, bacteria ~22%, balloon bug ~41%, bee ~23%, man-o-war ~22%, sharkling ~29%, grasshopper ~26%, mosquito ~23%, clown fish ~17%, piranha ~16%, Robin ~21%, Butterfly ~14% |
| Low outliers | about 5-10% | hedgehog ~6%, Kobold ~6%, tendril ~8%, rocklice ~8%, stickleback ~11%, gnarler ~10%, hoppity ~12% |
| High outliers | about 45-65% | Hummingbird ~47%, beetle ~43%, Uglee ~54%, woodpigeon ~57%, dragonfly ~64%, nudibranch ~61% |
| Machines | varies widely | NOT gate, logicgate, antigen detector, plusminus switch and the port gates are almost all table or command. Creator, replicator, Norn egg layer and recycler are 25-65% engine. The count gate is about 66% entity script. |
| GUI and tools | mostly engine | `crypt3.cos` ~86%, `GUI 1.cos` ~64%, `wolf control.cos` ~96% |

The high critter outliers are the ones with mate and nest claiming, replace-me growth with placement tests, and scan-with-kill loops. The low outliers are the ones where the CAOS is mostly a state ladder plus random rolls.

## 2. By kind of agent

**Critters (grazer, bacteria, fish, birds, insects, mammals).** The strongest fit is the state ladder as a ranked table.
- Fits well: hunger or age bands, a weighted roam roll, obstacle bounce as flag rows, clip bases as an A1 key table, corpse variants, and message-absorbing rows (collision, pickup, eat).
- Entity scripts: the per-tick `age+1` and `energy-1`, velocity ramps, centroid and steering arithmetic, eat gain.
- Does not fit: mate and nest claiming, direct `kill` of prey, spawn with a placement test, and cross-object reads.

**Plants and food (desert grass, opal and orange sponge, fungi, grass, cacbana, tendril, foxglove, apples, carrot, palms).** Seed environment checks and stage ladders fit very well.
- `seed_env` and `seed_fate` are rows of bands over room properties with a fall-through.
- Stage ladders such as orange sponge growth are the best-fitting tables in the batch.
- What strains: uptake and loss arithmetic against the room (about 140 lines in `grass.cos`), `pose+1` growth, and dormancy counters.

**Machines (Creator, replicator, recycler, splicer, volcano, teleporter, gates, detectors, dispensers).** Split into two groups.
- Port gates and sensors fit almost entirely. Examples: logic gate truth table, antigen detector bands, contact sensor, plusminus switch, siren, radio, smell and light modules. The shared hit, pickup and activate boilerplate becomes one library table.
- Bioenergy-spending machines do not. They spend a shared global pool, call PRAY synchronously, loop over passengers, or edit the room graph.

**Doors and lifts.** The door open, close and partner handshake is a bridge. The corridor doors are 11 near-identical copies and the ettin accessways are 4. Each collapses to one parameterised table. Teleporting and shoving everything touching the door, room-graph `door`/`link` edits, and passenger capture are the non-fitting parts.

**Creature systems (creatureDecisions, creatureDoneTo, creatureInvoluntary, creatureBreeding, creatureToCreature).** The best fit in the batch.
- `creatureDoneTo` has about 110 table lines out of 192. Its stimulus responses are ranked rows keyed on event and sender, with weighted sound variants.
- Breeding's mate outcome is a flag-ranked table.
- Gene moves, `born`, history edits and polling `untl code = -1` stay in the script engine.

**GUI, world and tools.** Mostly infrastructure plus `script_engine`.
- Text assembly, pagination, history search loops and PRAY import/export have no table form.
- Clean exceptions: key dispatch (`keyboard handler`, `textentry`), pointer reaction tables, screen-mode transition tables (world switcher `button_event`), and the environment time-of-day and season tables.
- The world map is data relations loaded once, not behaviour.

## 3. Recurring patterns and where each lands

| Pattern | Lands in | Files |
|---|---|---|
| Installer boilerplate and removal scripts | infrastructure. A species-keyed spawn record plus a spawn command at play time. | nearly every file |
| Mode ladder with sequential overrides | ranked decide table, in reverse of source order (later write wins in CAOS) | pigeon, dragonfly, kingfisher, Kobold, hedgehog |
| Threshold bands over energy or age | ranked table (native band if the threshold is a species constant, script condition if per-instance) | all critters |
| Scan for nearest, count or centroid | wire. It returns a handle, a count, or a sign flag. | stickleback, rocklice, Robin, hummingbird, beetle |
| Sign-of-delta steering | wire gives `sign(dx)`/`sign(dy)`, then an equality-keyed table | hunt in 8+ files |
| Obstacle bounce | flag-subset table, one per axis | snail, kingfisher, fish |
| Random pick or `rand` ladders | weighted table | grazer, meerk, bee, sponges |
| `loop rand untl <> 0` | weighted table without the zero row | opal sponge, beetle, stickleback, dragonfly |
| `anim [..] over`, `wait N` | bridge. `lock` becomes a busy state that ignores events. | most files |
| Clip bases in `ov30..` | A1 clip table | all critters |
| Hit, pickup, activate machine boilerplate | one shared library table | machines |
| Eat handshake | message to food, food replies, eater's entity handler credits energy | beetle, dragonfly, snail, Hummingbird |
| Population-cap guard | wire `count(class)`, ranked row, spawn command | bacteria, fungi, bug, launchers |
| Replace-me growth | spawn command with an init payload, plus kill self | grazer, fish, pigeon, plants |
| Room nudge on death or decay | commutative world command | many |
| Cycle-to-next buttons | keyed successor table if enumerable, entity script if a data setting | beatbox, detector, env controls |
| Standing emission (`emit chem rate`) | a level-triggered slot read by the smell subsystem. This is not an event command. | smell emitters, volcano, light and heat emitters |

## 4. Misfits, clustered by cause

**A. Direct writes, kills and claims on other objects**
- Mate claim: `targ ov17 / seta ov17 ownr` (Robin2 `fmte`, woodpigeon `fmte`, grazer shag).
- Nest test-and-set: `doif va50 eq 0 ... targ ov19 / setv ov71 1` (woodpigeon `fnst`).
- Eat by direct kill, with a gain that depends on winning (grazer, meerk, kingfisher `kill ov18`, clown fish `kill targ`, nudibranch `kill targ`).
- Alarm propagation `seta ov17 va17` inside `esee` (wasp, bee).
- Teleporting or shoving every creature touching the door (`mvft`, `velo`, `nohh`) in all 11 corridor doors, the airlocks and the mediporter.
- Reader resolution for eating and alarms: messages with a reply (`eaten`, `fed`) or a receiver that decides. Teleport and shove become a message to each occupant, which refuses or moves itself. Mate claim and nest claim need a handshake with arbitration.

**B. Exactly-once and check-then-spend on shared resources**
- `subv game "Bioenergy" 50` (ettin seed bank, grendel seed bank, aquatic launcher, replicator, cheese machine, medicine maker). Two machines can both pass the check.
- Room water uptake with a clamp: `altr room targ 3 va00` after `min(uptake, available)`. The nudge commutes but the clamp does not (desert grass, grass, foxglove, cacbana).

**C. Cross-object reads**
- Grazer reads the food's `ov99` and the partner's `ov05`/`ov95`.
- Environment monitors read the env-control machine's `ov01..ov03`.
- Creator reads `posx` of two gauge widgets as its energy test.
- Robin reads the mate's `ov19`.
- Lifts and airlock buttons read sibling `ov00`.
- `enum 1 1 91 ... doif ov00 eq 5` filters on the receiver's state.
- Several readers say essentially every critter needs this.

**D. Counters, cancellable delays and stale messages**
- Grazer panic: `setv ov97 rand 8 20 ... subv ov97 1`. It can be re-entered while panicking, so a stale `panic_over` would end the new panic early.
- Wasp anger `ov88`, logic gate `wait 40` that clears a freshly refilled input, and Uglee boredom against a per-instance threshold.
- Desert grass and tendril dormancy `ov72` decrement only when the environment test fails. They count failures, not time, so a delay message cannot represent them.
- Counters that count only while a condition holds (neon fish `ov74`, `ov70`).

**E. Same-tick cascade and multiple writers**
- Grazer `0 -> 1 -> 2` in one wake (`Need food? check`, `choose`, `Move to the food`). Under the swap this takes three steps.
- Kobold writes `ov00` in four places and the last wins. Foxglove plant phases cascade 0->1 and the `eq 1` block runs at once.
- Snail and kingfisher wall probes are independent `doif`s writing different variables. A single winning row loses an axis.
- Clown fish `ftsw` writes `velo` eight times and only the last survives.

**F. Spawn and replace-me**
- `new:`, then `tmvt`, then `kill targ` on failure. Parent state depends on the spawn result (hummingbird `matr`, snail `die_`, launcher, stickleback `layg`, gnarler `age_` with an age rollback).
- Child fields written after creation (bacteria copy, ~90 lines, 9 genome fields plus mutation).
- Mutual handle graphs (cacbana plant and flowers, meerk mound `ov17`).
- Placement needs the new object's bounding box.

**G. Bridge needs beyond clip-ended and timer**
- Wire-edge exits: SeaLift exits on `posy` thresholds; hatchery waits for the egg's `code = -1`; creature `appr`/`touc` wait for arrival.
- Parts as parallel tracks (volcano: part 0, 1 and 2 interleaved with messages).
- Commands mid-sequence and at exit. Corridor doors bracket the sequence with stim 75 and stim 95, and the second must run even if aborted.
- Abort by interrupting message (grazer collision `tick 1 stop`).
- Variable repeat bound (delay gate setting up to 20) and 25-iteration hatch jitter with a fresh random per repeat.
- Branching inside a sequence (recycler).

**H. Synchronous results**
- `pray injt` returns 0/-1/-2/-3 and the Creator branches on it in the same script.
- `pray test`, `pray file`, `hist` and string results.
- Welcome screen polls `untl code = -1` three times per iteration.

**I. GUI and text, genuinely script engine**
- `GUI 1.cos` (six copies of 'clear faces, enum 4, fetch, put in slot'), `crypt3.cos` history search, `wolf control`, speech bubble fit test, blueprint export.
- Destroy-and-recreate rendering every 10 ticks (`map display.cos`, `toolbar.cos`).

**J. Camera coupling**
- Gnats `visi 0` drives the fly-loop sound.
- Favourite places `doif visi 0 eq 1` sets gameplay state.
- Creature death `loop wait 1 untl visi 0 = 1` holds the death until visible.
- Rain sound starts and stops on drop visibility.
- Doors pick a camera transition by `from eq pntr`.

**K. Latent bugs found by reading conditions as bands** (the readers read these as evidence for totality checks)
- Bacteria ettin test `posx ge 4400 and ... posx le 50` is impossible.
- Opal sponge `va50 >= 8 or va50 <= 10` is always true, so `> 10` is unreachable.
- Grazer duplicates its first test.
- Man-o-war `hunt` sets -1 in both branches.
- Sharkling `va20 < 50` guard tests the wrong channel.
- Logic gate AND compares with 99 where 999 is the empty value.
- Camera `rand 0 1 eq 9` never fires.
- Ant hill stage gap at 800..1000, gumin grass adult bands leave gaps (ov01 111..120, 49..-49), and the smell detector has no row for exactly 1.0.
- Aquamites `ov01` is never decremented.
- Marine cave water has about 55 lines of commented-out code, and several installs are commented out.

## 5. Tensions with the definition or the proposals, ranked by how much they matter

**1. Entity scripts and commands that touch other objects (isolation, commutativity, exactly-once).** This is the biggest structural pressure.
- Evidence: cluster A and B above. About 8 of ~35 non-trivial behaviours in the hummingbird/ant/cacbana reader were cross-writes. A third of critter effect lines are spawn or kill.
- Supports the rules: additive effects commute and fit cleanly. These are `chem`, `stim`, `altr room` deltas, `emit`, sound, particles and a parent counter increment. Eat, alarm and teleport all convert to messages whose receiver acts on itself, and the door teleport becomes sound because the creature owns its carried and held flags.
- Strains the rules: exactly-once consumption, claims, and conserved pools need a reply or arbiter that the model does not have. Direct kill plus gain is the commonest instance. Whether commutativity is a property of the command kind or of the pair of emitters is also unclear (the readers differ). Population-cap spawns do not commute either.

**2. Reads of another entity's state.**
- **Resolved (owner, 2026-10-12):** wires are arbitrary lambdas that call only read-only script functions, e.g. `for (item : Query::Visible(me, "Grazer"))` giving a grazer ready to mate. See the shape note §2.3. Handles: wires may hold value types or handles (GameObject references keep a tombstone when the object dies), never script types; Tabular reads only the `int64_t`/`double` ones, and a command consumer resolves handle names itself.
- Cluster C, in nearly every critter and machine. It decides the open question in the definition: several readers answered 'yes, they need it' for at least four files and for essentially every critter.
- The model needs published attributes or entity-argument wires (`chem(creature, 48)`, `partner.nest`).
- Object handles are stored in state across ticks (`ov16/ov17/ov18/ov19`, `ov01` supporter) and go null when the referent dies. The model has no handle type (hash, flags, int64, double only), and two readers suggest handles as int64 stable ids.

**3. Delayed messages are uncancellable and can arrive in a re-entered state.**
- Evidence: cluster D, hit in the first creature tried (grazer panic). It is also the definition's own open question.
- Supports: most countdowns do convert (hedgehog pregnancy, hawk boredom, Uglee boredom, egg hatch `random(15000,20000)`, ettin muscle paralyser `random(1,25)`, switch heartbeat, `mesg ... 40` clears).
- Needs a cancel or generation id, or a state key so a late message holds. Counters that run only while a condition holds do not map at all and stay in an entity script.

**4. Same-tick cascades and multi-writer variables (DAG order, next-vector swap).**
- Evidence: cluster E. CAOS lets later code see earlier writes in one wake, and the last writer wins. Under the swap each transition takes a step, and several readers flagged this as a behaviour change.
- Workable: the definition allows reads of in-step writes through the DAG, so a decide table feeding a dispatch table works. The ranked order has to reverse the CAOS source order, which readers called a source of translation bugs.
- Needed: a rule for two tables writing one column in a step, and per-column hold semantics. Several readers raised unwritten columns holding.

**5. Spawn is a command with a result.**
- Evidence: cluster F, with replace-me the dominant lifecycle shape in the hummingbird/ant/cacbana and woodpigeon/dragonfly readers.
- The models fit when spawn carries an init payload and the consumer owns placement fallback (`:if-blocked safe`). Readers also proposed a `can_place(x,y)` wire read before the command, or stable derived ids.
- Does not fit: parent state that depends on the placement result.
- Spawn also needs arithmetic and `random` inside the s-expression (`y - 16`, `random(25,28)`, velocity `power/9`).
- Command order versus caps: two spawners reading the same count both pass. Readers tolerate overshoot.

**6. Sequences as bridges: abort, exit and busy semantics.**
- Evidence: cluster G. Most sequences fit, but exits on wire edges, commands on abort, parallel part tracks and variable repeats are missing.
- `lock/unlk` is used everywhere. Readers disagree whether CAOS drops or queues events during a locked sequence. Several say the model must choose, and a busy state with hold rows (ignore) is the natural reading.

**7. The 'no state' = state + 1' rule: the evidence mostly supports it.**
- Sites found: pose-as-stage growth (grass, cacbana, carrot, foxglove flower, starplant, palmplant, gumin seedling, apples, pumperspikel), cycles (radio channel, beatbox tune, detector mode, medicine selection, garbage spewer, env-control levels, launcher fish selector), negation (`negv ov10`, logic gate mode, camera screen), and countdowns shown as a pose (airlock).
- Every site resolved in one of four ways: a clip played once with an end edge, a stage derived from an age band (opal sponge, which removes the +1 entirely), an enumerated successor table (env-control 0..4, aquatic monitor 1,2,3,4,5,10,15,20,25 with an irregular successor, lightmodule colours), or data in an entity script.
- Mechanical test proposed by one reader: a value is state if any table keys on it, data if it is only passed on or banded. The smell id (1..17 skipping 11), delay-gate setting (1..20), catalog cursors and ring cursors are data. A 255-row enumeration is the absurd case.
- Pose doubling as display frame and stage is the real source of the conflation.

**8. Native comparison is input vs literal; one script condition per row.**
- Per-instance thresholds are the cause (`ov02 lt ov73`, bacteria `ov33..ov38`, heritable `ov13/ov14`, seed `ov80..ov86`, count < wanted).
- Several readers note most of these are species constants in practice and become literals. The bacteria thresholds are genuinely heritable and stay script conditions. Others become a derived `hungry` wire.
- Conjunctions of keys, bands and flags fit well (replicator `can_clone` has five conditions and one script condition; clown fish `stage_advance` has seven native ones). 'All conditions hold' is undecided in the definition, and `or` has no home (rows duplicate output).

**9. Weighted tables have no conditions.**
- Roughly 10 sites need a helper table to supply a key, and one reader noted the catalogue (2.6) permits a row with both condition and weight while the definition forbids it. Randoms with conditions or per-instance bounds are the cases: bacteria's one roll against three thresholds, `ov02 le rand 100 200`, `rand ov00 50 > 48`, `light < 0.2 or rand 0 19 eq 0`, cuttlefish's nested draws.
- Readers proposed a named per-step draw (salted per site) feeding a ranked table.
- Pure literal weights fit exactly (grazer activity, season delta, mutation slot, prey picks, roam rolls).

**10. Arithmetic placement.**
- The 'arithmetic in entity scripts' rule is strongly supported: `on energy(delta)`, scripts 256/257/258/260 in pigeon and Robin, `addv ov02 _p1_`, waterplant leaf handlers, the delay gate setting `clamp(setting+d,1,20)`, velocity ramps. Readers found no case that needed arithmetic in a table.
- Arithmetic also appears in command s-expressions, table value cells (light = base × factor × level), and wires (`relx` squares). If s-expressions cannot compute, every spawn needs an entity method just to build its arguments.
- Accumulator ordering: tables decide, then scripts apply, so a decision made on the updated value (`energy <= 0 -> die`) now sees the old one. That is a one-tick change.

**11. Tick rate is behaviour.**
- Objects choose their own period (`tick 6` down to `1` in lifts, `tick 20` for sleep, `tick rand 90 120`, dormancy `tick ov70/ov71`). Energy and age are counted per fire, so slowing the period slows decay. Carrot even reads `tick` as state.
- Tabular is 'not a scheduler', so `(tick n)` becomes a command to the host plus a `ticking` input.
- A per-agent heartbeat as a message per tick has a cost (the ant file raised it).

**12. Totality: supports the rule.**
- CAOS ladders silently fall through, and moving to tables makes the hold explicit and checkable. Readers found about a dozen latent bugs from band tables (cluster K). Two further needs: 'hold' (no write) must differ from 'default' (writes a value), and wildcard keys (`cacl 2 8 0 6`, `scrp 4 0 0 17`) need specificity ordering.

**13. Sim does not couple to the camera.**
- Cluster J. Gameplay discovery and death timing read `visi`. Doors branch on whether the pointer triggered them.
- Readers suggest a separate presentation or camera command class, and events sent by the camera layer instead of wires. A bridge exit on a `visible` wire would break the rule.

**14. Per-tick order proposal.**
- Mostly fits, but order-dependent examples: the mosquito reads `ov00` set earlier in the same script; kingfisher dispatches on a mode set at the top; spawn then immediately configure the child; GUI relies on immediate feedback within one handler (`part`, `pose`, `ptxt`, then message).
- 'Ate, gain energy now' becomes 'ate, reply, gain next tick'. Readers consider that acceptable for biology and wrong for UI.

**15. Level versus edge commands.** Standing emission (smell, light, heat) and per-tick `prt: send` of a level are not events (cluster in patterns table). One reader notes edge commands are the model's only kind.

## 6. Gaps: things the model has no place for

- **Reply and acknowledgement path.** Request-response messages, spend-if-available with grant or deny, claim with arbitration, spawn success feedback, and the PRAY return codes.
- **Published attributes and entity-argument wires.** Read-only fields other entities expose (`edible`, `pregnant`, adult flag, partner nest, env-control levels, creature chemistry and loci).
- **Handle type in state.** Object handles with liveness (weak refs), stored across ticks and cleaned when the referent dies. Proposed as stable int64 ids.
- **Relations with contributor-owned rows.** The display relation (D1): channel to members for radios, first free nest slot, lift floors, function-key bindings, port wiring between machines, selection and 'norn' indicator, map markers, bubbles owned by a creature.
- **Class addressing in `:who`.** `rtar f g s` then `mesg` is repeated about ten times in one airlock file.
- **Level-triggered commands.** Standing emission and continuous port levels.
- **Atomic resource pools.** Global Bioenergy and conserved room water have no owner.
- **Cancellable or generation-checked delays**, and counters that run only while a condition holds.
- **Wire-edge bridge exits**, abort-with-exit-commands, parallel animation tracks, variable repeat bounds, and mid-sequence branching.
- **Package-level handlers on classifier patterns** (`z_creaturesAffectingAnimals.cos` attaches to every family-2 agent), and a one-shot setup pass over existing objects.
- **World-wide components** (`wetness monitor` writes `ov60` on every object from outside).
- **Per-step named random draws** for thresholds, and per-recipient draws in a broadcast (volcano kicks each rock with its own random velocity).
- **Placement query by archetype** (can a thing of this size be at x,y?) and the `tmvt`/`mvsf` fallback inside spawn and teleport commands.
- **Enumeration order.** Scan order is unspecified yet implicit semantics (last match, first match, last reading wins in the medical scanner and detector).
- **Text and UI processing**, string results and database queries (`hist`, `read "catalog"`). Readers put these in the script engine or the GUI DSL.

## 7. Representative agents

**Grazer (2 15 2).** Mostly table and message-driven.
- Tables:
  - `grazer_brain` (ranked; keys life-stage; bands on energy, panic, pregnant, cooldown; one script condition 'touching target'), writing mode.
  - `grazer_activity` (weighted over walk, run, nod, chew, lay with weights 2/1/1/1/5).
  - `grazer_water_avoid`, `grazer_clip` (A1) and `grazer_message_absorb`.
  - `grazer_growth` (age >= 500 and not panicking, then spawn next stage).
- Wires: `esee` food (nearest with a defined tie-break), `herd_side` (aggregate sign), `nearest_eligible_mate`, `touching`, room property 5, `heading`.
- Commands: bridges nod, chew, eat and die (four steps, each fertilising the room by `altr room 3 0.1` and `4 0.1`, last step `kill this`). `(spawn :class (2 15 2) :stage youth :energy this.energy :at (x, y-16))` plus `(kill this)`. `(message partner "conceive")`. `(message food "eat")`.
- Entity scripts: `on fed() { energy += 50 }`, `on update() { age += 1; energy -= 1 }`.
- Open: panic re-entry (stale delayed message), the three-step cascade latency, collision aborting the running bridge.

**Corridor door pair (11 near-identical scripts).** One parameterised definition.
- `door_machine` (ranked; keys door-kind and state closed, opening, open, closing; edge on message arrival) with a bridge: anim, `(message partner 1000)`, wait, teleport, `(message partner 1001 :delay 2)`.
- `activator_kind` (flags: pointer, creature `fmly 4`, other) selects the teleport and camera behaviour. `switch_input` is a ranked table with a band on `p1 >= 128`. `clac_route` is a trivial one-row-per-switch table.
- Wires: `etch 4 0 0` touching, `from eq pntr`.
- Commands: a broadcast `(message each-touching "teleport" :to partner)`. Each creature's entity method refuses if carried or held. The `meta`/camera transition is a presentation command emitted only for the pointer. Stim 75 and 95 bracket the sequence and must run even on abort.
- Open: exit commands on abort, camera command class.

**Count gate (3 8 12).** The honest 'tables are the wrong tool' case.
- Table: `count_buttons` and `machine_events` (hit, pickup, activate rows) are tiny.
- Entity script: `on input(v) { n += 1; if (n >= target) { n = 0; port.send(v); } }`, and the up/down arrows `setting = clamp(setting ± 1, 1, 20)`.
- The count and target are data, not state. The table picks the arrow direction and the entity script owns the arithmetic.

The delay gate and logic gate fit the same pattern. Their truth table (logic gate: in1 present, in2 present, mode) is the cleanest small table in the batch.

**Files for follow-up:** none were written. Source CAOS file names are given in the sections above.