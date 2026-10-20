#ifndef TABULAR_INSTANCE_H
#define TABULAR_INSTANCE_H

// A prepared machine bound to a host, and the instances that run it. The
// instance trades in the store's cells, so this includes relation.h; the
// Machine itself is only named.

#include "tabular/relation.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace tabular
{

class Machine;
enum class trigger : uint8_t;

// How a step reads one machine input: one indirect call, no name and no
// allocation. self is the host object the instance belongs to, as the host
// passes it to CreateInstance and Step, so a getter reads that object's own
// state (a wire behind self) without a lookup by id; it is also what a row's
// script condition is tested against. A getter that cannot answer (a wire
// whose path is null) fills error_out, and every clause reading that input
// holds this step. A hash, flag set or int is returned in cell::i, a double in
// cell::d. release frees data once, when the binding is dropped or refused.
// A step builds no error of its own and adds nothing to a getter's or a
// condition's, except the first time it reports one (see host_binding), so a
// repeated failure allocates only what the host's message does: one past the
// small-string size allocates on the host's side, not Tabular's.
struct input_getter
{
	cell (*read)(void* data, void const* self, ErrorContext* error_out) = nullptr;
	void* data                                                          = nullptr;
	void (*release)(void* data)                                         = nullptr;
};

// What the host supplies to bind a machine.
//
// resolve is called once per machine input, at Bind, with the input's name; it is the only
// place a name meets the host, and there is no symbol table.
//
// report receives each failure that held a clause (P15), once per instance
// for each (clause, row) whose script condition could not answer, (clause,
// table input) a pick refused (NaN for a ranged input, a flag set wider than
// 32 bits), clause whose key tuple had no run, and machine input whose getter
// failed. table is kNoTable for a getter, row kNoRow when no
// row is at fault. The error names the machine, the clause list and the
// clause (or the input), the table and the row or key, after the host's own
// message where there is one: a condition is asked once per evaluation, and
// its report carries what it said then. Building it allocates, the first
// time only. Optional: without it the failures still hold, unreported.
//
// Both are called from Step, CreateInstance and Rebind (the old binding's
// report for its exit, the new one's for its entry), on the thread running
// them; instances sharing a binding may step concurrently, so a host stepping
// them on several threads gives functions safe to call that way. Neither may
// step or rebind the instance it is told about.
//
// Commands are not handed to the host: each run returns them (see emitted).
inline constexpr uint32_t kNoTable = UINT32_MAX;
inline constexpr size_t   kNoRow   = SIZE_MAX;

struct host_binding
{
	std::function<tl::expected<input_getter, ErrorContext>(std::string_view name)> resolve;
	std::function<void(uint64_t instance, uint32_t table, size_t row, ErrorContext const& error)> report;
};

// Tabular has no message concept (commands-and-messages spec, Part 3). The
// host dispatches a message to a handler machine, whose inputs are that
// message type's payload, read by getters from the message, and steps it once
// per message; it routes a message it has no handler for to the object's
// script. A machine that branches on argument-free event types keys on a
// fall-through key fed by a hash input the host fills for one step. A message
// a machine sends is a command in a row's list.

// One command a run emitted (P12): a clause with an emit answered, and this
// is the list it is in (so a host tells an exit's abort from an entry's
// start), its table and the picked row's command: a list of commands, as
// every command column holds, declared or suffix (`((anim chew) (message food
// "eat"))`, `()` for none). A view into the version the binding that ran the clause holds;
// Tabular does not interpret it.
struct emitted
{
	trigger          list  = {};
	uint32_t         table = 0;
	std::string_view command;
};

struct binding_access;

// A machine bound to one host: a getter per input, resolved once. Immutable,
// shared by every instance of the object type, held by counted_ptr. It keeps
// its Machine, and so the table versions that Machine was proved against, for
// as long as it lives: a store change never reaches a running binding. The
// host asks machine().Stale(store) when the store has changed; a stale machine
// is prepared again, bound, and its instances moved over with Rebind.
class Binding
{
public:
	~Binding();
	Binding(Binding const&)            = delete;
	Binding& operator=(Binding const&) = delete;

	Machine const& machine() const;

	void AddRef() const { _refs.fetch_add(1, std::memory_order_relaxed); }
	void Release() const;

private:
	friend struct binding_access;
	struct Data;
	explicit Binding(std::unique_ptr<Data>&& data);

	std::unique_ptr<Data const> _data;
	mutable std::atomic<int>    _refs{1};
};

// Binds machine to host: calls host.resolve once per machine input a getter
// reads, in declaration order. Refused, naming the machine and the input, when
// resolve is missing, when it refuses an input, or when it returns a getter
// with no read function. Bind adopts every getter resolve returns, succeed or
// fail: on a refusal those already returned are released.
tl::expected<counted_ptr<Binding const>, ErrorContext> Bind(counted_ptr<Machine const> machine, host_binding&& host);

// The draw site of a weighted clause: the machine's name hashed (CityHash32)
// in the high word, then the clause list (bits 24..31) and the clause's
// position (its low 24 bits; no list comes near that). So two
// weighted clauses in one step, or two machines on one object stepped with
// one seed, draw different numbers, and a site survives a reload that keeps
// the machine's name and the clause's place. The host varies the seed per
// instance and per step.
uint64_t DrawSite(std::string_view machine, trigger list, uint32_t clause);

// What one clause did the last time its list ran.
enum class clause_outcome : uint8_t
{
	NotRun,       // its list has not run (exit or entry on a step that entered no state), it is an entry or exit clause none of whose columns changed, or it is an override whose clause did not answer
	Answered,     // it picked: row (best, weighted) or bits (or), and its sets wrote next
	NoRun,        // its key tuple has no run: a key value outside the domain Prepare proved (a fall-through key's value with none falls through to the blank run first)
	Failed,       // the pick failed: a script condition could not answer, or a ranged input was NaN, or a flag set was wider than 32 bits
	SourceHeld,   // a source had no value: an earlier clause that did not answer, or an input whose getter failed
	Missed,       // an override found no run for its key tuple: NULL, so the clause it overrides answers alone; not a hold, never reported
};

// A clause that did not answer wrote nothing and emitted nothing: its table
// holds for the step.
struct clause_answer
{
	clause_outcome outcome = clause_outcome::NotRun;
	// Its fall-through key's value had no run, and it looked up the blank run
	// instead (which answered, or is the NoRun).
	bool           fellThrough = false;
	// Answered (best, weighted): the version row picked. Failed: the row whose
	// script condition could not answer, or UINT32_MAX when an input was.
	uint32_t       row     = UINT32_MAX;
	uint32_t       bits    = 0;            // or, Answered: the OR of the holding rows' flags
	uint32_t       input   = UINT32_MAX;   // Failed on an input: the table input the pick refused
};

// Provenance (P16), a debug-build aid: each instance keeps its last
// kProvenanceDepth picks. Tabular's CMakeLists.txt generates
// <tabular/build_config.h> (1 in the Debug config, 0 in the release
// family), read only here so the switch reaches the units that use instances
// and no other command line; a build that defines TABULAR_PROVENANCE itself
// (the budget gate sets 0) keeps its own, and one with neither (a standalone
// build) gets none. In a build without it, the ring and Provenance() do not
// exist, so release instances carry no bytes for them and no caller can read
// an empty ring as "no picks". Every translation unit must agree: the instance
// type carries an ABI tag when provenance is on (and MSVC records the setting
// in each object), so a unit compiled the other way fails to link instead of
// reading another layout.
#ifndef TABULAR_PROVENANCE
#if __has_include(<tabular/build_config.h>)
#include <tabular/build_config.h>
#else
#define TABULAR_PROVENANCE 0
#endif
#endif

#if TABULAR_PROVENANCE && defined(__GNUC__)
#define TABULAR_PROVENANCE_ABI [[gnu::abi_tag("tabular_provenance")]]
#else
#define TABULAR_PROVENANCE_ABI
#endif

#if defined(_MSC_VER)
#if TABULAR_PROVENANCE
#pragma detect_mismatch("tabular_provenance", "1")
#else
#pragma detect_mismatch("tabular_provenance", "0")
#endif
#endif

#if TABULAR_PROVENANCE

inline constexpr size_t kProvenanceDepth  = 16;
// The values one entry keeps: enough for every clause the worked machines and
// the budget brain have (two keys, two inputs and their previous values); a
// wider clause keeps the first ones and says it was cut (Truncated).
inline constexpr size_t kProvenanceValues = 8;

// Which run of the instance a pick belongs to.
enum class provenance_run : uint8_t
{
	Create,
	Step,
	Rebind,   // the old binding's exit, then the new binding's entry
	Close,    // the exit list, as the host ends the instance
};

// One clause's pick, as the instance ran it. values holds what the clause read,
// in order: its keys, then its table inputs, then, for a table with edges, the
// previous value each table input crossed from (a non-edge input's is its own
// value). A clause held by a source (SourceHeld) read nothing whole, so it
// keeps no values; one refused on an input keeps keys and inputs, and no
// previous values, which were never taken.
struct provenance_entry
{
	uint64_t       pass  = 0;                       // 0 for creation, then one more per Step, Rebind or Close
	provenance_run run   = provenance_run::Create;
	trigger        list  = {};
	// The fall-through key's value had no run and the blank run was looked up
	// (answer.fellThrough); keys still hold the value, as read.
	bool           fellThrough   = false;
	uint8_t        keyCount      = 0;
	uint8_t        inputCount    = 0;
	uint8_t        previousCount = 0;
	uint32_t       clause = 0;                      // its place in list
	uint32_t       table  = 0;
	clause_answer  answer;
	std::array<cell, kProvenanceValues> values{};

	bool Truncated() const { return size_t(keyCount) + inputCount + previousCount > kProvenanceValues; }
	std::span<cell const> Keys() const { return Slice(0, keyCount); }
	std::span<cell const> Inputs() const { return Slice(keyCount, inputCount); }
	std::span<cell const> Previous() const { return Slice(size_t(keyCount) + inputCount, previousCount); }

private:
	std::span<cell const> Slice(size_t first, size_t count) const
	{
		size_t const begin = first < kProvenanceValues ? first : kProvenanceValues;
		size_t const end   = first + count < kProvenanceValues ? first + count : kProvenanceValues;
		return std::span<cell const>(values.data() + begin, end - begin);
	}
};

// The ring read oldest first: entry 0 is the oldest pick kept, size() - 1 the
// last. A view into the instance, valid until it next runs.
class provenance_view
{
public:
	provenance_view(std::span<provenance_entry const> ring, size_t first, size_t count)
		: _ring(ring), _first(first), _count(count) {}

	size_t size() const { return _count; }
	bool   empty() const { return _count == 0; }
	provenance_entry const& operator[](size_t i) const { return _ring[(_first + i) % _ring.size()]; }

	class iterator
	{
	public:
		iterator(provenance_view const* view, size_t at) : _view(view), _at(at) {}
		provenance_entry const& operator*() const { return (*_view)[_at]; }
		iterator& operator++() { ++_at; return *this; }
		bool operator==(iterator const& other) const { return _at == other._at; }

	private:
		provenance_view const* _view;
		size_t                 _at;
	};
	iterator begin() const { return iterator(this, 0); }
	iterator end() const { return iterator(this, _count); }

private:
	std::span<provenance_entry const> _ring;
	size_t                            _first = 0;
	size_t                            _count = 0;
};

#endif

struct instance_access;

// One object's run of a bound machine: the current state vector, the next one
// a step writes, what each clause answered, and which failures it has
// reported. Composited into the host's object, never new'ed. Every buffer is
// sized when the instance is created, so a step allocates nothing (a report's
// first occurrence builds its error). Move-only: a copy would be a second
// object that shares the first's history.
class TABULAR_PROVENANCE_ABI instance
{
public:
	instance(instance&&) noexcept;
	instance& operator=(instance&&) noexcept;
	instance(instance const&)            = delete;
	instance& operator=(instance const&) = delete;
	~instance();

	// binding, Answer and Step assert that the instance was not moved from:
	// using one that was is caller misuse.
	uint64_t       id() const { return _id; }
	Binding const& binding() const;

	// The current state, in the machine's state column order.
	size_t StateCount() const { return _current.size(); }
	cell   State(size_t column) const { return _current[column]; }

	// What clause did the last time list ran (creation, for create and entry
	// until a step).
	clause_answer Answer(trigger list, size_t clause) const;

	// What the last run (creation, a step or a rebind) emitted, in the order
	// its lists ran: the span Step and Rebind return, and creation's, which
	// CreateInstance cannot. Valid until the instance next runs, is moved or
	// destroyed: a rebind's exit commands view the old binding's versions, so
	// the instance keeps that binding until then. That run may so drop the
	// last reference to the old binding: its getters' release functions (and
	// whatever the Binding's Machine and versions free) then run inside that
	// Step, on the thread stepping the instance. A host that wants them freed
	// elsewhere keeps its own reference to the old binding and drops it there.
	std::span<emitted const> Emitted() const;

#if TABULAR_PROVENANCE
	// The last kProvenanceDepth clauses that ran, oldest first, across
	// creation, steps and rebinds (a rebind keeps the history: its entries name
	// the old machine's clauses, then the new one's). A clause whose list did
	// not run is not recorded. The ring is sized at creation, so recording
	// allocates nothing.
	provenance_view Provenance() const;
#endif

private:
	friend struct instance_access;
	instance() = default;

	counted_ptr<Binding const> _binding;
	uint64_t                   _id = 0;
	std::vector<cell>          _current;
	std::vector<cell>          _next;
	std::vector<cell>          _inputs;      // this step's input values, read once per step
	std::vector<uint8_t>       _inputRead;   // per input: its getter answered
	// Per input: what an edge reading it crosses from this step, the last
	// value read (a getter that answered, not NaN) on an earlier step or at
	// creation; NaN, which crosses nothing, until one is read (see Step).
	std::vector<cell>          _previous;
	std::vector<clause_answer> _answers;     // every list's clauses, end to end
	std::vector<uint64_t>      _reported;    // one bit per failure that is reported once (see host_binding)
	std::vector<cell>          _keys;        // scratch: one clause's key values
	std::vector<cell>          _given;       // scratch: one clause's table inputs
	std::vector<cell>          _givenBefore; // scratch: one clause's table inputs' previous values, for its edges
	std::vector<float>         _axes;        // scratch: one best clause's clamped band inputs
	// The last run's commands: room for every clause that can emit, so a step
	// that emits allocates nothing.
	std::vector<emitted>       _emitted;
	// How many of _emitted the last run filled. Zeroed when moved from, as the
	// vector is, so a moved-from instance never claims slots it no longer has.
	struct emitted_count
	{
		size_t n = 0;
		emitted_count() = default;
		emitted_count(emitted_count&& other) noexcept : n(other.n) { other.n = 0; }
		emitted_count& operator=(emitted_count&& other) noexcept
		{
			n       = other.n;
			other.n = 0;
			return *this;
		}
	};
	emitted_count              _emittedCount;
	counted_ptr<Binding const> _left;        // the binding a rebind left, while its exit's commands are returned
#if TABULAR_PROVENANCE
	std::vector<provenance_entry> _provenance;           // the ring, kProvenanceDepth entries
	size_t                        _provenanceNext  = 0;  // where the next pick goes
	size_t                        _provenanceCount = 0;
	uint64_t                      _pass            = 0;
	provenance_run                _run             = provenance_run::Create;
#endif
};

// A new instance of binding with the given id, its state at each column's
// initial value. It reads every input once, then runs the
// create list and the entry list (creation enters the initial state; no exit
// runs) against the next vector, as a step runs its lists, makes the result
// current, and returns in Emitted() what the create, then the entry clauses
// emitted. seed draws for weighted clauses; self is the host object, handed to
// getters and script conditions. It cannot be refused: a clause that cannot
// answer holds, as in a step. The inputs read here are the previous values
// the first step's edges cross from, and the create and entry clauses see
// them as previous too, so creation crosses no edge: the first step sees a
// crossing only if its input moved since creation.
instance CreateInstance(Binding const& binding, uint64_t id, uint64_t seed, void const* self);

// One step.
//
// 1. Reads every input once, from its getter.
// 2. Copies current into next, so a column no clause writes holds.
// 3. Runs the step list in order, each clause reading state from next (so it
//    sees what earlier clauses wrote this step) and an earlier clause's answer
//    from this step, and writing its sets into next (a later write wins). A
//    clause with fall-through keys finds the run for its keys, or, when they
//    have none, retries with blanks (0) in them, in the order they give way
//    (see FindFallingThrough). An override runs only when the clause it
//    overrides answered, keys on the state that clause read (its sets wait
//    for the override), and on a hit writes its own sets over that clause's
//    and replaces its emit; a miss writes nothing and is no hold.
// 4. If any hash state column of next differs from current, a state was
//    entered (so A -> B -> A within one step enters nothing). Then the exit
//    list runs, its state sources reading current (the state being left), and
//    then the entry list, reading next; both write next, entry's writes last.
//    Of each, only the clauses entered on a column that changed run (Part 4:
//    entry and exit run per column; the others answer NotRun). Neither writes
//    a hash state column (the definition build refuses it), so neither enters
//    again. Creation, a rebind and Close run their entry or exit lists whole.
// 5. Next becomes current, and the step returns what the clauses that
//    answered emitted: the step list's, then exit's, then entry's, each list
//    in clause order (the same span as Emitted()).
//
// An edge (P13) crosses from its input's previous value: the last value read
// for it on an earlier step or at creation, whatever the clauses did with it,
// so a crossing on a step whose clause held is spent. A getter that failed or
// a NaN is no value read, and leaves the previous value where it was, so the
// crossing is measured on the next step from the last real value and is not
// skipped. An edge reads only a machine input with a getter (the definition
// build refuses any other source), so no other source keeps a previous value.
//
// A clause that does not answer writes and emits nothing (see clause_outcome)
// and is reported once (see host_binding). Allocates nothing, holds included,
// except a report's first occurrence (see input_getter for a host's own
// failure message).
std::span<emitted const> Step(instance& subject, void const* self, uint64_t seed);

// Moves subject onto binding (P14): a reload, which the host runs on each
// instance once it has prepared the changed machine again and bound it. Like a
// step that enters a state, with migration where the step list would be:
//
// 1. Leaving, under the old binding: every input is read once with its getter,
//    next is set to a copy of current, the old exit list runs
//    against current, and next becomes current. What exit emitted comes
//    first in the result, so the host hears the abort of what a state started
//    (R6) from the code that started it, before the new code starts it again.
// 2. Migration by name: each state column of the new machine takes the old
//    column of the same name, its value kept when the column has the same type
//    and, for a hash, the value is still a member, and otherwise its new
//    initial value (an int that became a double, or the reverse, is reset: a
//    conversion would be arithmetic the machine never wrote). A column no
//    longer declared is dropped. Every buffer is sized for the new binding,
//    the report-once memory is cleared (a new binding is a new set of clauses
//    and rows) and every clause answers NotRun.
// 3. Entering again, under the new binding: every input is read once with the
//    new getters, the new entry list runs against the migrated state, next
//    becomes current, and what entry emitted follows exit's in the result.
//    The create list does not run: the instance is not new.
//
// A reload is no movement of an input, so it crosses no edge and spends none:
// the exit and entry lists see each input's previous value as the value read
// for them, and afterwards an input the old machine also had (same name and
// type, read from a getter by both machines) keeps the previous value it had,
// so the next step crosses from the last value a step read. An input new to
// the machine, retyped, or one with no previous value yet, takes the value read here, as at creation. Previous values are
// raw, so a changed range is applied by the clamp where they are used.
//
// It cannot be refused: everything a binding reads it holds (its machine and
// the versions that machine was proved against), migration always lands on a
// valid state, and a clause that cannot answer holds as in a step. binding may
// be the one subject already has, which restarts its state. self and seed are
// as for Step: getters, script conditions and weighted draws need them.
// Allocates (it sizes the new buffers); it is not a step. The returned exit
// commands view the old binding's versions, so subject holds the old binding
// until its next run, and that run may drop the last reference to it (see
// Emitted()).
std::span<emitted const> Rebind(instance& subject, Binding const& binding, void const* self, uint64_t seed);

// Ends subject's run, as the host does when the object leaves (from any
// state: closing is not a transition, so no exit row is needed per state).
// Runs the leaving half of a rebind under subject's own binding: every input
// read once with its getter, crossing no edge, next set to a copy of current,
// the exit list run against current (the state being left), and next made
// current. Returns what exit emitted (the same span as Emitted()), so the
// host hears the aborts of what the state started; the host then runs them
// and destroys the instance. Like a step it cannot be refused and allocates
// nothing beyond a report's first occurrence; the other lists' answers are
// left as they were.
std::span<emitted const> Close(instance& subject, void const* self, uint64_t seed);

}

#endif // TABULAR_INSTANCE_H
