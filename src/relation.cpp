#include "tabular/relation.h"

#include "float_text.h"
#include "relation_internal.h"
#include "version_checks.h"

#include "Support/strings.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <iterator>
#include <unordered_map>
#include <utility>

namespace tabular
{

namespace
{

// Keys and orders compare by equality, which only an integer value gives:
// doubles fail on NaN and -0.0, and conditions and commands are not values.
bool ComparesByEquality(column_type type)
{
	return type == column_type::Hash || type == column_type::Flags || type == column_type::Int;
}

// A key needs equality only. Interned strings have it, so a string can be a
// key; it has no order worth sorting by, so it cannot be in one.
bool CanBeKey(column_type type)
{
	return ComparesByEquality(type) || type == column_type::String;
}

tl::expected<immutable_array<input_def>, ErrorContext> BuildInputs(create_table const& c)
{
	shared_array<input_def> inputs(c.inputs.size());
	for (uint32_t i = 0; i < c.inputs.size(); ++i)
	{
		add_input const& in = c.inputs[i];
		if (in.name.empty())
		{
			return tl::make_unexpected(ErrorContext("input ", i, " of table '", c.name, "' needs a name"));
		}
		for (uint32_t earlier = 0; earlier < i; ++earlier)
		{
			if (inputs[earlier].name == in.name)
			{
				return tl::make_unexpected(ErrorContext("table '", c.name, "' has input '", in.name, "' twice"));
			}
		}
		if (in.type != column_type::Double && in.type != column_type::Flags)
		{
			return tl::make_unexpected(ErrorContext("input '", in.name, "' of table '", c.name,
				"' is a ", TypeName(in.type), "; an input is a double or a flag set"));
		}
		if (in.ranged && in.type != column_type::Double)
		{
			return tl::make_unexpected(ErrorContext("input '", in.name, "' of table '", c.name, "' is a ",
				TypeName(in.type), "; only a double has a range"));
		}
		if (in.ranged && !(std::isfinite(in.lo) && std::isfinite(in.hi) && in.lo < in.hi))
		{
			return tl::make_unexpected(ErrorContext("input '", in.name, "' of table '", c.name, "' has range ",
				FloatText(in.lo), "..", FloatText(in.hi), "; a range is lo < hi, both finite"));
		}
		inputs[i] = input_def{std::string(in.name), in.type, in.ranged, in.lo, in.hi};
	}
	return immutable_array<input_def>(std::move(inputs));
}

// A band tests a double; a ranking flags column compares a flag set. Nothing
// else reads an input.
tl::expected<void, ErrorContext> CheckInput(create_table const& c, add_column const& in,
	immutable_array<input_def> const& inputs)
{
	if (in.input == UINT32_MAX)
	{
		if (in.type == column_type::Band)
		{
			return tl::make_unexpected(ErrorContext("band '", in.name, "' of table '", c.name,
				"' names the double input it tests"));
		}
		return {};
	}
	if (in.type != column_type::Band && in.type != column_type::Flags)
	{
		return tl::make_unexpected(ErrorContext("column '", in.name, "' of table '", c.name, "' is a ",
			TypeName(in.type), "; only a band or a flags column reads an input"));
	}
	if (in.input >= inputs.size())
	{
		return tl::make_unexpected(ErrorContext("column '", in.name, "' of table '", c.name, "' reads input ",
			in.input, ", but the table has ", inputs.size()));
	}
	column_type const wanted = in.type == column_type::Band ? column_type::Double : column_type::Flags;
	if (inputs[in.input].type != wanted)
	{
		return tl::make_unexpected(ErrorContext("column '", in.name, "' of table '", c.name, "' reads input '",
			inputs[in.input].name, "', a ", TypeName(inputs[in.input].type), "; a ", TypeName(in.type),
			" reads a ", TypeName(wanted)));
	}
	return {};
}

tl::expected<immutable_array<column_def>, ErrorContext> BuildColumns(create_table const& c,
	immutable_array<input_def> const& inputs)
{
	if (c.columns.empty())
	{
		return tl::make_unexpected(ErrorContext("table '", c.name, "' has no columns"));
	}
	shared_array<column_def> columns(c.columns.size());
	for (uint32_t i = 0; i < c.columns.size(); ++i)
	{
		add_column const& in = c.columns[i];
		if (in.name.empty())
		{
			return tl::make_unexpected(ErrorContext("column ", i, " of table '", c.name, "' needs a name"));
		}
		for (uint32_t earlier = 0; earlier < i; ++earlier)
		{
			if (columns[earlier].name == in.name)
			{
				return tl::make_unexpected(ErrorContext("table '", c.name, "' has column '", in.name, "' twice"));
			}
		}
		if (in.key && !CanBeKey(in.type))
		{
			return tl::make_unexpected(ErrorContext("column '", in.name, "' of table '", c.name, "' is a ",
				TypeName(in.type), " and cannot be a key"));
		}
		auto reads = CheckInput(c, in, inputs);
		if (!reads)
		{
			return tl::make_unexpected(std::move(reads.error()));
		}
		columns[i] = column_def{std::string(in.name), in.type, in.key, in.input};
	}
	return immutable_array<column_def>(std::move(columns));
}

tl::expected<immutable_array<uint32_t>, ErrorContext> BuildOrder(create_table const& c,
	immutable_array<column_def> const& columns)
{
	order_by const& order = *c.ordering;
	if (order.columns.empty())
	{
		return tl::make_unexpected(ErrorContext("the order of table '", c.name, "' names no columns"));
	}
	for (size_t k = 0; k < order.columns.size(); ++k)
	{
		uint32_t column = order.columns[k];
		if (column >= columns.size())
		{
			return tl::make_unexpected(ErrorContext("the order of table '", c.name, "' names column ", column,
				", but the table has ", columns.size()));
		}
		for (size_t earlier = 0; earlier < k; ++earlier)
		{
			if (order.columns[earlier] == column)
			{
				return tl::make_unexpected(ErrorContext("the order of table '", c.name, "' names column '",
					columns[column].name, "' twice"));
			}
		}
		if (!ComparesByEquality(columns[column].type))
		{
			return tl::make_unexpected(ErrorContext("column '", columns[column].name, "' of table '", c.name,
				"' is a ", TypeName(columns[column].type), " and cannot be in an order"));
		}
	}
	return immutable_array<uint32_t>::FromArray(order.columns.data(), order.columns.size());
}

// The pick shape: one ranking column, one script condition, the key prefix,
// and the output of an or table. Every table gets ranking and key_count, so a
// later front end reads them without re-deriving.
tl::expected<void, ErrorContext> BuildPick(create_table const& c, table_def& out)
{
	uint32_t conditions = 0;
	for (uint32_t i = 0; i < out.columns.size(); ++i)
	{
		column_def const& col = out.columns[i];
		if (col.key)
		{
			++out.key_count;
		}
		if (col.type == column_type::Condition && ++conditions > 1)
		{
			return tl::make_unexpected(ErrorContext("table '", c.name,
				"' has two condition columns; a row carries at most one script condition"));
		}
		if (col.type == column_type::Flags && col.input != UINT32_MAX)
		{
			if (out.ranking != UINT32_MAX)
			{
				return tl::make_unexpected(ErrorContext("table '", c.name, "' ranks by two flags columns, '",
					out.columns[out.ranking].name, "' and '", col.name, "'"));
			}
			out.ranking = i;
		}
	}
	out.pick = c.pick;
	if (c.pick != pick_policy::Or && c.output != UINT32_MAX)
	{
		return tl::make_unexpected(ErrorContext("table '", c.name, "' names an output; only an or table has one"));
	}
	// Before the no-pick return: a weight on a table nothing draws from is
	// written and never read, which is the mistake this refuses.
	if (c.pick != pick_policy::Weighted && c.weight != UINT32_MAX)
	{
		return tl::make_unexpected(ErrorContext("table '", c.name, "' names a weight; only a weighted table has one"));
	}
	if (c.pick == pick_policy::None)
	{
		// Only a pick reads an input, so a declared input is written and never
		// read. A band or ranking column here has no input it could name, and
		// CheckInput has already refused it for that.
		if (!out.inputs.empty())
		{
			return tl::make_unexpected(ErrorContext("table '", c.name, "' has no pick, so nothing reads its ",
				out.inputs.size(), " declared inputs; give the table a pick or drop them"));
		}
		return {};
	}
	if (c.pick == pick_policy::Weighted)
	{
		// A weighted table is a transform with a probability: nothing is tested
		// before the draw. A band or a ranking flags column can only read a
		// declared input (CheckInput), so refusing every input refuses them
		// without a rule of their own; a condition column needs none, so it is
		// refused here.
		for (column_def const& col : out.columns)
		{
			if (col.type == column_type::Condition)
			{
				return tl::make_unexpected(ErrorContext("column '", col.name, "' of weighted table '", c.name,
					"' is a condition; a weighted table has no conditions"));
			}
		}
		if (!out.inputs.empty())
		{
			return tl::make_unexpected(ErrorContext("weighted table '", c.name,
				"' declares inputs, which only a band or a ranking flags column reads, and a weighted table has no conditions"));
		}
		// An unnamed weight is an index past the columns, so one check covers
		// both "no weight" and "a weight that is not a double".
		if (c.weight >= out.columns.size() || out.columns[c.weight].type != column_type::Double)
		{
			return tl::make_unexpected(ErrorContext("weighted table '", c.name,
				"' names its weight, one of its double columns", c.weight < out.columns.size()
					? cc(std::string("; '"), out.columns[c.weight].name, "' is a ", TypeName(out.columns[c.weight].type)) : std::string()));
		}
		out.weight = c.weight;
	}
	if (c.pick != pick_policy::Best && out.ranking != UINT32_MAX)
	{
		return tl::make_unexpected(ErrorContext("table '", c.name, "' is ", c.pick == pick_policy::All ? "an all" : "an or",
			" table, and its flags column '", out.columns[out.ranking].name,
			"' ranks by an input; only a best table ranks"));
	}
	// A best table may be mutable: the display relations are engine data that
	// contributors insert into and that a pick must always answer (R2). The
	// other picks keep the narrower rule; no case for a mutable one is known,
	// and lifting it later breaks no file.
	if (!out.read_only && c.pick != pick_policy::Best)
	{
		return tl::make_unexpected(ErrorContext("table '", c.name,
			"' is picked, and only a best table may be mutable; an all, or or weighted table is read-only"));
	}
	// Totality covers a best table's band inputs, so each needs a finite domain.
	if (c.pick == pick_policy::Best)
	{
		for (column_def const& col : out.columns)
		{
			if (col.type == column_type::Band && !out.inputs[col.input].ranged)
			{
				return tl::make_unexpected(ErrorContext("band '", col.name, "' of best table '", c.name,
					"' reads input '", out.inputs[col.input].name, "', which declares no range"));
			}
		}
	}
	// A pick's keys are the order's leading columns, found by binary search.
	uint32_t k = 0;
	for (uint32_t i = 0; i < out.columns.size(); ++i)
	{
		if (!out.columns[i].key)
		{
			continue;
		}
		if (k >= out.order.size() || out.order[k] != i)
		{
			return tl::make_unexpected(ErrorContext("table '", c.name,
				"' is picked, so its order starts with its keys, in column order"));
		}
		++k;
	}
	if (c.pick == pick_policy::Or)
	{
		if (c.output == UINT32_MAX)
		{
			return tl::make_unexpected(ErrorContext("or table '", c.name, "' names its output, a flags column"));
		}
		if (c.output >= out.columns.size() || out.columns[c.output].type != column_type::Flags)
		{
			return tl::make_unexpected(ErrorContext("output '",
				c.output < out.columns.size() ? out.columns[c.output].name : std::string("?"),
				"' is not a flag set, and it is the output of table '", c.name, "'"));
		}
		out.output = c.output;
	}
	return {};
}

tl::expected<void, ErrorContext> BuildPivot(create_table const& c, table_def& out)
{
	if (!c.pivot)
	{
		return {};
	}
	pivot_by const& p = *c.pivot;
	if (out.pick != pick_policy::Best && out.pick != pick_policy::Or)
	{
		return tl::make_unexpected(ErrorContext("table '", c.name, "' is pivoted; only a best or an or table is"));
	}
	uint32_t const headersWanted = out.pick == pick_policy::Best ? out.ranking : out.output;
	if (p.headers == UINT32_MAX || p.headers != headersWanted)
	{
		return tl::make_unexpected(ErrorContext("the pivot headers of table '", c.name, "' are its ",
			out.pick == pick_policy::Best ? "ranking column" : "output column"));
	}
	if (p.cells >= out.columns.size() || p.cells == p.headers || out.columns[p.cells].key)
	{
		return tl::make_unexpected(ErrorContext("the pivot cells of table '", c.name,
			"' are a column that is neither a key nor the headers"));
	}
	column_type const cellType = out.columns[p.cells].type;
	if (cellType == column_type::Condition || cellType == column_type::Command)
	{
		return tl::make_unexpected(ErrorContext("the pivot cells of table '", c.name, "' are a ",
			TypeName(cellType), "; they are a value, a band or a string"));
	}
	for (uint32_t i = 0; i < out.columns.size(); ++i)
	{
		if (!out.columns[i].key && i != p.headers && i != p.cells)
		{
			return tl::make_unexpected(ErrorContext("table '", c.name, "' is pivoted, so it holds only its keys, '",
				out.columns[p.headers].name, "' and '", out.columns[p.cells].name, "'; it also has '",
				out.columns[i].name, "'"));
		}
	}
	out.pivot_headers = p.headers;
	out.pivot_cells   = p.cells;
	return {};
}

// A grid is read-only, has two integer axes and one cell type, and none of a
// row table's shape.
tl::expected<table_def, ErrorContext> BuildGrid(create_table const& c)
{
	grid_shape const& g = *c.grid;
	if (!c.columns.empty() || c.ordering || !c.inputs.empty() || c.pick != pick_policy::None
		|| c.output != UINT32_MAX || c.pivot || c.weight != UINT32_MAX || !c.suffix.empty())
	{
		return tl::make_unexpected(ErrorContext("grid '", c.name,
			"' has axes and cells; it has no columns, order, inputs, pick, output, pivot, weight or suffix"));
	}
	for (grid_axis const* axis : {&g.rows, &g.columns})
	{
		if (axis->name.empty())
		{
			return tl::make_unexpected(ErrorContext("an axis of grid '", c.name, "' needs a name"));
		}
		if (axis->type != column_type::Hash && axis->type != column_type::Int)
		{
			return tl::make_unexpected(ErrorContext("axis '", axis->name, "' of grid '", c.name, "' is a ",
				TypeName(axis->type), "; an axis is a hash or an int"));
		}
	}
	if (g.cells == column_type::Condition || g.cells == column_type::Band)
	{
		return tl::make_unexpected(ErrorContext("the cells of grid '", c.name, "' are ", TypeName(g.cells),
			"s; a grid holds values, strings or commands"));
	}
	// A grid is replaced whole by its one contributor, so it is never a table
	// that rows are inserted into.
	if (!c.read_only)
	{
		return tl::make_unexpected(ErrorContext("grid '", c.name, "' is read-only; it cannot be declared writable"));
	}
	table_def out{std::string(c.name), {}, {}, false, true};
	out.is_grid      = true;
	out.grid_rows    = column_def{std::string(g.rows.name), g.rows.type};
	out.grid_columns = column_def{std::string(g.columns.name), g.columns.type};
	out.grid_cells   = g.cells;
	return out;
}

tl::expected<table_def, ErrorContext> BuildTable(create_table const& declared, std::span<table_def const> earlier)
{
	if (declared.name.empty())
	{
		return tl::make_unexpected(ErrorContext("a table needs a name"));
	}
	for (table_def const& t : earlier)
	{
		if (t.name == declared.name)
		{
			return tl::make_unexpected(ErrorContext("table '", declared.name, "' is defined twice"));
		}
	}

	if (declared.grid)
	{
		return BuildGrid(declared);
	}

	// A suffix is one more Command column after the declared ones, so the store,
	// the picks and every front end see an ordinary column; only the CSV reader
	// treats it differently (it takes the rest of the line).
	std::vector<add_column> widened;
	create_table            withSuffix;
	create_table const*     source = &declared;
	if (!declared.suffix.empty())
	{
		if (declared.pivot)
		{
			return tl::make_unexpected(ErrorContext("table '", declared.name,
				"' is pivoted and has a suffix; a pivot cell cannot carry a command's trailing text"));
		}
		widened.assign(declared.columns.begin(), declared.columns.end());
		widened.push_back(add_column{declared.suffix, column_type::Command});
		withSuffix         = declared;
		withSuffix.columns = widened;
		source             = &withSuffix;
	}
	create_table const& c = *source;

	auto inputs = BuildInputs(c);
	if (!inputs)
	{
		return tl::make_unexpected(std::move(inputs.error()));
	}
	auto columns = BuildColumns(c, *inputs);
	if (!columns)
	{
		return tl::make_unexpected(std::move(columns.error()));
	}

	table_def out{std::string(c.name), std::move(*columns), {}, false, c.read_only};
	out.inputs = std::move(*inputs);
	out.suffix = !declared.suffix.empty();
	if (c.ordering)
	{
		auto order = BuildOrder(c, out.columns);
		if (!order)
		{
			return tl::make_unexpected(std::move(order.error()));
		}
		out.order  = std::move(*order);
		out.unique = c.ordering->unique;
	}
	auto picked = BuildPick(c, out);
	if (!picked)
	{
		return tl::make_unexpected(std::move(picked.error()));
	}
	auto pivoted = BuildPivot(c, out);
	if (!pivoted)
	{
		return tl::make_unexpected(std::move(pivoted.error()));
	}
	return out;
}

}

tl::expected<uint32_t, ErrorContext> schema::FindTable(std::string_view name) const
{
	for (uint32_t i = 0; i < _tables.size(); ++i)
	{
		if (_tables[i].name == name)
		{
			return i;
		}
	}
	return tl::make_unexpected(ErrorContext("no table named '", name, "'"));
}

tl::expected<uint32_t, ErrorContext> schema::FindColumn(uint32_t table, std::string_view name) const
{
	if (table >= _tables.size())
	{
		return tl::make_unexpected(ErrorContext("no table ", table));
	}
	auto const& columns = _tables[table].columns;
	for (uint32_t i = 0; i < columns.size(); ++i)
	{
		if (columns[i].name == name)
		{
			return i;
		}
	}
	return tl::make_unexpected(ErrorContext("table '", _tables[table].name, "' has no column '", name, "'"));
}

namespace
{

// A best table's key domains, resolved against every table of the schema: a
// foreign column may name a table later in the list. Only a best table's keys
// have one, and every one of them must say which: nothing is inferred, and a
// declaration nothing reads is refused like any other.
tl::expected<immutable_array<domain_def>, ErrorContext> BuildDomains(create_table const& c, table_def const& def,
	std::span<table_def const> all)
{
	std::vector<domain_def> out;
	for (uint32_t i = 0; i < c.columns.size(); ++i)
	{
		add_column const& in     = c.columns[i];
		key_domain const& domain = in.domain;
		if (domain.kind != domain_kind::None && !in.key)
		{
			return tl::make_unexpected(ErrorContext("column '", in.name, "' of table '", c.name,
				"' declares a domain, and only a key has one"));
		}
		if (!in.key || def.pick != pick_policy::Best)
		{
			if (domain.kind != domain_kind::None)
			{
				return tl::make_unexpected(ErrorContext("key '", in.name, "' of table '", c.name,
					"' declares a domain, and only a best table's keys are checked over one"));
			}
			continue;
		}
		domain_def resolved{domain.kind};
		switch (domain.kind)
		{
		case domain_kind::None:
			return tl::make_unexpected(ErrorContext("key '", in.name, "' of best table '", c.name,
				"' declares no domain; a key's domain is its members, another table's column, or its rows"));
		case domain_kind::Members:
		{
			if (in.type != column_type::Hash && in.type != column_type::Int)
			{
				return tl::make_unexpected(ErrorContext("key '", in.name, "' of table '", c.name, "' is a ",
					TypeName(in.type), "; only a hash or an int key lists its members"));
			}
			if (domain.members.empty())
			{
				return tl::make_unexpected(ErrorContext("key '", in.name, "' of table '", c.name, "' lists no members"));
			}
			std::vector<int64_t> members;
			for (cell const& m : domain.members)
			{
				auto fits = Check32(def.columns[i], def, m.i);
				if (!fits)
				{
					return tl::make_unexpected(std::move(fits.error()));
				}
				members.push_back(m.i);
			}
			std::sort(members.begin(), members.end());
			auto twice = std::adjacent_find(members.begin(), members.end());
			if (twice != members.end())
			{
				return tl::make_unexpected(ErrorContext("key '", in.name, "' of table '", c.name, "' lists member ",
					*twice, " twice"));
			}
			resolved.members = immutable_array<int64_t>::FromArray(members.data(), members.size());
			break;
		}
		case domain_kind::Column:
		{
			for (uint32_t t = 0; t < all.size() && resolved.table == UINT32_MAX; ++t)
			{
				if (all[t].name == domain.table)
				{
					resolved.table = t;
				}
			}
			if (resolved.table == UINT32_MAX)
			{
				return tl::make_unexpected(ErrorContext("key '", in.name, "' of table '", c.name,
					"' takes its domain from table '", domain.table, "', which the schema does not have"));
			}
			table_def const& source = all[resolved.table];
			for (uint32_t col = 0; col < source.columns.size() && resolved.column == UINT32_MAX; ++col)
			{
				if (source.columns[col].name == domain.column)
				{
					resolved.column = col;
				}
			}
			if (resolved.column == UINT32_MAX)
			{
				return tl::make_unexpected(ErrorContext("key '", in.name, "' of table '", c.name,
					"' takes its domain from column '", domain.column, "' of table '", source.name,
					"', which has no such column"));
			}
			column_type const sourceType = source.columns[resolved.column].type;
			if (sourceType != in.type)
			{
				return tl::make_unexpected(ErrorContext("key '", in.name, "' of table '", c.name, "' is a ",
					TypeName(in.type), ", and the column '", domain.column, "' of table '", source.name,
					"' its domain comes from is a ", TypeName(sourceType)));
			}
			break;
		}
		case domain_kind::Rows:
			break;
		}
		out.push_back(std::move(resolved));
	}
	return immutable_array<domain_def>(shared_array<domain_def>::FromArray(out.data(), out.size()));
}

// Tables whose key domains come from other tables form a DAG, so a table's
// domain is decided by tables whose own domains never wait on it: a change
// re-checks its dependents one step deep, and a machine's proof has an order
// to read domains in.
tl::expected<void, ErrorContext> CheckDomainCycles(std::span<table_def const> tables)
{
	enum : uint8_t { Unseen, Open, Done };
	std::vector<uint8_t>  state(tables.size(), Unseen);
	std::vector<uint32_t> path;
	// Depth first; a table met again while still open closes a cycle.
	auto visit = [&](auto& self, uint32_t t) -> tl::expected<void, ErrorContext>
	{
		state[t] = Open;
		path.push_back(t);
		for (domain_def const& d : tables[t].domains)
		{
			if (d.kind != domain_kind::Column)
			{
				continue;
			}
			if (state[d.table] == Open)
			{
				std::string cycle;
				auto from = std::find(path.begin(), path.end(), d.table);
				for (auto it = from; it != path.end(); ++it)
				{
					cycle = cc(cycle, "'", tables[*it].name, "' -> ");
				}
				cycle = cc(cycle, "'", tables[d.table].name, "'");
				return tl::make_unexpected(ErrorContext("key domains form a cycle: ", cycle,
					"; a key over its own table's rows says rows"));
			}
			if (state[d.table] == Unseen)
			{
				auto ok = self(self, d.table);
				if (!ok)
				{
					return ok;
				}
			}
		}
		path.pop_back();
		state[t] = Done;
		return {};
	};
	for (uint32_t t = 0; t < tables.size(); ++t)
	{
		if (state[t] == Unseen)
		{
			auto ok = visit(visit, t);
			if (!ok)
			{
				return ok;
			}
		}
	}
	return {};
}

}

tl::expected<schema, ErrorContext> BuildSchema(std::span<create_table const> tables)
{
	shared_array<table_def> out(tables.size());
	for (uint32_t t = 0; t < tables.size(); ++t)
	{
		auto built = BuildTable(tables[t], std::span<table_def const>(out.begin(), t));
		if (!built)
		{
			return tl::make_unexpected(ErrorContext("table ", t, ": ", built.error().message));
		}
		out[t] = std::move(*built);
	}
	// After every table is built, since a domain may name a later one.
	for (uint32_t t = 0; t < tables.size(); ++t)
	{
		auto domains = BuildDomains(tables[t], out[t], std::span<table_def const>(out.begin(), out.size()));
		if (!domains)
		{
			return tl::make_unexpected(ErrorContext("table ", t, ": ", domains.error().message));
		}
		out[t].domains = std::move(*domains);
	}
	auto acyclic = CheckDomainCycles(std::span<table_def const>(out.begin(), out.size()));
	if (!acyclic)
	{
		return tl::make_unexpected(std::move(acyclic.error()));
	}
	return schema(immutable_array<table_def>(std::move(out)));
}

}

namespace tabular
{

namespace
{

using detail::held_condition;
using detail::Hold;

bool IsValueColumn(column_type type)
{
	return type != column_type::Condition && type != column_type::Command && type != column_type::String
		&& type != column_type::Band;
}

// A null function is "no script condition"; with data or a release it is a
// closure the host half-built, whose data would leak.
bool IsMalformed(closure const& c)
{
	return c.function == nullptr && (c.data != nullptr || c.release != nullptr);
}

ErrorContext MalformedBand(column_def const& column, table_def const& def, cell band)
{
	return ErrorContext("band '", column.name, "' of '", def.name, "' is ", FloatText(band.v[0]), "..",
		FloatText(band.v[1]), "; a band runs low to high, both ends finite");
}

counted_string Intern(string_in s)
{
	return counted_string::MakeShared(s.text);
}

// Rows at or after row moved down one.
template<typename Map>
void ShiftRows(Map& index, uint32_t row)
{
	for (auto& entry : index)
	{
		for (uint32_t& r : entry.second)
		{
			if (r >= row)
			{
				++r;
			}
		}
	}
}

// Negative, zero or positive as the row sorts before, with or after key, over
// the first key.size() order slots. Order columns are integers, so every
// compared cell is one.
int CompareOrder(std::span<cell const> row, std::span<uint32_t const> orderSlots, std::span<cell const> key)
{
	for (size_t k = 0; k < key.size(); ++k)
	{
		int64_t a = row[orderSlots[k]].i;
		int64_t b = key[k].i;
		if (a != b)
		{
			return a < b ? -1 : 1;
		}
	}
	return 0;
}

// The first row that does not sort before key (lower), or the first that
// sorts after it (upper), by binary search over rows already in order.
size_t OrderBound(std::span<cell const> cells, size_t rowCount, size_t stride,
	std::span<uint32_t const> orderSlots, std::span<cell const> key, bool upper)
{
	size_t first = 0;
	size_t count = rowCount;
	while (count > 0)
	{
		size_t half = count / 2;
		size_t mid  = first + half;
		int    cmp  = CompareOrder(cells.subspan(mid * stride, stride), orderSlots, key);
		if (upper ? cmp <= 0 : cmp < 0)
		{
			first  = mid + 1;
			count -= half + 1;
		}
		else
		{
			count = half;
		}
	}
	return first;
}

}

TableVersion::TableVersion(std::unique_ptr<Data>&& data)
	: _data(std::move(data))
{
}

TableVersion::~TableVersion() = default;

void TableVersion::Release() const
{
	if (_refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
	{
		delete this;
	}
}

relation_store::relation_store(schema&& definition)
	: _storage(std::make_unique<Storage>())
{
	_storage->definition = std::move(definition);
	auto defs = _storage->definition.tables();
	_storage->tables.reserve(defs.size());
	_storage->versions.resize(defs.size());
	_storage->grids.resize(defs.size());
	_storage->dependents.resize(defs.size());
	for (size_t t = 0; t < defs.size(); ++t)
	{
		_storage->tables.emplace_back(defs[t]);
	}
	for (size_t t = 0; t < defs.size(); ++t)
	{
		for (domain_def const& d : defs[t].domains)
		{
			if (d.kind != domain_kind::Column)
			{
				continue;
			}
			auto& list = _storage->dependents[d.table];
			if (std::find(list.begin(), list.end(), t) == list.end())
			{
				list.push_back(static_cast<uint32_t>(t));
			}
		}
	}
	for (size_t t = 0; t < defs.size(); ++t)
	{
		if (defs[t].is_grid)
		{
			_storage->grids[t] = Storage::NewGrid(GridVersion::Data{defs[t]});
		}
		// A mutable best table is versioned too, so a pick holds one version of
		// it while contributors insert, and its insert is checked like a load.
		// So is a mutable table some best table's domain comes from: an insert
		// into it is refused when a dependent would not stay total, and only a
		// version built beside the current one is refused without undoing
		// anything.
		else if (defs[t].read_only || defs[t].pick == pick_policy::Best || !_storage->dependents[t].empty())
		{
			_storage->versions[t] = Storage::NewVersion(defs[t], table_rows(defs[t]));
		}
	}
}

relation_store::~relation_store() = default;
relation_store::relation_store(relation_store&&) noexcept = default;
relation_store& relation_store::operator=(relation_store&&) noexcept = default;

schema const& relation_store::definition() const
{
	return _storage->definition;
}

// A grid's rows live in its GridVersion; the empty row layout beside it would
// answer 0 forever, or be indexed past its end.
size_t relation_store::RowCount(uint32_t table) const
{
	if (GridVersion const* grid = _storage->GridOf(table))
	{
		return grid->Rows();
	}
	return _storage->RowsOf(table).RowCount();
}

uint64_t relation_store::OwnerOf(uint32_t table, size_t row) const
{
	// A grid has one contributor, so every row of it is that owner's.
	if (GridVersion const* grid = _storage->GridOf(table))
	{
		return grid->Owner();
	}
	return _storage->RowsOf(table).owners[row];
}

uint64_t relation_store::Generation(uint32_t table) const
{
	if (GridVersion const* grid = _storage->GridOf(table))
	{
		return grid->Generation();
	}
	return _storage->RowsOf(table).generation;
}

namespace
{

// What one row must be, whichever change brings it: a value fits its type, a
// command does something, a weight can be drawn, and a band is well formed and
// inside its input's range. A property of the row alone, so Load and Insert
// ask the same question.
tl::expected<void, ErrorContext> CheckRow(table_def const& def, slot_layout const& slots, std::span<cell const> values,
	std::span<std::string_view const> commands)
{
	for (uint32_t c = 0; c < def.columns.size(); ++c)
	{
		column_def const& column = def.columns[c];
		// A row that does nothing is a property of the row, like a band's shape.
		if (column.type == column_type::Command && commands[slots.slot[c]].empty())
		{
			return tl::make_unexpected(ErrorContext("column '", column.name, "' of '", def.name, "' got an empty command"));
		}
		if (IsValueColumn(column.type))
		{
			auto fits = Check32(column, def, values[slots.slot[c]].i);
			if (!fits)
			{
				return fits;
			}
		}
		if (c == def.weight)
		{
			// Every key run then has a positive total by construction (R7),
			// and a zero would be a row that can never be drawn. A subnormal
			// weight is refused too: a subnormal total lets the draw's product
			// round up to the total, and PickWeighted's fallback would run.
			double const weight = values[slots.slot[c]].d;
			if (!(std::isnormal(weight) && weight > 0))
			{
				return tl::make_unexpected(ErrorContext("weight '", column.name, "' of '", def.name, "' is ",
					FloatText(weight), "; a weight is a normal positive number"));
			}
		}
		if (column.type == column_type::Band)
		{
			cell const band = values[slots.slot[c]];
			if (!IsWellFormedBand(band))
			{
				return tl::make_unexpected(MalformedBand(column, def, band));
			}
			input_def const& in = def.inputs[column.input];
			// Clamped inputs never reach past the range, so a band that does
			// is partly dead.
			if (in.ranged && !(in.lo <= band.v[0] && band.v[1] <= in.hi))
			{
				return tl::make_unexpected(ErrorContext("band '", column.name, "' of '", def.name, "' is ",
					FloatText(band.v[0]), "..", FloatText(band.v[1]), "; it lies inside the range ",
					FloatText(in.lo), "..", FloatText(in.hi), " of input '", in.name, "'"));
			}
		}
	}
	return {};
}

std::vector<held_condition> HoldAll(std::span<closure const> closures)
{
	std::vector<held_condition> held;
	held.reserve(closures.size());
	for (closure const& c : closures)
	{
		held.push_back(Hold(c));
	}
	return held;
}

}

tl::expected<void, ErrorContext> relation_store::Insert(insert_row const& command)
{
	if (command.report)
	{
		command.report->clear();
	}
	// The row's closures are ours from here on: a refusal releases them when
	// held goes out of scope, and a success moves them into the rows.
	std::vector<held_condition> held = HoldAll(command.conditions);

	auto defs = _storage->definition.tables();
	if (command.table >= defs.size())
	{
		return tl::make_unexpected(ErrorContext("insert into table ", command.table, ", but the schema has ", defs.size(), " tables"));
	}
	table_def const& def  = defs[command.table];
	auto&            rows = _storage->tables[command.table];
	if (_storage->GridOf(command.table))
	{
		return tl::make_unexpected(ErrorContext("table '", def.name, "' is a grid; it is loaded with LoadGrid"));
	}
	if (def.read_only)
	{
		return tl::make_unexpected(ErrorContext("table '", def.name, "' is read-only; its rows come only from Load"));
	}
	if (command.values.size() != rows.valueCount)
	{
		return tl::make_unexpected(ErrorContext("insert into '", def.name, "' has ", command.values.size(),
			" values, but the table has ", rows.valueCount, " value columns"));
	}
	if (command.conditions.size() != rows.conditionCount)
	{
		return tl::make_unexpected(ErrorContext("insert into '", def.name, "' has ", command.conditions.size(),
			" conditions, but the table has ", rows.conditionCount, " condition columns"));
	}
	if (command.commands.size() != rows.commandCount)
	{
		return tl::make_unexpected(ErrorContext("insert into '", def.name, "' has ", command.commands.size(),
			" commands, but the table has ", rows.commandCount, " command columns"));
	}
	if (command.strings.size() != rows.stringCount)
	{
		return tl::make_unexpected(ErrorContext("insert into '", def.name, "' has ", command.strings.size(),
			" strings, but the table has ", rows.stringCount, " string columns"));
	}
	for (uint32_t c = 0; c < def.columns.size(); ++c)
	{
		if (def.columns[c].type == column_type::Condition && IsMalformed(command.conditions[rows.slot[c]]))
		{
			return tl::make_unexpected(ErrorContext("column '", def.columns[c].name, "' of '", def.name,
				"' got a condition with no function but data or a release"));
		}
	}
	auto shaped = CheckRow(def, rows, command.values, command.commands);
	if (!shaped)
	{
		return tl::make_unexpected(std::move(shaped.error()));
	}

	// A versioned table: an insert is the owner's rows so far plus this one,
	// merged and checked exactly as a load is.
	if (_storage->versions[command.table])
	{
		incoming_rows const in{command.owner, 1, command.values, command.commands, held, command.strings, true};
		auto published = _storage->Publish(command.table, in, nullptr, command.report, "inserted row", "this insert");
		if (!published)
		{
			if (command.report)
			{
				command.report->clear();
			}
			return tl::make_unexpected(std::move(published.error()));
		}
		return {};
	}

	size_t count = rows.owners.size();
	size_t pos   = count;
	if (!rows.orderSlots.empty())
	{
		std::vector<cell> key;
		key.reserve(rows.orderSlots.size());
		for (uint32_t s : rows.orderSlots)
		{
			key.push_back(command.values[s]);
		}
		size_t lower = OrderBound(rows.cells, count, rows.valueCount, rows.orderSlots, key, false);
		pos          = OrderBound(rows.cells, count, rows.valueCount, rows.orderSlots, key, true);
		if (def.unique && lower != pos)
		{
			return tl::make_unexpected(ErrorContext("table '", def.name, "' has a unique order, and owner ",
				rows.owners[lower], " already has a row with this key"));
		}
	}

	rows.cells.insert(rows.cells.begin() + pos * rows.valueCount, command.values.begin(), command.values.end());
	rows.conditions.insert(rows.conditions.begin() + pos * rows.conditionCount,
		std::make_move_iterator(held.begin()), std::make_move_iterator(held.end()));
	rows.commands.insert(rows.commands.begin() + pos * rows.commandCount,
		command.commands.begin(), command.commands.end());
	std::vector<counted_string> interned;
	interned.reserve(command.strings.size());
	for (string_in s : command.strings)
	{
		interned.push_back(Intern(s));
	}
	rows.strings.insert(rows.strings.begin() + pos * rows.stringCount,
		std::make_move_iterator(interned.begin()), std::make_move_iterator(interned.end()));
	rows.owners.insert(rows.owners.begin() + pos, command.owner);

	auto row = static_cast<uint32_t>(pos);
	for (uint32_t c = 0; c < def.columns.size(); ++c)
	{
		if (!def.columns[c].key)
		{
			continue;
		}
		// Rows after the insertion point moved down one. An append moves
		// nothing, and is the common case: every unordered insert.
		if (pos != count)
		{
			ShiftRows(rows.index[c], row);
			ShiftRows(rows.stringIndex[c], row);
		}
		// Each list stays in row order, which is the order Find returns.
		auto& list = def.columns[c].type == column_type::String
			? rows.stringIndex[c][std::string_view(rows.strings[pos * rows.stringCount + rows.slot[c]])]
			: rows.index[c][command.values[rows.slot[c]].i];
		list.insert(std::lower_bound(list.begin(), list.end(), row), row);
	}

	++rows.generation;
	return {};
}

void relation_store::Retract(retract_owner const& command)
{
	if (command.report)
	{
		command.report->clear();
	}
	auto defs = _storage->definition.tables();
	// Which tables lost rows: their dependents' domains changed with them.
	std::vector<uint8_t> touched(defs.size(), 0);
	for (uint32_t t = 0; t < defs.size(); ++t)
	{
		if (_storage->GridOf(t))
		{
			GridVersion::Data const& data = *_storage->grids[t]->_data;
			// A grid of zero rows still holds its owner's column keys.
			if (data.owner == command.owner && (data.rows != 0 || data.columns != 0))
			{
				GridVersion::Data empty{defs[t]};
				empty.generation = data.generation + 1;
				_storage->grids[t] = Storage::NewGrid(std::move(empty));
			}
			continue;
		}
		if (_storage->versions[t])
		{
			// The version without the owner's rows, put in place like any load.
			// A change that brings no rows is never checked, so this is never
			// refused: crash recovery cannot fail.
			auto removed = _storage->Publish(t, incoming_rows{command.owner}, nullptr, nullptr, "", "");
			touched[t] = removed && *removed ? 1 : 0;
			continue;
		}
		auto&  rows = _storage->tables[t];
		size_t kept = 0;
		for (size_t r = 0; r < rows.owners.size(); ++r)
		{
			if (rows.owners[r] == command.owner)
			{
				continue;
			}
			if (kept != r)
			{
				std::copy_n(rows.cells.begin() + r * rows.valueCount, rows.valueCount,
					rows.cells.begin() + kept * rows.valueCount);
				std::move(rows.conditions.begin() + r * rows.conditionCount,
					rows.conditions.begin() + (r + 1) * rows.conditionCount,
					rows.conditions.begin() + kept * rows.conditionCount);
				std::move(rows.commands.begin() + r * rows.commandCount,
					rows.commands.begin() + (r + 1) * rows.commandCount,
					rows.commands.begin() + kept * rows.commandCount);
				std::move(rows.strings.begin() + r * rows.stringCount,
					rows.strings.begin() + (r + 1) * rows.stringCount,
					rows.strings.begin() + kept * rows.stringCount);
				rows.owners[kept] = rows.owners[r];
			}
			++kept;
		}
		if (kept == rows.owners.size())
		{
			// None of this owner's rows are here: nothing changed, so the
			// generation must not either.
			continue;
		}
		rows.owners.resize(kept);
		rows.cells.resize(kept * rows.valueCount);
		rows.conditions.resize(kept * rows.conditionCount);
		rows.commands.resize(kept * rows.commandCount);
		rows.strings.resize(kept * rows.stringCount);
		// A retract happens on reload, not per frame, so rebuilding the key
		// indexes is cheaper to get right than patching shifted row numbers.
		rows.RebuildIndex(defs[t]);
		++rows.generation;
		touched[t] = 1;
	}
	// Surveyed once every table is retracted, so a dependent reads the domain
	// the whole retract leaves.
	if (command.report)
	{
		_storage->Survey(touched, *command.report);
	}
}

tl::expected<void, ErrorContext> relation_store::Load(load_rows const& command)
{
	if (command.report)
	{
		command.report->clear();
	}
	// Every closure the load carries is ours from here on: a refusal releases
	// them when held goes out of scope, and a success moves them into rows.
	std::vector<held_condition> held = HoldAll(command.conditions);

	auto defs = _storage->definition.tables();
	if (command.table >= defs.size())
	{
		return tl::make_unexpected(ErrorContext("load into table ", command.table, ", but the schema has ", defs.size(), " tables"));
	}
	table_def const& def = defs[command.table];
	if (_storage->GridOf(command.table))
	{
		return tl::make_unexpected(ErrorContext("table '", def.name, "' is a grid; it is loaded with LoadGrid"));
	}
	if (!def.read_only)
	{
		return tl::make_unexpected(ErrorContext("table '", def.name, "' is not read-only; its rows come from Insert"));
	}
	table_rows const& current = _storage->versions[command.table]->_data->rows;
	// Divided, not multiplied: a count large enough to wrap the product would
	// pass a size check against empty spans and read past them.
	auto spans = [&](size_t size, uint32_t width)
	{
		return width == 0 ? size == 0 : size % width == 0 && size / width == command.count;
	};
	if (!spans(command.values.size(), current.valueCount))
	{
		return tl::make_unexpected(ErrorContext("load into '", def.name, "' of ", command.count, " rows has ",
			command.values.size(), " values, but the table has ", current.valueCount, " value columns"));
	}
	if (!spans(command.commands.size(), current.commandCount))
	{
		return tl::make_unexpected(ErrorContext("load into '", def.name, "' of ", command.count, " rows has ",
			command.commands.size(), " commands, but the table has ", current.commandCount, " command columns"));
	}
	if (!spans(command.conditions.size(), current.conditionCount))
	{
		return tl::make_unexpected(ErrorContext("load into '", def.name, "' of ", command.count, " rows has ",
			command.conditions.size(), " conditions, but the table has ", current.conditionCount, " condition columns"));
	}
	if (!spans(command.strings.size(), current.stringCount))
	{
		return tl::make_unexpected(ErrorContext("load into '", def.name, "' of ", command.count, " rows has ",
			command.strings.size(), " strings, but the table has ", current.stringCount, " string columns"));
	}
	for (size_t i = 0; i < command.conditions.size(); ++i)
	{
		if (IsMalformed(command.conditions[i]))
		{
			size_t const r = i / current.conditionCount;
			if (command.refusal)
			{
				command.refusal->row = r;
			}
			return tl::make_unexpected(ErrorContext("load into '", def.name,
				"' got a condition with no function but data or a release")
				.with("row ", r));
		}
	}
	for (size_t r = 0; r < command.count; ++r)
	{
		auto shaped = CheckRow(def, current, command.values.subspan(r * current.valueCount, current.valueCount),
			command.commands.subspan(r * current.commandCount, current.commandCount));
		if (!shaped)
		{
			if (command.refusal)
			{
				command.refusal->row = r;
			}
			return tl::make_unexpected(std::move(shaped.error()).with("row ", r));
		}
	}

	incoming_rows const in{command.owner, command.count, command.values, command.commands, held, command.strings, false};
	auto published = _storage->Publish(command.table, in, command.refusal, command.report, "loaded row", "this load");
	if (!published)
	{
		if (command.report)
		{
			command.report->clear();
		}
		return tl::make_unexpected(std::move(published.error()));
	}
	// A load of no rows is a bare retract from this table: never refused, and
	// what it leaves is reported instead.
	if (command.count == 0 && *published && command.report)
	{
		std::vector<uint8_t> touched(defs.size(), 0);
		touched[command.table] = 1;
		_storage->Survey(touched, *command.report);
	}
	return {};
}

table_rows const& relation_store::Storage::RowsDuring(uint32_t table, uint32_t changed, table_rows const* candidate) const
{
	return table == changed && candidate ? *candidate : RowsOf(table);
}

void relation_store::Storage::DomainOf(uint32_t table, table_rows const& rows, uint32_t changed,
	table_rows const* candidate, detail::key_domains& out) const
{
	table_def const& def = definition.tables()[table];
	out.overRows.assign(def.domains.size(), 0);
	out.values.resize(def.domains.size());
	uint32_t k = 0;
	for (uint32_t c = 0; c < def.columns.size() && k < def.domains.size(); ++c)
	{
		if (!def.columns[c].key)
		{
			continue;
		}
		domain_def const& d = def.domains[k];
		detail::DeclaredDomain(d, d.kind == domain_kind::Column ? &RowsDuring(d.table, changed, candidate) : nullptr, k,
			out);
		++k;
	}
	detail::ResolveDomain(rows, out);
}

tl::expected<void, ErrorContext> relation_store::Storage::CheckDependents(uint32_t changed, table_rows const& candidate,
	std::vector<run_note>* report, char const* change) const
{
	auto defs = definition.tables();
	detail::key_domains domains;
	for (uint32_t dependent : dependents[changed])
	{
		table_rows const&   rows = RowsOf(dependent);
		DomainOf(dependent, rows, changed, &candidate, domains);
		std::vector<size_t> kept(rows.RowCount(), SIZE_MAX);
		auto checked = detail::CheckBest(
			detail::check_subject{dependent, defs[dependent], rows, kept, "row", change, defs[changed].name},
			domains, nullptr, report);
		if (!checked)
		{
			// Which change found it: the dependent may have been left so by an
			// earlier bare retract, which nothing refused.
			return tl::make_unexpected(std::move(checked.error()).with(change, " changes table '", defs[changed].name, "'"));
		}
	}
	return {};
}

void relation_store::Storage::Survey(std::vector<uint8_t> const& touched, std::vector<run_note>& report) const
{
	auto defs = definition.tables();
	std::vector<uint8_t> surveyed(defs.size(), 0);
	for (uint32_t t = 0; t < defs.size(); ++t)
	{
		if (!touched[t])
		{
			continue;
		}
		surveyed[t] = defs[t].pick == pick_policy::Best ? 1 : 0;
		for (uint32_t dependent : dependents[t])
		{
			surveyed[dependent] = 1;
		}
	}
	detail::key_domains domains;
	for (uint32_t t = 0; t < defs.size(); ++t)
	{
		if (!surveyed[t])
		{
			continue;
		}
		DomainOf(t, RowsOf(t), UINT32_MAX, nullptr, domains);
		detail::check_subject survey{t, defs[t], RowsOf(t), {}, "", "", "", true};
		auto surveyed = detail::CheckBest(survey, domains, nullptr, &report);
		(void)surveyed;   // a survey refuses nothing
	}
}

tl::expected<bool, ErrorContext> relation_store::Storage::Publish(uint32_t table, incoming_rows const& in,
	load_refusal* refusal, std::vector<run_note>* report, char const* noun, char const* change)
{
	table_def const&  def     = definition.tables()[table];
	table_rows const& current = version_access::Rows(*versions[table]);

	// Every row the next version holds, as (from the change?, row). The version
	// is a function of the owners' rows, not of the order they arrived in, so
	// rows sort by the order columns, then owner id, then index within the
	// owner's rows: an owner reloading identical rows leaves every row where it
	// was, and an insert lands after the owner's rows with an equal order key.
	struct source
	{
		bool   loaded;
		size_t row;
	};
	std::vector<source> order;
	order.reserve(current.RowCount() + in.count);
	uint32_t firstNew = 0;
	for (size_t r = 0; r < current.RowCount(); ++r)
	{
		if (current.owners[r] != in.owner)
		{
			order.push_back({false, r});
		}
		else if (in.keepOwner)
		{
			order.push_back({false, r});
			firstNew = std::max(firstNew, current.ownerRows[r] + 1);
		}
	}
	if (order.size() == current.RowCount() && in.count == 0)
	{
		return false;   // nothing of this owner's here and nothing new: no change
	}
	for (size_t r = 0; r < in.count; ++r)
	{
		order.push_back({true, r});
	}
	auto cellsOf = [&](source s) -> std::span<cell const>
	{
		return s.loaded ? in.values.subspan(s.row * current.valueCount, current.valueCount)
		                : std::span<cell const>(current.cells).subspan(s.row * current.valueCount, current.valueCount);
	};
	auto ownerOf = [&](source s) { return s.loaded ? in.owner : current.owners[s.row]; };
	auto ownerRowOf = [&](source s) -> size_t { return s.loaded ? firstNew + s.row : current.ownerRows[s.row]; };
	auto keyCompare = [&](source a, source b) -> int
	{
		std::span<cell const> ac = cellsOf(a);
		std::span<cell const> bc = cellsOf(b);
		for (uint32_t s : current.orderSlots)
		{
			if (ac[s].i != bc[s].i)
			{
				return ac[s].i < bc[s].i ? -1 : 1;
			}
		}
		return 0;
	};
	// (owner, index) is unique per row, so the order is total and the sort
	// needs no stability.
	std::sort(order.begin(), order.end(), [&](source a, source b)
	{
		int const byKey = keyCompare(a, b);
		if (byKey != 0)
		{
			return byKey < 0;
		}
		if (ownerOf(a) != ownerOf(b))
		{
			return ownerOf(a) < ownerOf(b);
		}
		return ownerRowOf(a) < ownerRowOf(b);
	});
	if (!current.orderSlots.empty() && def.unique)
	{
		for (size_t i = 1; i < order.size(); ++i)
		{
			if (keyCompare(order[i - 1], order[i]) == 0)
			{
				// Survivors are unique among themselves, so at least one of a
				// clashing pair is a new row; name the new one, and the other as
				// the row it clashes with.
				source const earlier = order[i - 1];
				source const later   = order[i];
				source const mine    = later.loaded ? later : earlier;
				source const other   = later.loaded ? earlier : later;
				if (refusal)
				{
					refusal->row     = mine.row;
					refusal->earlier = other.loaded ? other.row : SIZE_MAX;
				}
				std::string key;
				for (uint32_t s : current.orderSlots)
				{
					key = cc(key, key.empty() ? "" : ", ", cellsOf(mine)[s].i);
				}
				return tl::make_unexpected(ErrorContext("table '", def.name, "' has a unique order, and owner ",
					ownerOf(other), " already has a row with key (", key, ") of ", noun, " ", mine.row));
			}
		}
	}

	// A weighted run is drawn from by laying its weights end to end, so its total
	// must be a number. Each weight is normal and positive (checked per row),
	// so the total can only fail by overflowing; a run no new row joins was
	// summed when it was published.
	if (def.pick == pick_policy::Weighted && in.count > 0)
	{
		uint32_t const weightSlot = current.slot[def.weight];
		auto sameKey = [&](source a, source b)
		{
			for (uint32_t k = 0; k < def.key_count; ++k)
			{
				uint32_t const s = current.orderSlots[k];
				if (cellsOf(a)[s].i != cellsOf(b)[s].i)
				{
					return false;
				}
			}
			return true;
		};
		size_t start = 0;
		while (start < order.size())
		{
			size_t end        = start;
			double total      = 0;
			size_t lastLoaded = SIZE_MAX;
			while (end < order.size() && sameKey(order[start], order[end]))
			{
				total += cellsOf(order[end])[weightSlot].d;
				if (order[end].loaded)
				{
					lastLoaded = order[end].row;
				}
				++end;
			}
			if (!std::isfinite(total))
			{
				if (refusal)
				{
					refusal->row = lastLoaded;
				}
				std::string key;
				for (uint32_t k = 0; k < def.key_count; ++k)
				{
					key = cc(key, key.empty() ? "" : ", ", cellsOf(order[start])[current.orderSlots[k]].i);
				}
				return tl::make_unexpected(ErrorContext("table '", def.name, "' key (", key,
					"): the weights total more than a double holds"));
			}
			start = end;
		}
	}

	// The current version may be held, so survivors are copied, never moved.
	// Built before the checks, which read the version it would publish.
	table_rows next(def);
	next.cells.reserve(order.size() * next.valueCount);
	next.commands.reserve(order.size() * next.commandCount);
	next.conditions.reserve(order.size() * next.conditionCount);
	next.strings.reserve(order.size() * next.stringCount);
	next.owners.reserve(order.size());
	next.ownerRows.reserve(order.size());
	std::vector<size_t> fresh;
	fresh.reserve(order.size());
	for (source s : order)
	{
		std::span<cell const> from = cellsOf(s);
		next.cells.insert(next.cells.end(), from.begin(), from.end());
		for (uint32_t k = 0; k < next.conditionCount; ++k)
		{
			next.conditions.push_back(s.loaded ? in.conditions[s.row * next.conditionCount + k]
			                                   : current.conditions[s.row * next.conditionCount + k]);
		}
		for (uint32_t k = 0; k < next.commandCount; ++k)
		{
			if (s.loaded)
			{
				next.commands.emplace_back(in.commands[s.row * next.commandCount + k]);
			}
			else
			{
				next.commands.push_back(current.commands[s.row * next.commandCount + k]);
			}
		}
		for (uint32_t k = 0; k < next.stringCount; ++k)
		{
			next.strings.push_back(s.loaded ? Intern(in.strings[s.row * next.stringCount + k])
			                                : current.strings[s.row * next.stringCount + k]);
		}
		next.owners.push_back(ownerOf(s));
		next.ownerRows.push_back(static_cast<uint32_t>(ownerRowOf(s)));
		fresh.push_back(s.loaded ? s.row : SIZE_MAX);
	}
	next.RebuildIndex(def);

	// A published version is total, so a change that brings rows checks every
	// key tuple of the domain of the version it would publish: a run it never
	// touches may have been stranded by a bare Retract, and a reload can take a
	// run's only default away. A change of no rows is never refused (Retract
	// relies on it, so crash recovery cannot fail).
	if (in.count > 0)
	{
		if (def.pick == pick_policy::Best)
		{
			detail::key_domains domains;
			DomainOf(table, next, table, &next, domains);
			auto checked = detail::CheckBest(detail::check_subject{table, def, next, fresh, noun, change}, domains,
				refusal, report);
			if (!checked)
			{
				return tl::make_unexpected(std::move(checked.error()));
			}
		}
		auto dependent = CheckDependents(table, next, report, change);
		if (!dependent)
		{
			return tl::make_unexpected(std::move(dependent.error()));
		}
		// The changed table was checked before its dependents; a report reads
		// by table index, as a survey's does.
		if (report)
		{
			std::stable_sort(report->begin(), report->end(),
				[](run_note const& a, run_note const& b) { return a.table < b.table; });
		}
	}

	next.generation = current.generation + 1;
	versions[table] = NewVersion(def, std::move(next));
	return true;
}

tl::expected<counted_ptr<TableVersion const>, ErrorContext> relation_store::Version(uint32_t table) const
{
	auto defs = _storage->definition.tables();
	if (table >= defs.size())
	{
		return tl::make_unexpected(ErrorContext("no table ", table));
	}
	if (_storage->GridOf(table))
	{
		return tl::make_unexpected(ErrorContext("table '", defs[table].name, "' is a grid; read it with Grid"));
	}
	if (!_storage->versions[table])
	{
		return tl::make_unexpected(ErrorContext("table '", defs[table].name, "' is mutable, not a best table, and no domain comes from it, so it has no version"));
	}
	return _storage->versions[table];
}

namespace
{

// Checks a (row, column) address and the column's kind, so each reader can
// refuse a bad address with the same message.
tl::expected<column_def const*, ErrorContext> CheckAddress(table_def const& def, size_t rowCount,
	size_t row, uint32_t column, column_type const* wanted, char const* wantedName)
{
	if (row >= rowCount)
	{
		return tl::make_unexpected(ErrorContext("table '", def.name, "' has no row ", row));
	}
	if (column >= def.columns.size())
	{
		return tl::make_unexpected(ErrorContext("table '", def.name, "' has no column ", column));
	}
	column_def const& c = def.columns[column];
	bool matches = wanted ? c.type == *wanted : IsValueColumn(c.type);
	if (!matches)
	{
		return tl::make_unexpected(ErrorContext("column '", c.name, "' of '", def.name, "' is not a ", wantedName));
	}
	return &c;
}

tl::expected<cell, ErrorContext> ReadValue(table_def const& def, table_rows const& rows, size_t row, uint32_t column)
{
	auto checked = CheckAddress(def, rows.RowCount(), row, column, nullptr, "value column");
	if (!checked)
	{
		return tl::make_unexpected(std::move(checked.error()));
	}
	return rows.cells[row * rows.valueCount + rows.slot[column]];
}

tl::expected<std::string_view, ErrorContext> ReadCommand(table_def const& def, table_rows const& rows, size_t row, uint32_t column)
{
	column_type const wanted  = column_type::Command;
	auto              checked = CheckAddress(def, rows.RowCount(), row, column, &wanted, "command");
	if (!checked)
	{
		return tl::make_unexpected(std::move(checked.error()));
	}
	return std::string_view(rows.commands[row * rows.commandCount + rows.slot[column]]);
}

tl::expected<bool, ErrorContext> ReadHolds(table_def const& def, table_rows const& rows, size_t row, uint32_t column,
	void const* this_in)
{
	column_type const wanted  = column_type::Condition;
	auto              checked = CheckAddress(def, rows.RowCount(), row, column, &wanted, "condition");
	if (!checked)
	{
		return tl::make_unexpected(std::move(checked.error()));
	}
	closure const& c     = rows.conditions[row * rows.conditionCount + rows.slot[column]]->get();
	if (c.function == nullptr)
	{
		return true;   // no script condition
	}
	ErrorContext   error;
	bool           holds = c.function(c.data, this_in, &error);
	// An error may arrive as context alone, from a closure wrapping another's.
	if (!error.empty())
	{
		return tl::make_unexpected(std::move(error));
	}
	return holds;
}

tl::expected<counted_string, ErrorContext> ReadString(table_def const& def, table_rows const& rows, size_t row, uint32_t column)
{
	column_type const wanted  = column_type::String;
	auto              checked = CheckAddress(def, rows.RowCount(), row, column, &wanted, "string");
	if (!checked)
	{
		return tl::make_unexpected(std::move(checked.error()));
	}
	return rows.strings[row * rows.stringCount + rows.slot[column]];
}

tl::expected<std::vector<size_t>, ErrorContext> FindText(table_def const& def, table_rows const& rows, uint32_t column,
	std::string_view text)
{
	if (column >= def.columns.size())
	{
		return tl::make_unexpected(ErrorContext("table '", def.name, "' has no column ", column));
	}
	column_def const& c = def.columns[column];
	if (c.type != column_type::String || !c.key)
	{
		return tl::make_unexpected(ErrorContext("column '", c.name, "' of '", def.name,
			"' is not a string key, so it cannot be looked up by text"));
	}
	std::vector<size_t> out;
	auto const& index = rows.stringIndex[column];
	auto        found = index.find(text);
	if (found != index.end())
	{
		out.assign(found->second.begin(), found->second.end());
	}
	return out;
}

tl::expected<std::vector<size_t>, ErrorContext> FindKey(table_def const& def, table_rows const& rows, uint32_t column, cell value)
{
	if (column >= def.columns.size())
	{
		return tl::make_unexpected(ErrorContext("table '", def.name, "' has no column ", column));
	}
	if (!def.columns[column].key)
	{
		return tl::make_unexpected(ErrorContext("column '", def.columns[column].name, "' of '", def.name,
			"' is not a key, so it cannot be looked up"));
	}
	if (def.columns[column].type == column_type::String)
	{
		return tl::make_unexpected(ErrorContext("column '", def.columns[column].name, "' of '", def.name,
			"' is a string; look it up by text"));
	}
	auto fits = Check32(def.columns[column], def, value.i);
	if (!fits)
	{
		return tl::make_unexpected(std::move(fits.error()));
	}

	std::vector<size_t> out;
	auto const& index = rows.index[column];
	auto        found = index.find(value.i);
	if (found != index.end())
	{
		out.assign(found->second.begin(), found->second.end());
	}
	return out;
}

tl::expected<row_range, ErrorContext> FindPrefix(table_def const& def, table_rows const& rows, std::span<cell const> prefix)
{
	if (rows.orderSlots.empty())
	{
		return tl::make_unexpected(ErrorContext("table '", def.name, "' has no order to look up by"));
	}
	if (prefix.size() > rows.orderSlots.size())
	{
		return tl::make_unexpected(ErrorContext("a prefix of ", prefix.size(), " values for table '", def.name,
			"', whose order has ", rows.orderSlots.size(), " columns"));
	}
	for (size_t k = 0; k < prefix.size(); ++k)
	{
		auto fits = Check32(def.columns[def.order[static_cast<uint32_t>(k)]], def, prefix[k].i);
		if (!fits)
		{
			return tl::make_unexpected(std::move(fits.error()));
		}
	}
	size_t count = rows.RowCount();
	return row_range{
		OrderBound(rows.cells, count, rows.valueCount, rows.orderSlots, prefix, false),
		OrderBound(rows.cells, count, rows.valueCount, rows.orderSlots, prefix, true),
	};
}

}

table_def const& TableVersion::definition() const { return _data->def; }
size_t   TableVersion::RowCount() const           { return _data->rows.RowCount(); }
uint64_t TableVersion::OwnerOf(size_t row) const  { return _data->rows.owners[row]; }
uint64_t TableVersion::Generation() const         { return _data->rows.generation; }

tl::expected<cell, ErrorContext> TableVersion::Value(size_t row, uint32_t column) const
{
	return ReadValue(_data->def, _data->rows, row, column);
}

tl::expected<std::string_view, ErrorContext> TableVersion::CommandAt(size_t row, uint32_t column) const
{
	return ReadCommand(_data->def, _data->rows, row, column);
}

tl::expected<std::vector<size_t>, ErrorContext> TableVersion::Find(uint32_t column, cell value) const
{
	return FindKey(_data->def, _data->rows, column, value);
}

tl::expected<row_range, ErrorContext> TableVersion::FindOrdered(std::span<cell const> prefix) const
{
	return FindPrefix(_data->def, _data->rows, prefix);
}

tl::expected<cell, ErrorContext> relation_store::Value(uint32_t table, size_t row, uint32_t column) const
{
	auto rows = _storage->RowTable(table);
	if (!rows)
	{
		return tl::make_unexpected(std::move(rows.error()));
	}
	return ReadValue(_storage->definition.tables()[table], **rows, row, column);
}

tl::expected<counted_string, ErrorContext> TableVersion::StringAt(size_t row, uint32_t column) const
{
	return ReadString(_data->def, _data->rows, row, column);
}

tl::expected<std::vector<size_t>, ErrorContext> TableVersion::Find(uint32_t column, std::string_view text) const
{
	return FindText(_data->def, _data->rows, column, text);
}

tl::expected<counted_string, ErrorContext> relation_store::StringAt(uint32_t table, size_t row, uint32_t column) const
{
	auto rows = _storage->RowTable(table);
	if (!rows)
	{
		return tl::make_unexpected(std::move(rows.error()));
	}
	return ReadString(_storage->definition.tables()[table], **rows, row, column);
}

tl::expected<std::vector<size_t>, ErrorContext> relation_store::Find(uint32_t table, uint32_t column, std::string_view text) const
{
	auto rows = _storage->RowTable(table);
	if (!rows)
	{
		return tl::make_unexpected(std::move(rows.error()));
	}
	return FindText(_storage->definition.tables()[table], **rows, column, text);
}

tl::expected<bool, ErrorContext> relation_store::Holds(uint32_t table, size_t row, uint32_t column, void const* this_in) const
{
	auto rows = _storage->RowTable(table);
	if (!rows)
	{
		return tl::make_unexpected(std::move(rows.error()));
	}
	return ReadHolds(_storage->definition.tables()[table], **rows, row, column, this_in);
}

tl::expected<bool, ErrorContext> TableVersion::Holds(size_t row, uint32_t column, void const* this_in) const
{
	return ReadHolds(_data->def, _data->rows, row, column, this_in);
}

tl::expected<std::string_view, ErrorContext> relation_store::CommandAt(uint32_t table, size_t row, uint32_t column) const
{
	auto rows = _storage->RowTable(table);
	if (!rows)
	{
		return tl::make_unexpected(std::move(rows.error()));
	}
	return ReadCommand(_storage->definition.tables()[table], **rows, row, column);
}

tl::expected<std::vector<size_t>, ErrorContext> relation_store::Find(uint32_t table, uint32_t column, cell value) const
{
	auto rows = _storage->RowTable(table);
	if (!rows)
	{
		return tl::make_unexpected(std::move(rows.error()));
	}
	return FindKey(_storage->definition.tables()[table], **rows, column, value);
}

tl::expected<row_range, ErrorContext> relation_store::FindOrdered(uint32_t table, std::span<cell const> prefix) const
{
	auto rows = _storage->RowTable(table);
	if (!rows)
	{
		return tl::make_unexpected(std::move(rows.error()));
	}
	return FindPrefix(_storage->definition.tables()[table], **rows, prefix);
}

}
