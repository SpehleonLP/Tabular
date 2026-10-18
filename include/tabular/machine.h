#ifndef TABULAR_MACHINE_H
#define TABULAR_MACHINE_H

#include "tabular/relation.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace tabular
{

// ---- Machine commands -------------------------------------------------------
// A machine is one query, prepared once: a state vector, the inputs the host
// supplies, and four ordered lists of clauses, each clause one read of one
// best, or or weighted table. Like the schema commands these are views, valid only during the
// call that consumes them, and names are resolved against the schema then.

// Where a clause takes a key or a table input from. Inside a step the instance
// supplies it, not the caller.
enum class source_kind : uint8_t
{
	State,     // a state column: the next vector as the step has written it so far
	Input,     // one of the machine's inputs
	Clause,    // a value column of the row an earlier clause of the same list picked
	Literal,   // a constant
};

struct source
{
	source_kind      kind = source_kind::Literal;
	// State: the state column. Input: the machine input. Clause: the value
	// column of the earlier clause's table that is read.
	std::string_view name;
	// Clause: the earlier clause's position in the same list. Only an earlier
	// one, so the clauses of a list form a DAG by how they are written.
	uint32_t         clause  = UINT32_MAX;
	// Literal: a value of the target's type, a hash as its CityHash32.
	cell             literal = cell::Integer(0);
};

// One `where` (target a key column) or `given` (target a table input) entry.
struct clause_bind
{
	std::string_view target;
	source           from;
};

// state column := the picked row's value column, written to the next vector.
struct set_state
{
	std::string_view state;
	std::string_view column;
};

struct add_clause
{
	std::string_view              table;   // a best, an or or a weighted table
	std::span<clause_bind const>  where;   // exactly one per key column
	std::span<clause_bind const>  given;   // exactly one per table input
	std::span<set_state const>    set;
	// A Command column of the table (its suffix included) whose text the step
	// hands to the host's sink; empty for none.
	std::string_view              emit;
};

// A state column. A hash names one of its members; an int or a double holds
// any value, so a key reading one keeps its table's declared domain.
struct add_state
{
	std::string_view      name;
	column_type           type    = column_type::Hash;   // Hash, Int or Double
	std::span<cell const> members;                       // Hash only: each as its CityHash32
	cell                  initial = cell::Integer(0);    // a Hash's must be a member
};

// When a clause list runs: on creating an instance, on entering a state, every
// step, on leaving a state.
enum class trigger : uint8_t
{
	Create,
	Entry,
	Step,
	Exit,
};
inline constexpr size_t kTriggerCount = 4;

struct create_machine
{
	std::string_view            name;
	std::span<add_state const>  state;
	// Named and typed like a table's inputs, and Hash or Int too, since a
	// machine input may feed a key: a message's type is a hash input.
	std::span<add_input const>  inputs;
	std::span<add_clause const> create;
	std::span<add_clause const> entry;
	std::span<add_clause const> step;
	std::span<add_clause const> exit;
};

// Whether definition is well formed against the schema: every name it uses
// exists, each clause reads a best, an or or a weighted table, every key and
// table input of each clause is bound once to a source of its type, sources
// name only earlier clauses, a literal for a ranged input is finite and in its
// range, sets read value columns of the state column's type, emit names a
// command column, an or clause is read only through its output and emits
// nothing, and each hash state's initial value is a member. No rows are read.
// Prepare checks this first.
tl::expected<void, ErrorContext> CheckMachine(create_machine const& definition, schema const& tables);

struct machine_access;

// A machine proved against the table versions it holds: immutable, shared by
// every instance of it. Re-Prepare it when Stale.
class Machine
{
public:
	~Machine();
	Machine(Machine const&)            = delete;
	Machine& operator=(Machine const&) = delete;

	std::string_view name() const;

	// Whether a table the machine reads, or a table one of its clauses' key
	// domains is read from, has changed in store since it was prepared, so its
	// proof no longer speaks for the store's tables. store must be the one it
	// was prepared against.
	bool Stale(relation_store const& store) const;

	void AddRef() const { _refs.fetch_add(1, std::memory_order_relaxed); }
	void Release() const;

private:
	friend struct machine_access;
	struct Data;
	explicit Machine(std::unique_ptr<Data>&& data);

	std::unique_ptr<Data const> _data;
	mutable std::atomic<int>    _refs{1};
};

// Proves the machine and lays it out. For each clause, its key domain is read
// from its sources: a hash state column is closed by its members, an earlier
// best or weighted clause by the values its rows can answer, a literal is one
// value; any other source (a machine input, an int or double state column, an
// or clause's flags) closes nothing, and the key keeps the table's declared
// domain (an or or a weighted table declares none: its rows). On a best table
// every tuple of that domain must have a run that is total over the table's
// inputs, with no dead row and no two owners' rows overlapping, which is the
// check a load makes, over this domain instead of the table's; on a weighted
// table every tuple must have a run; an or table answers 0 for a tuple with no
// run. Every hash a set can write must be a member of its state column. A
// refusal names the machine, the clause list and the clause. report, if
// given, is cleared and, if the machine is accepted, holds every run outside
// a clause's domain as an orphan, clause by clause in list order.
tl::expected<counted_ptr<Machine const>, ErrorContext> Prepare(create_machine const& definition,
	relation_store const& store, std::vector<run_note>* report = nullptr);

}

#endif // TABULAR_MACHINE_H
