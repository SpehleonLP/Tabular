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
// condition's, so a failure allocates only what the host's message does: one
// past the small-string size allocates on the host's side, not Tabular's.
struct input_getter
{
	cell (*read)(void* data, void const* self, ErrorContext* error_out) = nullptr;
	void* data                                                          = nullptr;
	void (*release)(void* data)                                         = nullptr;
};

// What the host supplies to bind a machine. resolve is called once per machine
// input, at Bind, with the input's name; it is the only place a name meets the
// host, and there is no symbol table. Slice 4 adds the sink for emitted
// commands (P12) and the report for failing picks (P15) here as further
// members, so Bind's signature does not change.
struct host_binding
{
	std::function<tl::expected<input_getter, ErrorContext>(std::string_view name)> resolve;
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

// Binds machine to host: calls host.resolve once per machine input, in
// declaration order. Refused, naming the machine and the input, when resolve is
// missing, when it refuses an input, or when it returns a getter with no read
// function. Bind adopts every getter resolve returns, succeed or fail: on a
// refusal those already returned are released.
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
	NotRun,       // its list has not run: an exit list, or entry on a step that entered no state
	Answered,     // it picked: row (best, weighted) or bits (or), and its sets wrote next
	NoRun,        // its key tuple has no run: a key value outside the domain Prepare proved
	Failed,       // the pick failed: a script condition could not answer, or a ranged input was NaN, or a flag set was wider than 32 bits
	SourceHeld,   // a source had no value: an earlier clause that did not answer, or an input whose getter failed
};

// A clause that did not answer wrote nothing: its table holds for the step.
// Slice 4 reads these to hand a picked row's suffix to the sink and to report
// a failing pick once per (table, row).
struct clause_answer
{
	clause_outcome outcome = clause_outcome::NotRun;
	uint32_t       row     = UINT32_MAX;   // best, weighted: the version row picked
	uint32_t       bits    = 0;            // or: the OR of the holding rows' flags
};

struct instance_access;

// One object's run of a bound machine: the current state vector, the next one
// a step writes, and what each clause answered. Composited into the host's
// object, never new'ed. Every buffer is sized when the instance is created, so
// a step allocates nothing. Move-only: a copy would be a second object that
// shares the first's history.
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
	std::vector<cell>          _keys;        // scratch: one clause's key values
	std::vector<cell>          _given;       // scratch: one clause's table inputs
	std::vector<float>         _axes;        // scratch: one best clause's clamped band inputs
};

// A new instance of binding with the given id, its state at each column's
// initial value. It reads every input once, then runs the create list and the
// entry list (creation enters the initial state) against the next vector, as
// a step runs its list, and makes the result current. seed draws for weighted
// clauses; self is the host object, handed to getters and script conditions.
// It cannot be refused: a clause that cannot answer holds, as in a step.
instance CreateInstance(Binding const& binding, uint64_t id, uint64_t seed, void const* self);

// One step. Reads every input once; copies current into next, so a column no
// clause writes holds; runs the step list in order, each clause reading state
// from next (so it sees what earlier clauses wrote this step) and an earlier
// clause's answer from this step, and writing its sets into next (a later
// write of a column wins). If any hash state column of next differs from
// current, a state was entered (so A -> B -> A within one step enters
// nothing), and the entry list runs once on top, against next, its writes
// winning over the step list's; an entry clause writes only int and double
// columns (the definition build refuses a hash), so entry never enters again.
// Then next becomes current. A clause that does not answer writes nothing (see
// clause_outcome). Allocates nothing, holds included (see input_getter for a
// host's own failure message).
void Step(instance& subject, void const* self, uint64_t seed);

}

#endif // TABULAR_INSTANCE_H
