#ifndef TABULAR_MACHINE_INTERNAL_H
#define TABULAR_MACHINE_INTERNAL_H

// A machine's resolved definition and what Prepare keeps of it. Internal to
// Tabular (and its tests): nothing outside src/ includes it.

#include "tabular/machine.h"
#include "layout.h"
#include "table_layout.h"
#include "Support/shared_array.hpp"

#include <array>
#include <memory>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tabular
{

namespace detail
{

// A source with its names resolved: index is the state column, the machine
// input, or the earlier clause; column is that clause table's value column.
struct resolved_source
{
	source_kind kind    = source_kind::Literal;
	uint32_t    index   = UINT32_MAX;
	uint32_t    column  = UINT32_MAX;
	cell        literal = cell::Integer(0);
	// Clause, when the clause read is overridden: the override's column of
	// the same name, read instead when the override hit (the coalesced
	// answer); UINT32_MAX when its table has none.
	uint32_t    overrideColumn = UINT32_MAX;
};

struct resolved_clause
{
	uint32_t                                    table          = UINT32_MAX;
	shared_array<resolved_source>               keys;     // per key column, in column order
	shared_array<resolved_source>               inputs;   // per table input, in input order
	// (value column, state column), in the order written: a later write of one
	// column wins, as a later clause's does.
	shared_array<std::pair<uint32_t, uint32_t>> sets;
	uint32_t                                    emit           = UINT32_MAX;   // a Command column
	// The clause this one overrides, the one before it; and the clause that
	// overrides this one, the one after it. UINT32_MAX for none.
	uint32_t                                    overrides      = UINT32_MAX;
	uint32_t                                    overriddenBy   = UINT32_MAX;
	// Entry and exit only: the hash state columns whose change runs this
	// clause on a step (Part 4: entry and exit run per column). A clause runs
	// when a column it keys on as state.<col> changed; clauses of the list
	// that read one another run as one, on the union of their columns; and a
	// group that keys on no state column runs on any change, as every hash
	// column is listed. Sorted, distinct.
	immutable_array<uint32_t>                   enteredOn;
	// The table's fall-through keys, the slots a step's run lookup retries with
	// 0 (blank) when the keys as read have no run, in the order they give way.
	// An override's are its own keys first, then its base's fall-through keys
	// in the base's order (see ownFallThrough).
	fall_through_keys                           fallThrough;
	// An override: how many of fallThrough are keys its base does not have.
	// The rest are the base's, so the retry count shifted right by this is the
	// level, comparable with the base's own retry count: a hit counts only at a
	// level no less specific than the one the base answered at (owner,
	// 2026-10-20: the more specific row wins, and the override wins a tie).
	uint32_t                                    ownFallThrough = 0;
};

struct state_column
{
	std::string           name;
	column_type           type    = column_type::Hash;
	shared_array<int64_t> members;   // Hash: sorted, distinct
	cell                  initial = cell::Integer(0);
};

struct machine_def
{
	std::string                                              name;
	shared_array<state_column>                               state;
	shared_array<input_def>                                  inputs;
	std::array<shared_array<resolved_clause>, kTriggerCount> lists;
};

tl::expected<machine_def, ErrorContext> ResolveMachine(create_machine const& definition, schema const& tables);

// Where a refusal is, in the author's terms, for the file reader and the
// definition build alike: "machine 'grazer' step clause 0 (table 'grazer_brain')".
std::string ClausePlace(std::string_view machine, size_t list, size_t index, std::string_view table);

// One clause as Prepare proved it: the version it read and every run of its
// key domain. tuples are the domain's key tuples in key order, keys end to
// end. A best clause lays every run out in one set of arrays: runs.runs[t] is
// tuple t's groups, and the lists name the version's row numbers. An or or a
// weighted clause is answered from the whole run, so rowRuns[t] is tuple t's
// version rows instead (an or tuple with no run is empty and answers 0).
struct prepared_clause
{
	counted_ptr<TableVersion const> version;
	size_t                          tupleCount = 0;
	immutable_array<int64_t>        tuples;
	frozen_runs                     runs;      // best
	immutable_array<row_span>       rowRuns;   // or, weighted
	// What a step feeds a best layout: per axis, the table input it reads, and
	// the input the ranking flags column reads (UINT32_MAX for none).
	immutable_array<uint32_t>       axisInputs;
	uint32_t                        rankingInput = UINT32_MAX;
};

// A table whose generation the proof depends on, and the generation it read.
struct watched_table
{
	uint32_t table      = 0;
	uint64_t generation = 0;
};

// How WouldRepeat tells whether a machine input moved. exact compares the value
// read: any use but a band's (a key, a flags rank, a cyclic table input, whose
// wrapped cell is no interval of the value). Otherwise only the input's cell
// matters, between the sorted, distinct edges of every band it feeds, across
// the machine's tables, at [first, first + count) of repeat_plan::edges.
struct input_watch
{
	uint32_t first = 0;
	uint32_t count = 0;
	bool     exact = false;
};

// What Prepare knows of whether a step can repeat. Lazy getters are the
// binding's, so Bind adds that exclusion.
struct repeat_plan
{
	// No table of the machine is weighted, has an edge band or a script
	// condition, or is mutable.
	bool                         repeatable = false;
	immutable_array<input_watch> inputs;    // per machine input; empty unless repeatable
	immutable_array<float>       edges;
};

}

struct Machine::Data
{
	detail::machine_def                                              def;
	std::array<shared_array<detail::prepared_clause>, kTriggerCount> lists;
	immutable_array<detail::watched_table>                           watched;   // by table, distinct
	detail::repeat_plan                                              repeat;
};

// Prepare builds a Machine and tests read one; both are outside the class.
struct machine_access
{
	using Data = Machine::Data;

	static Data const&                Of(Machine const& m) { return *m._data; }
	static counted_ptr<Machine const> Make(std::unique_ptr<Data>&& data);
};

}

#endif // TABULAR_MACHINE_INTERNAL_H
