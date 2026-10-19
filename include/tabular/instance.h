#ifndef TABULAR_INSTANCE_H
#define TABULAR_INSTANCE_H

// A prepared machine bound to a host, and the instances that run it. The
// instance trades in the store's cells, so this includes relation.h; the
// Machine itself is only named.

#include "tabular/relation.h"

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
// resolve is called once per machine input a getter reads (every input but
// those a message carries), at Bind, with the input's name; it is the only
// place a name meets the host, and there is no symbol table.
//
// sink receives every command a step emits (P12): for each clause with an
// emit that answered, the instance's id, the clause's table and the picked
// row's command text, a view into the version the binding holds (valid while
// the binding lives). Tabular does not interpret it. A machine that emits
// needs one; Bind refuses it otherwise, since a dropped command is a state
// function never started.
//
// report receives each failure that held a clause (P15), once per instance
// for each (clause, row) whose script condition could not answer, (clause,
// table input) a pick refused (NaN for a ranged input, a flag set wider than
// 32 bits), clause whose key tuple had no run, and machine input whose getter
// failed, and once for a message carrying more arguments than the machine
// declares. table is kNoTable for a getter or the message, row kNoRow when no
// row is at fault. The error names the machine, the clause list and the
// clause (or the input), the table and the row or key, after the host's own
// message where there is one: a condition is asked once per evaluation, and
// its report carries what it said then. Building it allocates, the first
// time only. Optional: without it the failures still hold, unreported.
//
// Both are called from Step and CreateInstance, on the thread running them;
// instances sharing a binding may step concurrently, so a host stepping them
// on several threads gives functions safe to call that way. Neither may step
// or rebind the instance it is told about.
inline constexpr uint32_t kNoTable = UINT32_MAX;
inline constexpr size_t   kNoRow   = SIZE_MAX;

struct host_binding
{
	std::function<tl::expected<input_getter, ErrorContext>(std::string_view name)> resolve;
	std::function<void(uint64_t instance, uint32_t table, std::string_view command)> sink;
	std::function<void(uint64_t instance, uint32_t table, size_t row, ErrorContext const& error)> report;
};

// A message for one step (R3): its type, as the CityHash32 of its name, and
// its arguments, in the order the machine's message_inputs declares them. The
// machine reads them as inputs. Arguments are positional slots every message
// type shares; a type's own arity is not declared. A step with no message has
// type 0, and an argument the message does not carry reads 0, as every
// argument does on a step with no message. Arguments past the declared ones
// are not read, and the instance reports that once (see host_binding). The
// host runs several messages in one tick as successive steps.
struct message
{
	uint32_t              type = 0;
	std::span<cell const> arguments;
};

struct binding_access;

// A machine bound to one host: a getter per input, resolved once. Immutable,
// shared by every instance of the object type, held by counted_ptr. It keeps
// its Machine, and so the table versions that Machine was proved against, for
// as long as it lives: a store change never reaches a running binding. The
// host asks machine().Stale(store) when the store has changed; a stale machine
// is prepared again and its instances rebound (slice 6).
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
// with no read function; and, naming the machine and a clause, when a clause
// emits and the host gives no sink. Bind adopts every getter resolve returns,
// succeed or fail: on a refusal those already returned are released.
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
	NotRun,       // its list has not run: exit or entry on a step that entered no state
	Answered,     // it picked: row (best, weighted) or bits (or), and its sets wrote next
	NoRun,        // its key tuple has no run: a key value outside the domain Prepare proved (a message type with none falls through to the blank run first)
	Failed,       // the pick failed: a script condition could not answer, or a ranged input was NaN, or a flag set was wider than 32 bits
	SourceHeld,   // a source had no value: an earlier clause that did not answer, or an input whose getter failed
};

// A clause that did not answer wrote nothing and emitted nothing: its table
// holds for the step.
struct clause_answer
{
	clause_outcome outcome = clause_outcome::NotRun;
	// Answered (best, weighted): the version row picked. Failed: the row whose
	// script condition could not answer, or UINT32_MAX when an input was.
	uint32_t       row     = UINT32_MAX;
	uint32_t       bits    = 0;            // or, Answered: the OR of the holding rows' flags
	uint32_t       input   = UINT32_MAX;   // Failed on an input: the table input the pick refused
};

struct instance_access;

// One object's run of a bound machine: the current state vector, the next one
// a step writes, what each clause answered, and which failures it has
// reported. Composited into the host's object, never new'ed. Every buffer is
// sized when the instance is created, so a step allocates nothing (a report's
// first occurrence builds its error). Move-only: a copy would be a second
// object that shares the first's history.
class instance
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

	// Whether the last step absorbed its message: some clause keyed on its
	// type found a run for that type (not the blank run it falls through to)
	// and answered. False on a step with no message. The host routes a message
	// the machine did not absorb to the object's script (R3).
	bool Absorbed() const { return _absorbed; }

private:
	friend struct instance_access;
	instance() = default;

	counted_ptr<Binding const> _binding;
	uint64_t                   _id = 0;
	std::vector<cell>          _current;
	std::vector<cell>          _next;
	std::vector<cell>          _inputs;      // this step's input values, read once per step
	std::vector<uint8_t>       _inputRead;   // per input: its getter answered
	std::vector<clause_answer> _answers;     // every list's clauses, end to end
	std::vector<uint64_t>      _reported;    // one bit per failure that is reported once (see host_binding)
	std::vector<cell>          _keys;        // scratch: one clause's key values
	std::vector<cell>          _given;       // scratch: one clause's table inputs
	std::vector<float>         _axes;        // scratch: one best clause's clamped band inputs
	bool                       _absorbed = false;
};

// A new instance of binding with the given id, its state at each column's
// initial value. It reads every input once (with no message), then runs the
// create list and the entry list (creation enters the initial state; no exit
// runs) against the next vector, as a step runs its lists, makes the result
// current, and hands the sink what the create, then the entry clauses
// emitted. seed draws for weighted clauses; self is the host object, handed to
// getters and script conditions. It cannot be refused: a clause that cannot
// answer holds, as in a step.
instance CreateInstance(Binding const& binding, uint64_t id, uint64_t seed, void const* self);

// One step, with msg as its message (none by default: type 0).
//
// 1. Reads every input once: a getter's, or the message's type and arguments.
// 2. Copies current into next, so a column no clause writes holds.
// 3. Runs the step list in order, each clause reading state from next (so it
//    sees what earlier clauses wrote this step) and an earlier clause's answer
//    from this step, and writing its sets into next (a later write wins). A
//    clause keyed on the message finds the run for its type, or, when the type
//    has none, the blank run (the same keys with 0 in the message key).
// 4. If any hash state column of next differs from current, a state was
//    entered (so A -> B -> A within one step enters nothing). Then the exit
//    list runs, its state sources reading current (the state being left), and
//    then the entry list, reading next; both write next, entry's writes last.
//    Neither writes a hash state column (the definition build refuses it), so
//    neither enters again.
// 5. Next becomes current, and the sink is handed what the clauses that
//    answered emitted: the step list's, then exit's, then entry's, each list
//    in clause order. So a sink sees the step's state already current.
//
// A clause that does not answer writes and emits nothing (see clause_outcome)
// and is reported once (see host_binding). Allocates nothing, holds included,
// except a report's first occurrence (see input_getter for a host's own
// failure message).
void Step(instance& subject, void const* self, uint64_t seed, message const& msg = {});

}

#endif // TABULAR_INSTANCE_H
