# Tabular: feature checklist

Every feature the Tabular documents describe, in one place, checked against what is built.
`definition.md` is the contract. Where an older document disagrees with it, the older one
lost; those features are under **Rejected** so they are not rebuilt by accident. Features
`definition.md` assigns to the host are under **Host adapter (not Tabular)** so they never read
as Tabular's debt.

Sources, cited in brackets:

| Tag | Document |
|---|---|
| def | `docs/definition.md` |
| rem | `docs/superpowers/landed/specs/2026-10-18-tabular-remainder-design.md` (premises P1–P17, rulings R1–R12, As-built, Open owner decisions) |
| cat | `docs/project-notes/2026-10-12-table-catalogue-and-shape.md` |
| c3 | `docs/project-notes/2026-10-12-c3-bootstrap-fit.md` |
| retro | `docs/project-notes/2026-10-12-retrospective.md` |
| arc | superproject `docs/superpowers/superseded/arcs/tables-as-queries.md` |
| cook | superproject `docs/superpowers/superseded/specs/2026-10-07-tabular-cook-and-lookup-parity-design.md` |
| attach | superproject `docs/superpowers/superseded/specs/2026-10-08-tabular-attach-design.md` |
| sr | superproject `docs/superpowers/superseded/specs/2026-10-09-tabular-state-and-rules-design.md` |
| guard | superproject `docs/superpowers/specs/2026-10-09-context-free-guard-closures-design.md` |

`[x]` built and tested; the parenthesis says how it was built, and the gtest suites after
"Tests:" are where it is held. Every item of Tabular's own is `[x]`; what stays open is a
decision, listed in def Undecided and rem Open owner decisions, not a missing feature.

## 1. Relations core

- [x] Typed columns: hash, flags, int, double, condition, command [def Structure; CONTEXT.md] Tests: `RelationSchema`, `RelationStore`
- [x] Command-pattern structs for create table, columns, insert, retract [def order 1] Tests: `RelationSchema`, `RelationStore`
- [x] Every row carries its owner; retract every row an owner has [def First version; arc §6.2; cat D1] Tests: `RelationStore.RetractRemovesOnlyThatOwnerAndKeepsSurvivorOrder`, `RelationStore.RetractReachesEveryTable`
- [x] Key columns are equality and indexed: candidates are a lookup, not a scan [def A row] Tests: `RelationFind`
- [x] A sorted order with prefix lookup, optionally unique; a unique clash is refused [def order 1] Tests: `RelationFind.OrderedPrefixFindsAContiguousRange`, `RelationStore.UniqueOrderRefusesASecondRowWithTheKey`
- [x] Two kinds of table, each refusing the other's writes: read-only and mutable; a best table may be mutable, held as versions and checked on insert [def Two kinds] Tests: `RelationLoad.ReadOnlyTableRefusesInsert`, `RelationLoad.AWritableTableHasNoVersionAndRefusesLoad`, `MutableBest`
- [x] Read-only tables are immutable versions, replaced whole only after the new one checks out (RCU); a held version stays consistent [def Two kinds] Tests: `RelationLoad.AHeldVersionIsNeverModified`, `RelationLoad.LoadIsAllOrNothing`
- [x] Hash = `CityHash32` of the bytes as written, matching a script literal's `NameHash` [CONTEXT.md Hash; sr §3.1] Tests: `RowsCsv.AHashIsOfTheBytesAsWrittenLikeNameHash`
- [x] Runs with no world, no threads, no script engine, no files [def What Tabular is] (the core includes only the standard library and Spehleon/lib's Errors and Support, the front ends add its Yaml and CSV readers; every suite, the worked machine included, runs in a plain gtest with host stubs)
- [x] RAII owner handles that retract their rows when dropped (crash and unload recovery) [def Lifetimes, Owner handles; cat D1 §1.3; rem P1] (`owner_handle`; retracts from every table, read-only and grids included) Tests: `OwnerHandle`
- [x] Report that a relation's rows changed, so the owning subsystem can apply its reload policy and dependent queries wake [def First version, Generation, Triggers; arc §6.2; rem P2] (the per-table generation counter; there is no callback) Tests: `RelationGeneration`
- [x] Refuse hash 0 and same-file hash collisions, naming both names [sr §3.1] (CSV front ends, within one load, any column; YAML members and literals within one machine) Tests: `HashNames.ZeroIsRefusedAndRepeatsAreNot`, `RowsCsv.TwoNamesWithOneHashAreRefusedNamingBothLines`, `MachineDefinition.AMachineMemberIsHashedAsACellIs`
- [x] `vec2` column: two floats, one cell; the only vector type. Bands are `vec2`; a position is a string column naming the Section plus one or two `vec2` columns; no `Location` type [def Column types, owner 2026-10-16] Tests: `RelationColumns.Vec2HoldsTwoFloatsInOneCell`, `RowsCsv.Vec2IsWrittenAsARange`
- [x] String column holding an interned `counted_string`, in its own per-table slot array like commands [def Column types, owner 2026-10-16] Tests: `RelationColumns`

## 2. Front ends

- [x] Grid tables: key × key, dense, every cell filled; grid CSV; one contributor [owner 2026-10-16; def Pivot] Tests: `Grid`, `GridCsv`
- [x] YAML table definitions; any diagnostic fails, unknown keys included [def order 2] Tests: `SchemaYaml.AMisspelledKeyIsRefusedNotIgnored`, `SchemaYaml`
- [x] CSV rows into read-only tables; header names columns; refusals name the line and the clashing line; BOM dropped; no Unicode normalisation [def order 3] Tests: `RowsCsv`
- [x] RFC 4180 quoting in every CSV front end, one reader: quoted cells keep commas, line breaks and doubled quotes; spreadsheet padding and empty rows ignored; only an unquoted `#` starts a comment [def order 3 Quoting; commands-and-messages spec Part 1] Tests: `CsvQuoting`
- [x] CSV filling condition columns: `csv_rows::condition`, a `std::function<tl::expected<closure, ErrorContext>(std::string_view)>`, is called per cell; read-only `Load` accepts condition columns [def order 3, owner 2026-10-16] Tests: `RowsCsv.ConditionCellsGoThroughTheFunction`, `RelationLoadConditions`
- [x] A command cell can hold a comma: quoted, or as a table's `suffix` cells [def order 3, Suffix; sr §6.1 `restInLastColumn`; rem P3] Tests: `SuffixCsv.TheCellsAreOneListOfCommands`, `SuffixSchema`
- [x] Pivoted tables: any axis pivots into the headers (clip table pivots a key; phase table pivots the output, cells are bands) [def Pivot; cat §2.6; arc §5.2; cook §2.2] Tests: `PivotCsv`
- [x] Flag domain from pivot headers: first column None = 0, each next column the next bit, `A|B` combos OR earlier columns; headers are distinct flag sets [cook §2.2; arc §5.2; def The None header] Tests: `PivotCsv.AnimationsLookupFixturePicksAnimationsCells`, `PivotCsv.EqualFlagSetsAreRefusedNamingBothHeaders`, `PivotCsv.ALaterNoneHeaderIsRefusedNotMadeAFlagBit`
- [x] Tables and machines from one YAML document, rows from named CSV texts: Tabular's part of a `.tabular` bundle [arc §5.1; def Surface] (`DocumentFromYaml`, the CSV loaders; the bundle file's container is under Host adapter) Tests: `MachineDefinition.TheGrazerIsWellFormed`, `GrazerMachine.TheSpecsBrainLoadsAndPrepares`

## 3. Queries: matching and picking

- [x] Flags match: all-in-input first, then the largest `R & F` as an integer (bit order is priority), then the larger `R`; else the required `R == 0` default row; ties go to row order; pivot headers are distinct flag sets [def Flag matching; arc §3.2 `best`; cook §5] Tests: `PickBest.RanksLikeAnimationsLookupBest`, `PickBest.AFullMatchBeatsALargerPartialOne`, `PickBest.EqualRanksGoToRowOrder`
- [x] Ranked fall-through: keys select candidates, flags rank them, take the best row whose other conditions hold, down to the default [def Picking] Tests: `PickBest.FallsThroughARowWhoseScriptConditionFails`, `PickBest.BandsAreTestedBeforeTheScript`
- [x] Native condition kinds on Tabular's own types only: flags subset, band (half-open `start <= x < end`, as animation's phase bands; a comparison against a literal is a band reaching its input's range end), edge (crossing over the previous..now interval) [def A row, Edges, What it is not; arc §3.1; cat §2.6; rem P13] (an edge is a band marked `edge` that holds when the clamped input crossed into the band from previous to now, so a fast input never skips a short band; best, all and or tables carry one; an edge row is conditional to the proof, as a scripted row is; `pick_query::previous` feeds the interpreted picks; a `cyclic` input wraps into its range and is crossed the shorter way around, owner 2026-10-19) Tests: `PickBest`, `PickOr.EdgesAreHalfOpenAndNanHoldsNothing`, `EdgePick`, `EdgeSchema`, `EdgeLayout`, `CyclicSchema`, `CyclicPick`, `CyclicLayout`, `CyclicMachine`, `CyclicParity`
- [x] At most one script condition per row, any number of native ones [def A row] Tests: `PickSchema.OneRankingColumnAndOneScriptCondition`
- [x] Weighted tables: keys + one weight + outputs, no conditions; draw across the selected rows [def Picking; c3 §5.9; rem P5] (`pick: weighted`, `PickWeighted`) Tests: `PickWeighted`, `PickWeightedLoad`, `PickWeightedSchema`
- [x] Pivoted flag outputs OR together, as animation phase tables do (`PipelineClosure::eval_phase_bits`): headers are flags, cells are bands, blank cells contribute nothing; every row whose key matches and whose band holds ORs in its column's bit; several rows may share a key and accumulate; nothing holding gives 0, so an OR table is total with no default row [def Picking; `src/Animation/authoring-form-spec.md` §Phase tables] Tests: `PickOr.MatchesAnimationsPhaseBits`, `PickOr.OverlappingBandsOrTogether`
- [x] Pick "all": every matching row runs in row order [arc §3.2; cat §2.6] Tests: `PickAll`
- [x] Display relations: one relation per question, each total [def Picking schema refusals (R2); cat D1; rem R2] (R2 accepted: nothing new in Tabular; the display-selection example is two mutable best tables, `owner.display` taking its domain from `display.display`) Tests: `DisplaySelection`

## 4. Load-time checks

Whatever the loader checks, an author can know by reading the table [def promise 2].

- [x] Totality over finite key domains; a table with opaque conditions must end in an unconditional row or an explicit hold [def promise 1, Key domains, The layout is the proof; cat §2.6; rem P10] (best tables, read-only and mutable: every key declares its domain (members, another table's column, rows, message); every tuple of the domain has a total run, orphans are reported, a foreign domain re-checks its dependents, a bare retract reports instead of refusing. A machine's `Prepare` runs the same check over the domain each clause's sources close; an explicit hold is a row with no values. Or and all tables may answer nothing; a weighted run has a positive total by its load refusal (R7)) Tests: `DomainTotality`, `KeyDomain`, `PickBest.AKeyRunWithoutADefaultIsRefusedAtLoad`, `GrazerMachine.AStateMemberWithNoRunIsRefused`, `MachinePrepare`
- [x] Shadowed / unreachable rows refused (e.g. an earlier row with no conditions makes later rows dead) [arc §2.9; sr §6.1, §7] (best tables, on load, insert and `Prepare`; a row in no list of the run layout; partial overlaps are priority within one owner; between owners, equal-flag overlaps are refused) Tests: `PickBest.ARowEarlierRowsCoverIsRefusedNamingThem`, `MutableBest.AnInsertedRowAnEarlierRowCoversIsRefused`, `Coverage`
- [x] Within one step, tables that feed each other form a DAG [def promise 2; attach §4; rem P11] (a clause names only earlier clauses of its list, so the DAG holds by how a machine is written; a clause naming itself or a later one is refused at definition build, naming the machine, list and clause) Tests: `GrazerMachine.AClauseNamingALaterClauseIsRefusedAtDefinitionBuild`, `GrazerMachine.AClauseNamingItselfIsRefusedAtDefinitionBuild`
- [x] Two tables writing one state column are ordered, never ambiguous [arc §2.6; attach §4; c3 §5.4; rem R4] (clauses are ordered by position and the later write wins, entry's on top of the step list's; one clause writing a column twice is refused) Tests: `GrazerMachine.TwoClausesWritingOneColumnAreOrderedByPosition`, `MachineStep.OfTwoClausesSettingOneColumnTheLaterWins`

## 5. Host interface

- [x] Script conditions (anything with a wire, anything complicated) reach the host only through the loader's `condition(string_view)` function (§2), with no symbol table; native conditions (`a..b` bands, flag sets) are read by Tabular and never go through it [def Host interface, owner 2026-10-16] Tests: `RowsCsv.ConditionCellsGoThroughTheFunction`, `RowsCsv.ABlankConditionCellIsNoConditionAndNeverReachesTheFunction`
- [x] Input resolution for every name Tabular does not own (wires are the host's) [def Host interface; cat §2.3; rem P8] (`host_binding::resolve`, called once per machine input at `Bind`, returns an `input_getter` read with one indirect call per step; no symbol table) Tests: `MachineBind`
- [x] Emitted commands and messages are returned, not sunk [def Host interface, step item 7; rem P12; commands-and-messages spec Part 2] (`Step` and `Rebind` return a `std::span<emitted const>` of `{list, table, command}`, `instance::Emitted()` the last run's, creation's included; step then exit then entry, each in clause order; a per-instance buffer sized for every clause that can emit, so a step allocates nothing; a rebind keeps the old binding until the next run; `host_binding::sink` removed; a message is a command, `(message ...)`) Tests: `MachineResults`, `GrazerEffects.EveryEntryAndExitIsReturnedInOrder`, `MachineRebind.TheOldExitsCommandsOutliveTheOldBindingUntilTheNextRun`, `GrazerEffects.StepsThatEmitTakeMessagesExitAndRepeatReportsAllocateNothing`
- [x] A failing condition (raise, null path) holds the table for the step, reported once per (table, row) [arc §2.11; sr §6.3; rem P15] (a condition that cannot answer, a NaN ranged input, an over-wide flag set, a key with no run or a getter that cannot answer leaves the clause unwritten, and clauses reading it hold too; `host_binding::report` hears each once per instance with the host's own error; a cascaded hold is not reported, and a repeat allocates nothing) Tests: `MachineReport`, `GrazerEffects.ARaisingConditionHoldsAndIsReportedOnce`, `GrazerStep.ACornerCheckThatCannotAnswerHoldsTheBrain`

## 6. Machines

- [x] Definition (immutable) → Binding (per definition × object type: resolved inputs) → Instance (per object: state vector, edge history) [def Lifetimes; cat §2.2; rem P7, P8] (`create_machine`, YAML `machines:`, `CheckMachine`; `Prepare` returning an immutable `Machine` that holds the versions it read and can say it is stale; `Bind` returning a shared `Binding` of one getter per input; the snake_case `instance` holding the current and next state vectors and each input's previous value) Tests: `MachineDefinition`, `MachinePrepare`, `MachineBind`, `GrazerStep.TheInstanceKeepsItsBindingAndMachineForStaleness`
- [x] State vector declared in the definition, owned by the table, not mirrored in script [sr §4.1; memory: no script mirror] (hash columns with members and an initial member, int and double columns with an initial value; `instance::State(column)` reads it by index; script reading it by name is under Host adapter) Tests: `MachineDefinition.AHashStatesInitialValueIsAMember`, `MachineStep.CreationRunsCreateThenEntryAgainstTheInitialState`
- [x] Values write the *next* state vector, swapped in at the end of the step; next state always names a member, never computed from a state [def A row; rem P9] (a step copies current to next, clauses write next, next becomes current; a written hash is a member by `Prepare`'s check) Tests: `MachineStep.AChainSettlesInOneStepBecauseStateIsReadFromNext`, `MachineStep.AColumnNoClauseWritesHolds`, `GrazerMachine.ANextStateThatIsNotAMemberIsRefused`
- [x] Transition tables and entry tables [def Structure, Machines] (ordinary clauses under `step`, `exit` and `entry`; `Step` runs the step list, then, when a hash state column changed, exit keyed on the state being left and entry on the one entered, entry's writes last; neither may set a hash state column; `CreateInstance` runs create, then entry, and no exit) Tests: `MachineStep.EntryRunsOncePerStepThatChangesAHashState`, `MachineStep.AnEntryClauseMayNotSetAHashState`, `MachineExit`
- [x] Bridges: the steps between two states [def Structure, Bridges are states; cat §2.1; rem R5] (R5(b): bridges are states, a consequence with no code of its own; a bridge only the animation system needs stays its suffix command, R5(a). A bounded repeat is a command the host runs, under Host adapter) Tests: `MachineBridge.ABridgeIsStatesThatMoveAlongWhenTheClipEnds`, `MachineBridge.ABridgeStateWithNoRowsIsRefusedByTheMachinesProof`
- [x] State-function table: a state maps to an ordinary host script function, which may suspend; the machine holds the state until it returns [def State functions; rem R6] (no table kind, a consequence: an entry table's command starts it, an exit table's aborts it, its return is a message a step row keys on, and the state's blank run holds it meanwhile) Tests: `StateFunction.EntryStartsItTheMachineHoldsUntilItReturnsAndExitAbortsIt`
- [x] Function-chain table: the chain of functions between two functions [def State functions; rem R6] (bridge states whose entry commands start functions and whose rows move on the return message; Tabular cannot tell a function's command or message from a clip's, so the bridge machinery is the evidence; rem Open owner decisions asks whether that suffices) Tests: `MachineBridge.AFunctionChainIsStatesThatStartFunctionsAndMoveAlongOnTheReturn`
- [x] State functions are aborted and their state re-entered on reload [def State functions, Reload] (a consequence of `Rebind`: the old binding's exit aborts, then the new binding's entry starts, both returned by the one call) Tests: `StateFunction.AReloadAbortsItWithTheOldCodeAndStartsItWithTheNew`
- [x] "Exactly one row" proved for the machine's SELECT, for input values in the table's declared domain, then compiled to run fast [def A machine is a SELECT, Machines; sr §6.4; guard §3; rem P10, R8] (proved and laid out by `Prepare`; the compiled form is that layout, which `Step` runs: binary search to the run, ranked flag groups, binary search to the cell, its list. An input value with no run holds the clause. Whether the searches become a direct index built at `Prepare` is open, def Undecided) Tests: `GrazerMachine`, `MachinePrepare`, `StepParity`
- [x] Hot reload migrates instance state by name: keep, reset to initial, or drop [def promise 5, Reload; sr §4.4; rem P14] (`Rebind(instance, binding, self, seed)`: old exit, migration by name, new entry; edge history kept by input name and type; a reload crosses no edge; it cannot be refused) Tests: `MachineRebind`, `GrazerReload`
- [x] Several machines per object as orthogonal regions [arc §4.1] (several instances composited into one host object, each stepped with that object as `self`) Tests: `MachineStep.SeveralMachinesOnOneObjectAreSeveralInstances`

## 7. Commands, messages, randomness

- [x] Commands: the command column stores the s-expression text, returned when an `emit` clause answers [def A row Suffix] Tests: `MachineResults.CommandsComeBackInTheOrderTheListsRan`
- [x] Commands as lists: suffix cells joined and read as a sequence of s-expressions, wrapped into one list the store keeps; an empty suffix is `()`; a cell of several items outside parentheses is wrapped, a single word stays a word, and one that is not whole by itself is refused; a declared command column reads its cell by the same rule, so every command column is a list of commands; the store checks only that the text is exactly one list (balanced parentheses, terminated strings with `\"` and `\\` escapes), naming the line in CSV [def Suffix; commands-and-messages spec Part 2] Tests: `SuffixCsv.ACommandAcrossTwoCellsReadsAsInOne`, `SuffixCsv.AnEmptySuffixIsTheEmptyList`, `SuffixCsv.ACellOfSeveralItemsIsWrappedAndAWordStaysAWord`, `SuffixCsv.UnbalancedParenthesesAreRefusedNamingTheLine`, `SuffixCsv.AnUnterminatedStringIsRefusedNamingTheLine`, `SuffixCsv.ABridgeRowIsOneListOfStepsAndMessages`, `SuffixCsv.ACellOfSeveralItemsThatIsNotWholeIsRefused`, `SuffixCsv.CellsThatJoinIntoTwoListsAreRefused`, `SuffixCsv.AnEmptyListCellAddsNothing`, `SuffixCsv.ADeclaredCommandColumnIsAListOfCommandsToo`, `SuffixStore`
- [x] A table absorbs a message, one at a time, in arrival order [def Machines, Messages; rem R3] (`Step` takes one `message{type, arguments}`; arrival order is the host's queue; a table keys on the type through a `domain: message` key whose blank cell is no message; a type with no run falls through to the blank run in `PickBest` and `Step` alike; `instance::Absorbed()` tells the host whether to route the message to script; the design is under owner review, def Undecided) Tests: `MachineMessage`, `MachineStep.AMessageNoRowKeysOnFallsThroughToTheBlankRun`
- [x] Deterministic randomness: counter-based draw from the host's seed, salted per draw site, no stored generator [def promise 4; cat §2.5; rem P4] (`Draw`, `DrawUnit`, `DrawSite`) Tests: `Draw`, `DrawUnit`, `PickWeighted.ADrawIsDeterministicInSeedAndSite`, `StepParity.ADrawSiteSaltsTheMachineTheListAndTheClause`

## 8. Budgets and evidence

- [x] A measured number and a failing benchmark for each: build a definition, bind, create an instance, step (per row), relation insert / retract / query [def Budgets; retro; rem P17] (19 benchmarks in `Tabular/bench/`, time and memory, gated by `scripts/test-tabular-budgets.sh` at `-O2` on one stated machine; each budget is the measurement with its margin; the gate fails over budget, on a measurement of nothing, and when definition.md's table drifts; the Debug suite runs them small without thresholds) Tests: `TabularBudget`
- [x] Parity / oracle tests with reddening mutations for any compiled path [cook §7; sr §9; guard §6] (`Step` against `PickBest`, `PickOr` and `PickWeighted` in every list, randomized; each helper both sides share was mutated and turned a non-parity test red; see def Parity) Tests: `StepParity`, `EdgeParity`, `ListParity`
- [x] Provenance: the last few picks per object (table, row, inputs), debug only [arc §2.10; rem P16] (`instance::Provenance()`, a ring of the last 16 clause runs with up to 8 values each; on with `TABULAR_PROVENANCE`, which Tabular's generated `tabular/build_config.h` sets in Debug only; kept across `Rebind`; allocation-free; absent from release instances) Tests: `Provenance`

## Host adapter (not Tabular)

`definition.md` assigns these to the subsystem that has Tabular (What it is not; The host
interface; rem §8, R1, R3, R9). They are listed so they are found, not as Tabular's debt.

- `.tabular` bundle: the one file holding YAML definitions plus CSV sections, and splitting it into named texts [arc §5.1; def What it is not]
- Mutable-table writes from s-expressions and script expressions (spawn tables, gambits, screen ownership): the host, running a returned command, turns it into `insert_row` [def Two kinds; rem §3]
- Compile refuses any snippet it cannot prove halts; read-only snippets call only read-only functions; functions that take a script callback are refused [def Host interface; guard Rulings]
- How a host-compiled snippet sees wires (free wire names as parameters the wirer's closure forces) [def Host interface; cat §3]
- Snapshot rule: wires read other entities' committed state as of step start [c3 §8.5]
- Query order for unordered results (nearest, then stable id) [c3 §8.9; rem R1]
- Fan-out over a query result [c3 §8.1; rem R1]
- Whether a command's consumer compiles its s-expression at bind, so a step enqueues a prebuilt record [rem R9]
- `random()` in a table snippet and in ordinary script using the same deterministic generator, and how `random(...)` inside a command is drawn [rem R9]
- A delayed message arriving after its state ended and was re-entered: the instance's entry count captured by `:delay`, and `message_stale` as an input flag the host supplies [rem R3]
- Script reading a machine's state by name (`instance::State` reads by index) [sr §4.1]
- Running state functions: start on the entry command, abort on every change of the state whatever exit emits, send the return message [def State functions, The host interface]
- A bounded repeat (a head nod 1-4 times): a command such as `(anim nod :repeat random(1, 4))`, not rows [def The host's part]

## Consequences

These are not features to build. Each goes red to green on its own once the features it follows
from are built; a test for one is a test of those. `[x]` when everything it follows from is `[x]`.

- [x] Handles fit in a table: a `uint64_t` punned into an `int64_t` column, no handle type ← typed columns (§1) [def Column types, owner 2026-10-16] (no test of its own; `RelationFind.ExtremeValuesLookUpAndRetract` holds the extreme values)
- [x] Unique is provable at load ← a unique clash is refused (§1) + read-only rows arrive only by load (§1, RCU) [arc §3.2]
- [x] A failed rebuild keeps the old version running ← read-only versions replaced only after the new one checks out (§1, RCU) [def promise 5; attach §6] Tests: `RelationLoad.LoadIsAllOrNothing`, `GrazerReload.AFailedPrepareLeavesTheOldBindingRunning`
- [x] Conditions are `{function, data, release}` closures, released with their row ← the condition function returns the `closure` type `relation.h` already defines (§5) [def Host interface; cat §2.3] Tests: `RelationLoadConditions`
- [x] Every query picks a row (totality at run time) ← flags match with its required default row (§3) + ranked fall-through down to the default (§3) + the totality check (§4) [def promise 1; cat §2.6]; a machine clause over a tuple of its proved domain always picks, and one whose input-bound key has no run, or whose pick fails, holds
- [x] A missing default (`R == 0`) row is refused ← flags match requires it (§3) [def Flag matching]; checked, as totality, for every key tuple of the domain a load that brings rows publishes; a bare retract is unchecked and reports Tests: `PickBest.AKeyRunWithoutADefaultIsRefusedAtLoad`, `DomainTotality.ABareRetractIsNeverRefusedAndReportsWhatItLeaves`
- [x] No tie is ever ambiguous ← flags match breaks ties by row order (§3) [def Flag matching] Tests: `PickBest.EqualRanksGoToRowOrder`
- [x] Band gaps and overlaps are caught ← a gap fails the totality check (§4); an overlap is a shadowed row (§4) [c3 §5.12]; a partial overlap is priority within one owner; between owners, equal-flag overlaps are refused Tests: `PickBest.AGapInTheQuiescentRowsIsRefusedNamingAValue`, `PickBest.APartialOverlapIsPriorityNotDeath`
- [x] Tables run in DAG order within a step, so a phase → lookup → bridge chain settles in one step ← tables that feed each other form a DAG (§4) [def A row]; from clause order plus in-step reads of next and of earlier clauses' answers, with no settling loop Tests: `MachineStep.AChainSettlesInOneStepBecauseStateIsReadFromNext`, `MachinePrepare.AnOrClauseInAPhaseChainPrepares`
- [x] Triggers: at instance creation and on entering a state, every step, and when rows change ← entry and transition tables (§6) + reporting that rows changed (§1) + hot reload migrating by name (§6) [def Triggers, Reload]; a rows change is seen as `Machine::Stale`, and the host's `Prepare`, `Bind` and `Rebind` run the old exit and the new entry Tests: `GrazerReload.ARowsChangeIsStaleAndTheReloadRestartsTheStateWithTheNewRows`
- [x] Edges are machine state ← the Instance holds edge history (§6) + edge is a native condition kind (§3) [def Structure, Edges; cat §2.6] Tests: `MachineEdge`, `EdgeParity`, `GrazerEdge`
- [x] A seed per instance and step ← counter-based draws from the host's seed (§7) [def Host interface, promise 4]; `Step` takes the seed and a weighted clause draws at `DrawSite(machine, list, clause)` Tests: `StepParity.ADrawSiteSaltsTheMachineTheListAndTheClause`
- [x] Delays are a message to self with `:delay` ← no clock in Tabular (Rejected) + returned commands carry messages (§5) [def What it is not]; the grazer's panic entry emits `(message this "calm" :delay random(0.4, 1.0))` and the calm message, when the host delivers it, moves the brain Tests: `GrazerEffects.EveryEntryAndExitIsReturnedInOrder`

## Rejected

Do not rebuild these; the contract ruled them out.

- A cooked `.kre_tabular` binary: text is parsed at load [retro; vs cook, arc §6.1, sr §3.2]
- Tabular parsing or compiling AngelScript conditions itself (fork parser, operand resolution): the host owns the language [def What it is not; vs arc §2.11, sr §6.2]
- Native numeric comparisons that combine inputs, do arithmetic or mirror host types [def What it is not; retro]
- Reading another object's table state through paths (`lamp.brain.state`) [cat §2.4; vs sr §5]
- Script records bound to a Tabular instance (`Tabular(path, root, record)`) [sr supersedes attach §3.1]
- Linear scan only, no index [def A row: keys are indexed; vs arc §3.3]
- Timers and counters inside Tabular [def What it is not]
- Loading a mutable table from CSV, or mutating a read-only one [def Two kinds]
- State-function and function-chain tables as table kinds: they are entry, exit and step clauses (R6) [def State functions]
- A dedicated cycle check over a machine's tables: a clause names only earlier clauses (P11) [def Machines]

## Non-goals

Query language or ad-hoc queries; Datalog or recursion; arithmetic in cells or sequences as
rows; a scheduler; file IO; owning wires, the bundle file or reload policy; a rule DSL outside
the script language [arc §7; def What it is not; memory: code is data].
