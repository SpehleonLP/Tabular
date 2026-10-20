#include "tabular/machine.h"

#include "float_text.h"
#include "machine_internal.h"
#include "relation_internal.h"
#include "version_checks.h"

#include "Support/strings.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace tabular
{

namespace
{

using detail::machine_def;
using detail::resolved_clause;
using detail::resolved_source;
using detail::state_column;

char const* const kListNames[kTriggerCount] = {"create", "entry", "step", "exit"};

std::span<add_clause const> ListOf(create_machine const& m, size_t list)
{
	switch (static_cast<trigger>(list))
	{
	case trigger::Create: return m.create;
	case trigger::Entry:  return m.entry;
	case trigger::Step:   return m.step;
	case trigger::Exit:   return m.exit;
	}
	return {};
}

// What a source can carry and a set can write: a value that is one integer or
// one double. Bands, vectors, strings, commands and conditions are not.
bool IsScalarValue(column_type type)
{
	return type == column_type::Hash || type == column_type::Flags || type == column_type::Int
		|| type == column_type::Double;
}

tl::expected<std::vector<input_def>, ErrorContext> ResolveInputs(create_machine const& m)
{
	std::vector<input_def> out;
	for (uint32_t i = 0; i < m.inputs.size(); ++i)
	{
		add_input const& in = m.inputs[i];
		if (in.name.empty())
		{
			return tl::make_unexpected(ErrorContext("input ", i, " needs a name"));
		}
		for (input_def const& earlier : out)
		{
			if (earlier.name == in.name)
			{
				return tl::make_unexpected(ErrorContext("input '", in.name, "' is declared twice"));
			}
		}
		if (!IsScalarValue(in.type))
		{
			return tl::make_unexpected(ErrorContext("input '", in.name, "' is a ", TypeName(in.type),
				"; a machine input is a hash, a flag set, an int or a double"));
		}
		if (in.ranged && in.type != column_type::Double)
		{
			return tl::make_unexpected(ErrorContext("input '", in.name, "' is a ", TypeName(in.type),
				"; only a double has a range"));
		}
		if (in.ranged && !(std::isfinite(in.lo) && std::isfinite(in.hi) && in.lo < in.hi))
		{
			return tl::make_unexpected(ErrorContext("input '", in.name, "' has range ", FloatText(in.lo), "..",
				FloatText(in.hi), "; a range is lo < hi, both finite"));
		}
		// The crossing and the wrap are a band's, so the table input whose bands
		// read this one declares them; here it would be accepted and ignored.
		if (in.cyclic)
		{
			return tl::make_unexpected(ErrorContext("input '", in.name,
				"' of the machine is cyclic; a table input whose bands read it declares that"));
		}
		out.push_back(input_def{std::string(in.name), in.type, in.ranged, in.lo, in.hi});
	}
	return out;
}

tl::expected<std::vector<state_column>, ErrorContext> ResolveState(create_machine const& m)
{
	std::vector<state_column> out;
	for (uint32_t i = 0; i < m.state.size(); ++i)
	{
		add_state const& in = m.state[i];
		if (in.name.empty())
		{
			return tl::make_unexpected(ErrorContext("state column ", i, " needs a name"));
		}
		for (state_column const& earlier : out)
		{
			if (earlier.name == in.name)
			{
				return tl::make_unexpected(ErrorContext("state column '", in.name, "' is declared twice"));
			}
		}
		state_column column{std::string(in.name), in.type, {}, in.initial};
		if (in.type == column_type::Int || in.type == column_type::Double)
		{
			if (!in.members.empty())
			{
				return tl::make_unexpected(ErrorContext("state column '", in.name, "' is an ", TypeName(in.type),
					" and lists members; only a hash state column has members"));
			}
			out.push_back(std::move(column));
			continue;
		}
		if (in.type != column_type::Hash)
		{
			return tl::make_unexpected(ErrorContext("state column '", in.name, "' is a ", TypeName(in.type),
				"; a state column is a hash, an int or a double"));
		}
		if (in.members.empty())
		{
			return tl::make_unexpected(ErrorContext("hash state column '", in.name,
				"' lists no members; its value is always one of them"));
		}
		for (cell const& member : in.members)
		{
			if (!Fits32(member.i))
			{
				return tl::make_unexpected(ErrorContext("state column '", in.name,
					"' lists a member that is not a 32-bit hash: ", member.i));
			}
			column.members.push_back(member.i);
		}
		std::sort(column.members.begin(), column.members.end());
		auto twice = std::adjacent_find(column.members.begin(), column.members.end());
		if (twice != column.members.end())
		{
			return tl::make_unexpected(ErrorContext("state column '", in.name, "' lists member ", *twice, " twice"));
		}
		if (!std::binary_search(column.members.begin(), column.members.end(), in.initial.i))
		{
			return tl::make_unexpected(ErrorContext("state column '", in.name, "' starts at ", in.initial.i,
				", which is not one of its members"));
		}
		out.push_back(std::move(column));
	}
	return out;
}

// what names the target in messages: "key 'mode'", "input 'energy'";
// targetInput is the table input a given binds, null for a key.
tl::expected<resolved_source, ErrorContext> ResolveSource(source const& from, column_type targetType,
	input_def const* targetInput, std::string const& what, uint32_t self, machine_def const& def,
	std::span<resolved_clause const> earlier, schema const& tables)
{
	resolved_source out;
	out.kind = from.kind;
	column_type     fromType = targetType;
	std::string     fromText;
	switch (from.kind)
	{
	case source_kind::State:
	{
		for (uint32_t s = 0; s < def.state.size() && out.index == UINT32_MAX; ++s)
		{
			if (def.state[s].name == from.name)
			{
				out.index = s;
			}
		}
		if (out.index == UINT32_MAX)
		{
			return tl::make_unexpected(ErrorContext(what, " reads state column '", from.name,
				"', which the machine does not declare"));
		}
		fromType = def.state[out.index].type;
		fromText = cc(std::string("state column '"), from.name, "'");
		break;
	}
	case source_kind::Input:
	{
		for (uint32_t i = 0; i < def.inputs.size() && out.index == UINT32_MAX; ++i)
		{
			if (def.inputs[i].name == from.name)
			{
				out.index = i;
			}
		}
		if (out.index == UINT32_MAX)
		{
			return tl::make_unexpected(ErrorContext(what, " reads input '", from.name,
				"', which the machine does not declare"));
		}
		fromType = def.inputs[out.index].type;
		fromText = cc(std::string("input '"), from.name, "'");
		break;
	}
	case source_kind::Clause:
	{
		// Only an earlier clause: the DAG holds by how the list is written, so
		// nothing has to look for a cycle.
		if (from.clause >= earlier.size())
		{
			return tl::make_unexpected(ErrorContext(what, " reads clause ", from.clause,
				from.clause == self ? ", which is this clause" : ", which comes later",
				"; a clause reads only the clauses before it in its list"));
		}
		table_def const& source = tables.tables()[earlier[from.clause].table];
		auto column = tables.FindColumn(earlier[from.clause].table, from.name);
		if (!column)
		{
			return tl::make_unexpected(ErrorContext(what, " reads column '", from.name, "' of clause ", from.clause,
				", and its table '", source.name, "' has no such column"));
		}
		fromType = source.columns[*column].type;
		if (!IsScalarValue(fromType))
		{
			return tl::make_unexpected(ErrorContext(what, " reads column '", from.name, "' of clause ", from.clause,
				", a ", TypeName(fromType), "; a clause answers with a value column"));
		}
		// An or clause answers one value, the OR of its holding rows' flags;
		// any other column differs between those rows.
		if (source.pick == pick_policy::Or && *column != source.output)
		{
			return tl::make_unexpected(ErrorContext(what, " reads column '", from.name, "' of clause ", from.clause,
				", and or table '", source.name, "' answers only its output '", source.columns[source.output].name, "'"));
		}
		// An override's answer is read through the clause it overrides, which
		// gives the coalesced answer; read alone it would be NULL on a miss.
		if (earlier[from.clause].overrides != UINT32_MAX)
		{
			return tl::make_unexpected(ErrorContext(what, " reads clause ", from.clause, ", which overrides clause ",
				earlier[from.clause].overrides, "; read clause ", earlier[from.clause].overrides,
				", which gives the coalesced answer"));
		}
		out.index  = from.clause;
		out.column = *column;
		fromText   = cc(std::string("column '"), from.name, "' of clause ", from.clause);
		// The clause read is overridden: the coalesced answer is the
		// override's column of the same name where the override hit, so that
		// column must carry the same type.
		if (from.clause + 1 < earlier.size() && earlier[from.clause + 1].overrides == from.clause)
		{
			uint32_t const   overrideTable = earlier[from.clause + 1].table;
			table_def const& over          = tables.tables()[overrideTable];
			auto             same          = tables.FindColumn(overrideTable, from.name);
			if (same)
			{
				if (over.columns[*same].type != fromType)
				{
					return tl::make_unexpected(ErrorContext(what, " reads column '", from.name, "' of clause ", from.clause,
						", a ", TypeName(fromType), ", and clause ", from.clause + 1, ", which overrides it, answers that column "
						"from table '", over.name, "' as a ", TypeName(over.columns[*same].type)));
				}
				out.overrideColumn = *same;
			}
		}
		break;
	}
	case source_kind::Literal:
		if (Is32BitColumn(targetType) && !Fits32(from.literal.i))
		{
			return tl::make_unexpected(ErrorContext(what, " is a ", TypeName(targetType),
				" and its literal is not a 32-bit value: ", from.literal.i));
		}
		// R12: a pick clamps a ranged input, and a clamp has no place for NaN; a
		// constant outside the range is a mistake, not a value to clamp.
		if (targetInput && targetInput->ranged
			&& !(std::isfinite(from.literal.d) && from.literal.d >= targetInput->lo && from.literal.d <= targetInput->hi))
		{
			return tl::make_unexpected(ErrorContext(what, " has range ", FloatText(targetInput->lo), "..",
				FloatText(targetInput->hi), ", and its literal is ", FloatText(from.literal.d),
				"; a literal for a ranged input is finite and inside its range"));
		}
		out.literal = from.literal;
		return out;
	}
	if (fromType != targetType)
	{
		return tl::make_unexpected(ErrorContext(what, " is a ", TypeName(targetType), ", and the ", fromText,
			" it reads is a ", TypeName(fromType)));
	}
	return out;
}

// What a source says about a key's run at run time: whether every value it
// can hold is one Prepare proves a run for. A hash state column, a literal
// and a best or weighted clause close their key; a machine input, an int or
// a double state column and an or clause's flags close nothing, and a value
// outside the declared domain holds the clause for want of a run.
bool ClosesItsKey(resolved_source const& from, machine_def const& def, std::span<resolved_clause const> earlier,
	schema const& tables)
{
	switch (from.kind)
	{
	case source_kind::State:   return def.state[from.index].type == column_type::Hash;
	case source_kind::Literal: return true;
	case source_kind::Input:   return false;
	case source_kind::Clause:  return tables.tables()[earlier[from.index].table].pick != pick_policy::Or;
	}
	return false;
}

// An override (Part 4) is a left outer join with a coalesce onto the clause
// before it: it must directly follow that clause, write only what that clause
// writes (state columns, and an emit only where it emits), and that clause
// must itself always answer for a reachable key, so the machine stays total
// with the override's table partial. An or table answers every key (its OR
// is 0 for none), so it has no miss to override with, nor a row to replace.
tl::expected<void, ErrorContext> CheckOverride(add_clause const& c, resolved_clause const& over, uint32_t self,
	machine_def const& def, std::span<resolved_clause const> earlier, schema const& tables)
{
	if (c.overrides >= self)
	{
		return tl::make_unexpected(ErrorContext("the clause overrides clause ", c.overrides,
			"; an override names the clause directly before it"));
	}
	if (c.overrides + 1 != self)
	{
		return tl::make_unexpected(ErrorContext("the clause overrides clause ", c.overrides,
			", which does not directly precede it; an override follows the clause it overrides"));
	}
	resolved_clause const& base     = earlier[c.overrides];
	table_def const&       baseDef  = tables.tables()[base.table];
	table_def const&       overDef  = tables.tables()[over.table];
	if (base.overrides != UINT32_MAX)
	{
		return tl::make_unexpected(ErrorContext("the clause overrides clause ", c.overrides,
			", which is itself an override; an override names a clause that always answers"));
	}
	if (overDef.pick == pick_policy::Or || baseDef.pick == pick_policy::Or)
	{
		return tl::make_unexpected(ErrorContext("the clause overrides clause ", c.overrides, ", and table '",
			(overDef.pick == pick_policy::Or ? overDef : baseDef).name,
			"' is an or table, which answers every key (0 for none) and has no row to replace or miss to fall back from"));
	}
	for (auto const& [column, state] : over.sets)
	{
		bool shared = false;
		for (auto const& baseSet : base.sets)
		{
			shared = shared || baseSet.second == state;
		}
		if (!shared)
		{
			return tl::make_unexpected(ErrorContext("the clause sets state column '", def.state[state].name,
				"', which clause ", c.overrides, " it overrides does not; an override replaces that clause's answer, "
				"so it sets a subset of its columns"));
		}
	}
	if (over.emit != UINT32_MAX && base.emit == UINT32_MAX)
	{
		return tl::make_unexpected(ErrorContext("the clause emits, and clause ", c.overrides,
			" it overrides does not; an override's emit replaces that clause's"));
	}
	for (size_t k = 0; k < base.keys.size(); ++k)
	{
		uint32_t column = 0;
		for (uint32_t col = 0, seen = 0; col < baseDef.columns.size(); ++col)
		{
			if (baseDef.columns[col].key && seen++ == k)
			{
				column = col;
				break;
			}
		}
		if (IsFallThroughKey(baseDef, column) || ClosesItsKey(base.keys[k], def, earlier, tables))
		{
			continue;
		}
		return tl::make_unexpected(ErrorContext("the clause overrides clause ", c.overrides, ", whose key '",
			baseDef.columns[column].name, "' of table '", baseDef.name, "' is read from a source that closes nothing, so "
			"it may hold for want of a run; an override names a clause that always answers"));
	}
	return {};
}

tl::expected<resolved_clause, ErrorContext> ResolveClause(add_clause const& c, uint32_t self, machine_def const& def,
	std::span<resolved_clause const> earlier, schema const& tables)
{
	resolved_clause out;
	auto table = tables.FindTable(c.table);
	if (!table)
	{
		return tl::make_unexpected(ErrorContext("the clause reads table '", c.table, "', which the schema does not have"));
	}
	out.table = *table;
	table_def const& t = tables.tables()[out.table];
	// A clause answers one value per column: a best pick's row, a weighted
	// draw's row, or an or pick's OR of flags. An all pick answers a list.
	if (t.pick == pick_policy::None)
	{
		return tl::make_unexpected(ErrorContext("table '", t.name, "' has no pick; a clause reads a picked table"));
	}
	if (t.pick == pick_policy::All)
	{
		return tl::make_unexpected(ErrorContext("table '", t.name,
			"' is an all table; a clause answers one value per column, and an all pick answers with every row that holds"));
	}

	out.fallThrough = FallThroughKeysOf(t);

	// where: exactly one source per key column, in column order.
	std::vector<uint32_t> keyColumns;
	for (uint32_t col = 0; col < t.columns.size(); ++col)
	{
		if (t.columns[col].key)
		{
			keyColumns.push_back(col);
		}
	}
	out.keys.resize(keyColumns.size());
	std::vector<uint8_t> bound(keyColumns.size(), 0);
	for (clause_bind const& b : c.where)
	{
		auto column = tables.FindColumn(out.table, b.target);
		if (!column)
		{
			return tl::make_unexpected(ErrorContext("where names column '", b.target, "', which table '", t.name,
				"' does not have"));
		}
		auto k = std::find(keyColumns.begin(), keyColumns.end(), *column);
		if (k == keyColumns.end())
		{
			return tl::make_unexpected(ErrorContext("where binds column '", b.target, "' of table '", t.name,
				"', which is not a key; where binds keys, given binds inputs"));
		}
		size_t const at = static_cast<size_t>(k - keyColumns.begin());
		if (bound[at])
		{
			return tl::make_unexpected(ErrorContext("where binds key '", b.target, "' twice"));
		}
		bound[at] = 1;
		auto resolved = ResolveSource(b.from, t.columns[*column].type, nullptr, cc(std::string("key '"), b.target, "'"),
			self, def, earlier, tables);
		if (!resolved)
		{
			return tl::make_unexpected(std::move(resolved.error()));
		}
		// An earlier clause keyed on a fall-through key can answer from its
		// blank run, whose key holds 0: a value only a fall-through key holds,
		// so an ordinary key reading it would look up a run no row can name (a
		// hash 0 is refused) instead of a value.
		if (resolved->kind == source_kind::Clause && !(!t.domains.empty() && t.domains[at].kind == domain_kind::FallThrough))
		{
			// The coalesced answer may come from the override's table, whose
			// same-named column is as much a source.
			std::pair<uint32_t, uint32_t> const read[] = {{earlier[resolved->index].table, resolved->column},
				{resolved->overrideColumn == UINT32_MAX ? UINT32_MAX : earlier[resolved->index + 1].table, resolved->overrideColumn}};
			for (auto const& [table, column] : read)
			{
				if (table == UINT32_MAX)
				{
					continue;
				}
				table_def const& source = tables.tables()[table];
				if (IsFallThroughKey(source, column))
				{
					return tl::make_unexpected(ErrorContext("key '", b.target, "' reads column '",
						source.columns[column].name, "' of clause ", resolved->index, ", the fall-through key of table '",
						source.name, "'; that clause can answer from its blank run, with 0, which only a fall-through key "
						"holds"));
				}
			}
		}
		out.keys[at] = *resolved;
	}
	for (size_t k = 0; k < keyColumns.size(); ++k)
	{
		if (!bound[k])
		{
			return tl::make_unexpected(ErrorContext("key '", t.columns[keyColumns[k]].name, "' of table '", t.name,
				"' is not bound; where gives every key a source"));
		}
	}

	// given: exactly one source per table input, in input order.
	out.inputs.resize(t.inputs.size());
	bound.assign(t.inputs.size(), 0);
	for (clause_bind const& b : c.given)
	{
		uint32_t input = UINT32_MAX;
		for (uint32_t i = 0; i < t.inputs.size() && input == UINT32_MAX; ++i)
		{
			if (t.inputs[i].name == b.target)
			{
				input = i;
			}
		}
		if (input == UINT32_MAX)
		{
			return tl::make_unexpected(ErrorContext("given names input '", b.target, "', which table '", t.name,
				"' does not declare; given binds the table's inputs"));
		}
		if (bound[input])
		{
			return tl::make_unexpected(ErrorContext("given binds input '", b.target, "' twice"));
		}
		bound[input] = 1;
		auto resolved = ResolveSource(b.from, t.inputs[input].type, &t.inputs[input],
			cc(std::string("input '"), b.target, "'"), self, def, earlier, tables);
		if (!resolved)
		{
			return tl::make_unexpected(std::move(resolved.error()));
		}
		out.inputs[input] = *resolved;
	}
	for (uint32_t i = 0; i < t.inputs.size(); ++i)
	{
		if (!bound[i])
		{
			return tl::make_unexpected(ErrorContext("input '", t.inputs[i].name, "' of table '", t.name,
				"' is not bound; given gives every input of the table a source"));
		}
	}
	// P13: the instance keeps the previous value of every machine input, read
	// once per step through its getter, and an edge crosses from it. A literal
	// never moves, so its edge never holds. A state column or an earlier
	// clause is a value of this step's own making, with no previous value kept.
	for (column_def const& col : t.columns)
	{
		if (!col.edge)
		{
			continue;
		}
		resolved_source const& from = out.inputs[col.input];
		if (from.kind == source_kind::Input)
		{
			continue;
		}
		std::string here;
		switch (from.kind)
		{
		case source_kind::State:   here = cc(std::string("state column '"), def.state[from.index].name, "'"); break;
		case source_kind::Clause:
			here = cc(std::string("column '"), tables.tables()[earlier[from.index].table].columns[from.column].name,
				"' of clause ", from.index);
			break;
		case source_kind::Literal: here = "a literal"; break;
		case source_kind::Input:   break;   // continued above
		}
		return tl::make_unexpected(ErrorContext("input '", t.inputs[col.input].name, "' is read by edge '", col.name,
			"', which compares it with its value on the previous step; it comes from a machine input's getter, and here from ",
			here));
	}

	// set: each a value column into a state column of its type. Two clauses
	// writing one column are ordered by position; one clause writing a column
	// twice has no order, so it is refused.
	for (set_state const& s : c.set)
	{
		uint32_t state = UINT32_MAX;
		for (uint32_t i = 0; i < def.state.size() && state == UINT32_MAX; ++i)
		{
			if (def.state[i].name == s.state)
			{
				state = i;
			}
		}
		if (state == UINT32_MAX)
		{
			return tl::make_unexpected(ErrorContext("set writes state column '", s.state,
				"', which the machine does not declare"));
		}
		for (auto const& earlierSet : out.sets)
		{
			if (earlierSet.second == state)
			{
				return tl::make_unexpected(ErrorContext("set writes state column '", s.state, "' twice"));
			}
		}
		auto column = tables.FindColumn(out.table, s.column);
		if (!column)
		{
			return tl::make_unexpected(ErrorContext("set reads column '", s.column, "', which table '", t.name,
				"' does not have"));
		}
		if (t.pick == pick_policy::Or && *column != t.output)
		{
			return tl::make_unexpected(ErrorContext("set reads column '", s.column, "' of or table '", t.name,
				"', which answers only its output '", t.columns[t.output].name, "'"));
		}
		column_type const type = t.columns[*column].type;
		if (!IsScalarValue(type))
		{
			return tl::make_unexpected(ErrorContext("set reads column '", s.column, "' of table '", t.name, "', a ",
				TypeName(type), "; a state column is written from a value column"));
		}
		if (type != def.state[state].type)
		{
			return tl::make_unexpected(ErrorContext("set writes ", TypeName(def.state[state].type), " state column '",
				s.state, "' from column '", s.column, "', a ", TypeName(type)));
		}
		out.sets.emplace_back(*column, state);
	}

	if (!c.emit.empty())
	{
		if (t.pick == pick_policy::Or)
		{
			return tl::make_unexpected(ErrorContext("the clause emits '", c.emit, "', and or table '", t.name,
				"' answers the OR of every row that holds, which may be several rows; it has no one row to emit"));
		}
		auto column = tables.FindColumn(out.table, c.emit);
		bool hasCommand = false;
		for (column_def const& col : t.columns)
		{
			hasCommand = hasCommand || col.type == column_type::Command;
		}
		if (!hasCommand)
		{
			return tl::make_unexpected(ErrorContext("the clause emits '", c.emit, "', and table '", t.name,
				"' has no command column or suffix to emit"));
		}
		if (!column || t.columns[*column].type != column_type::Command)
		{
			return tl::make_unexpected(ErrorContext("the clause emits '", c.emit, "', which is not a command column of table '",
				t.name, "'"));
		}
		out.emit = *column;
	}

	if (c.overrides != UINT32_MAX)
	{
		auto overridden = CheckOverride(c, out, self, def, earlier, tables);
		if (!overridden)
		{
			return tl::make_unexpected(std::move(overridden.error()));
		}
		out.overrides = c.overrides;
	}
	return out;
}

// Which hash state columns' change runs each entry or exit clause on a step
// (Part 4, owner 2026-10-19): the columns it keys on as state.<col>, read off
// its where. Clauses that read one another (a clause source, either way), and
// an override with the clause it overrides, run as one group on the union of
// their columns, so a step never runs a reader whose source it skipped; a
// group keying on no state column runs on any change of state, as every
// clause did before.
void EnteredOn(machine_def const& def, std::vector<resolved_clause>& clauses)
{
	std::vector<uint32_t> group(clauses.size());
	for (uint32_t i = 0; i < clauses.size(); ++i)
	{
		group[i] = i;
	}
	auto root = [&](uint32_t i)
	{
		while (group[i] != i)
		{
			i = group[i];
		}
		return i;
	};
	for (uint32_t i = 0; i < clauses.size(); ++i)
	{
		// An override runs only when the clause it overrides answered, so the
		// two run on one test.
		if (clauses[i].overrides != UINT32_MAX)
		{
			group[root(i)] = root(clauses[i].overrides);
		}
		for (auto const* sources : {&clauses[i].keys, &clauses[i].inputs})
		{
			for (resolved_source const& from : *sources)
			{
				if (from.kind == source_kind::Clause)
				{
					group[root(i)] = root(from.index);
				}
			}
		}
	}
	std::vector<std::vector<uint32_t>> columns(clauses.size());
	for (uint32_t i = 0; i < clauses.size(); ++i)
	{
		for (resolved_source const& from : clauses[i].keys)
		{
			if (from.kind == source_kind::State && def.state[from.index].type == column_type::Hash)
			{
				columns[root(i)].push_back(from.index);
			}
		}
	}
	std::vector<uint32_t> every;
	for (uint32_t s = 0; s < def.state.size(); ++s)
	{
		if (def.state[s].type == column_type::Hash)
		{
			every.push_back(s);
		}
	}
	for (uint32_t i = 0; i < clauses.size(); ++i)
	{
		std::vector<uint32_t> on = columns[root(i)];
		std::sort(on.begin(), on.end());
		on.erase(std::unique(on.begin(), on.end()), on.end());
		clauses[i].enteredOn = on.empty() ? every : std::move(on);
	}
}

}

namespace detail
{

std::string ClausePlace(std::string_view machine, size_t list, size_t index, std::string_view table)
{
	return cc(std::string("machine '"), machine, "' ", kListNames[list], " clause ", index, " (table '", table, "')");
}

tl::expected<machine_def, ErrorContext> ResolveMachine(create_machine const& definition, schema const& tables)
{
	if (definition.name.empty())
	{
		return tl::make_unexpected(ErrorContext("a machine needs a name"));
	}
	machine_def def;
	def.name = std::string(definition.name);
	auto where = [&]() { return cc(std::string("machine '"), def.name, "'"); };
	auto inputs = ResolveInputs(definition);
	if (!inputs)
	{
		return tl::make_unexpected(std::move(inputs.error()).with(where()));
	}
	def.inputs = std::move(*inputs);
	auto state = ResolveState(definition);
	if (!state)
	{
		return tl::make_unexpected(std::move(state.error()).with(where()));
	}
	def.state = std::move(*state);
	for (size_t list = 0; list < kTriggerCount; ++list)
	{
		std::span<add_clause const> clauses = ListOf(definition, list);
		for (uint32_t i = 0; i < clauses.size(); ++i)
		{
			auto clause = ResolveClause(clauses[i], i, def, def.lists[list], tables);
			if (!clause)
			{
				return tl::make_unexpected(std::move(clause.error()).with(ClausePlace(def.name, list, i, clauses[i].table)));
			}
			// A state is entered when a hash column of next differs from current
			// at the end of the step, and entry runs once, after the step list: a
			// hash an entry clause wrote would be a transition that never gets an
			// entry of its own (R6's state function would never start). Exit runs
			// on that same test, keyed on the state being left: a hash it wrote
			// would overrule the step list's transition with a choice made from
			// the old state. Int and double columns are data, and both may write
			// them.
			for (auto const& [column, state] : clause->sets)
			{
				if (def.state[state].type != column_type::Hash)
				{
					continue;
				}
				if (list == size_t(trigger::Entry))
				{
					return tl::make_unexpected(ErrorContext("an entry clause sets hash state column '", def.state[state].name,
						"': entry runs once per step, after the step list, so a state it moved to would get no entry of "
						"its own; a transition belongs in the step list")
						.with(ClausePlace(def.name, list, i, clauses[i].table)));
				}
				if (list == size_t(trigger::Exit))
				{
					return tl::make_unexpected(ErrorContext("an exit clause sets hash state column '", def.state[state].name,
						"': exit runs when the step list has chosen a state, keyed on the one being left, so its write "
						"would overrule that choice; a transition belongs in the step list")
						.with(ClausePlace(def.name, list, i, clauses[i].table)));
				}
			}
			if (clause->overrides != UINT32_MAX)
			{
				def.lists[list][clause->overrides].overriddenBy = i;
			}
			def.lists[list].push_back(std::move(*clause));
		}
	}
	EnteredOn(def, def.lists[size_t(trigger::Entry)]);
	EnteredOn(def, def.lists[size_t(trigger::Exit)]);
	return def;
}

}

namespace
{

// Which tables the proof read, so a change to any of them makes the machine
// stale: the tables its clauses pick from, and the tables their domains read.
// Each generation is the one of the version the proof read, taken with it.
struct watch_list
{
	std::vector<detail::watched_table> tables;   // distinct

	void Add(uint32_t table, TableVersion const& version)
	{
		for (detail::watched_table const& w : tables)
		{
			if (w.table == table)
			{
				return;
			}
		}
		tables.push_back(detail::watched_table{table, version.Generation()});
	}
};

// Calls visit(row) for each version row a proved clause can answer from, until
// it returns false: the rows a best clause's layout reaches (a row no pick
// reaches answers nothing), or every row of a weighted or an or clause's runs.
// Returns whether every row was visited.
template<typename Visit>
bool EachAnswerRow(detail::prepared_clause const& clause, Visit&& visit)
{
	for (uint32_t row : clause.runs.layout.lists)
	{
		if (!visit(row))
		{
			return false;
		}
	}
	for (detail::row_span const& run : clause.rowRuns)
	{
		for (uint32_t row = run.first; row < run.first + run.count; ++row)
		{
			if (!visit(row))
			{
				return false;
			}
		}
	}
	return true;
}

int64_t ValueAt(table_rows const& rows, uint32_t row, uint32_t column)
{
	return rows.cells[size_t(row) * rows.valueCount + rows.slot[column]].i;
}

// The clause's key domain, read from its sources. A source that closes its key
// gives the values it can hold: a hash state column its members, a best or
// weighted earlier clause the values of the column read in the rows it can
// answer from, a literal its value. Any other source closes nothing (a machine
// input, an int or double state column, an or clause's flags, since the OR of
// any subset of its rows may come out), and the key keeps the table's declared
// domain; an or or a weighted table declares none, so its key is over its rows.
// Rows-declared keys then contribute the sub-tuples the rows name, crossed
// with the rest, as a load's domain does.
tl::expected<void, ErrorContext> ClauseDomain(machine_def const& def, resolved_clause const& clause,
	std::span<detail::prepared_clause const> earlier, relation_store const& store, watch_list& watch,
	detail::key_domains& out)
{
	table_def const& t    = store.definition().tables()[clause.table];
	size_t const     keys = clause.keys.size();
	out.Reset(keys);
	for (uint32_t k = 0; k < keys; ++k)
	{
		resolved_source const& from   = clause.keys[k];
		std::vector<int64_t>&  values = out.values[k];
		bool                   closed = true;
		switch (from.kind)
		{
		case source_kind::State:
			closed = def.state[from.index].type == column_type::Hash;
			if (closed)
			{
				values = def.state[from.index].members;
			}
			break;
		case source_kind::Literal:
			values.assign(1, from.literal.i);
			break;
		case source_kind::Clause:
		{
			detail::prepared_clause const& source = earlier[from.index];
			table_rows const&              rows   = version_access::Rows(*source.version);
			closed = version_access::Def(*source.version).pick != pick_policy::Or;
			if (closed)
			{
				EachAnswerRow(source, [&](uint32_t row)
				{
					values.push_back(ValueAt(rows, row, from.column));
					return true;
				});
				// The coalesced answer: where the override hits, its row's
				// column of the same name is what the key reads.
				if (from.overrideColumn != UINT32_MAX)
				{
					detail::prepared_clause const& over     = earlier[from.index + 1];
					table_rows const&              overRows = version_access::Rows(*over.version);
					EachAnswerRow(over, [&](uint32_t row)
					{
						values.push_back(ValueAt(overRows, row, from.overrideColumn));
						return true;
					});
				}
				std::sort(values.begin(), values.end());
				values.erase(std::unique(values.begin(), values.end()), values.end());
			}
			break;
		}
		case source_kind::Input:
			closed = false;
			break;
		}
		// A fall-through key is open whatever feeds it: a value with no run
		// falls through to the blank run, so a source's values need no run of
		// their own.
		bool const fallThrough = !t.domains.empty() && t.domains[k].kind == domain_kind::FallThrough;
		if (closed && !fallThrough)
		{
			continue;
		}
		values.clear();
		if (t.domains.empty())
		{
			out.overRows[k] = 1;
			continue;
		}
		domain_def const&               declared = t.domains[k];
		counted_ptr<TableVersion const> foreign;
		if (declared.kind == domain_kind::Column)
		{
			auto version = store.Version(declared.table);
			if (!version)
			{
				return tl::make_unexpected(std::move(version.error()));
			}
			foreign = *version;
			watch.Add(declared.table, *foreign);
		}
		detail::DeclaredDomain(declared, foreign ? &version_access::Def(*foreign) : nullptr,
			foreign ? &version_access::Rows(*foreign) : nullptr, k, out);
	}
	return {};
}

// Cuts domains to the tuples version has rows for, in order.
void KeepTuplesWithRuns(TableVersion const& version, detail::key_domains& domains)
{
	size_t const      keys = domains.overRows.size();
	std::vector<cell> key(keys);
	size_t            kept = 0;
	for (size_t t = 0; t < domains.count; ++t)
	{
		int64_t const* tuple = domains.tuples.data() + t * keys;
		for (size_t k = 0; k < keys; ++k)
		{
			key[k] = cell::Integer(tuple[k]);
		}
		auto run = version.FindOrdered(key);
		if (!run || run->first == run->last)
		{
			continue;
		}
		std::copy(tuple, tuple + keys, domains.tuples.data() + kept * keys);
		++kept;
	}
	domains.count = kept;
	domains.tuples.resize(kept * keys);
}

tl::expected<detail::prepared_clause, ErrorContext> ProveClause(machine_def const& def, resolved_clause const& clause,
	std::span<detail::prepared_clause const> earlier, relation_store const& store, watch_list& watch,
	std::vector<run_note>* report)
{
	table_def const& t = store.definition().tables()[clause.table];
	auto version = store.Version(clause.table);
	if (!version)
	{
		return tl::make_unexpected(std::move(version.error()));
	}
	watch.Add(clause.table, **version);
	detail::prepared_clause out;
	out.version = *version;
	table_rows const& rows = version_access::Rows(*out.version);

	detail::key_domains domains;
	auto domain = ClauseDomain(def, clause, earlier, store, watch, domains);
	if (!domain)
	{
		return tl::make_unexpected(std::move(domain.error()));
	}
	detail::ResolveDomain(rows, domains);
	// An override's table need not be total (Part 4): a tuple its rows have no
	// run for is a miss at run time, so only the runs present are proved.
	if (clause.overrides != UINT32_MAX)
	{
		KeepTuplesWithRuns(*out.version, domains);
	}

	// The load's own check, over the clause's domain: no row is fresh, and
	// nothing is changing, so a gap names the run's first row.
	std::vector<size_t> standing(rows.RowCount(), SIZE_MAX);
	detail::check_subject const subject{clause.table, t, rows, standing, "row", "", "", false, "the clause's domain"};
	if (t.pick == pick_policy::Best)
	{
		auto proved = detail::CheckBest(subject, domains, nullptr, report, &out.runs);
		if (!proved)
		{
			return tl::make_unexpected(std::move(proved.error()));
		}
		std::vector<uint32_t> axes;
		detail::LayoutAxes(t, axes);
		out.axisInputs = immutable_array<uint32_t>::FromArray(axes.data(), axes.size());
		if (t.ranking != UINT32_MAX)
		{
			out.rankingInput = t.columns[t.ranking].input;
		}
	}
	else
	{
		// An or pick always answers one value, the OR of its holding rows' flags
		// and 0 when none hold, so a tuple with no run is an empty run. A
		// weighted draw needs a row; Load proved every run's weights draw.
		std::vector<detail::row_span> runs;
		auto proved = detail::CheckRuns(subject, domains, t.pick == pick_policy::Weighted, report, runs);
		if (!proved)
		{
			return tl::make_unexpected(std::move(proved.error()));
		}
		out.rowRuns = immutable_array<detail::row_span>::FromArray(runs.data(), runs.size());
	}
	out.tupleCount = domains.count;
	out.tuples     = immutable_array<int64_t>::FromArray(domains.tuples.data(), domains.tuples.size());

	// A hash a set can write must name a member of its state column, since the
	// next state is always one. Read off the rows the clause can answer from.
	for (auto const& [column, state] : clause.sets)
	{
		state_column const& target = def.state[state];
		if (target.type != column_type::Hash)
		{
			continue;
		}
		uint32_t bad = UINT32_MAX;
		EachAnswerRow(out, [&](uint32_t row)
		{
			if (std::binary_search(target.members.begin(), target.members.end(), ValueAt(rows, row, column)))
			{
				return true;
			}
			bad = row;
			return false;
		});
		if (bad != UINT32_MAX)
		{
			return tl::make_unexpected(ErrorContext("table '", t.name, "' row ", bad, " (owner ", rows.owners[bad],
				") sets state column '", target.name, "' from column '", t.columns[column].name, "' to ",
				ValueAt(rows, bad, column), ", which is not one of its members"));
		}
	}

	// Proof only: a step reads cuts, groups and lists, never these.
	std::vector<uint8_t>().swap(out.runs.layout.closed);
	std::vector<uint8_t>().swap(out.runs.layout.listed);
	return out;
}

}

tl::expected<void, ErrorContext> CheckMachine(create_machine const& definition, schema const& tables)
{
	auto resolved = detail::ResolveMachine(definition, tables);
	if (!resolved)
	{
		return tl::make_unexpected(std::move(resolved.error()));
	}
	return {};
}

Machine::Machine(std::unique_ptr<Data>&& data) : _data(std::move(data)) {}

counted_ptr<Machine const> machine_access::Make(std::unique_ptr<Data>&& data)
{
	return UncountedWrap(static_cast<Machine const*>(new Machine(std::move(data))));
}

Machine::~Machine() = default;

void Machine::Release() const
{
	if (_refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
	{
		delete this;
	}
}

std::string_view Machine::name() const
{
	return _data->def.name;
}

bool Machine::Stale(relation_store const& store) const
{
	// A store of another schema may have fewer tables; it cannot be the one
	// proved against, and indexing it would read past its tables.
	size_t const tableCount = store.definition().tables().size();
	for (detail::watched_table const& w : _data->watched)
	{
		if (w.table >= tableCount || store.Generation(w.table) != w.generation)
		{
			return true;
		}
	}
	return false;
}

tl::expected<counted_ptr<Machine const>, ErrorContext> Prepare(create_machine const& definition,
	relation_store const& store, std::vector<run_note>* report)
{
	// As a load's: cleared, and left empty by a refusal, which says why.
	if (report)
	{
		report->clear();
	}
	auto refuse = [&](ErrorContext&& error)
	{
		if (report)
		{
			report->clear();
		}
		return tl::make_unexpected(std::move(error));
	};
	schema const& tables = store.definition();
	auto resolved = detail::ResolveMachine(definition, tables);
	if (!resolved)
	{
		return refuse(std::move(resolved.error()));
	}
	auto data = std::make_unique<machine_access::Data>();
	data->def = std::move(*resolved);
	watch_list watch;
	for (size_t list = 0; list < kTriggerCount; ++list)
	{
		std::vector<resolved_clause> const& clauses = data->def.lists[list];
		for (size_t i = 0; i < clauses.size(); ++i)
		{
			auto proved = ProveClause(data->def, clauses[i], data->lists[list], store, watch, report);
			if (!proved)
			{
				return refuse(std::move(proved.error()).with(
					detail::ClausePlace(data->def.name, list, i, tables.tables()[clauses[i].table].name)));
			}
			data->lists[list].push_back(std::move(*proved));
		}
	}
	data->watched = immutable_array<detail::watched_table>::FromArray(watch.tables.data(), watch.tables.size());
	return machine_access::Make(std::move(data));
}

}
