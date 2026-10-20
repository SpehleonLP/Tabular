#include "tabular/schema_yaml.h"
#include "csv_cells.h"
#include "machine_internal.h"
#include "Yaml/yaml_markup.h"
#include "Yaml/yaml_parse.h"

#include "Support/strings.h"

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace tabular
{

namespace
{

using LF::Yaml::FieldType;
using LF::Yaml::MakeContainer;
using LF::Yaml::YamlEnum;
using LF::Yaml::YamlField;
using LF::Yaml::YamlSet;

// The authored shape, read by offset; the views handed to BuildSchema point into it.
struct column_yaml
{
	std::string name;
	int32_t     type = -1;
	bool        key  = false;
	std::string input;
	bool        domain = false;   // never written: the domain is read from its node (see ReadDomain)
	bool        edge   = false;
	bool        fallthrough = false;
};

// The mapping form of a column's domain: members, or another table's column.
struct domain_yaml
{
	std::vector<std::string> members;
	std::string              table;
	std::string              column;
};

struct input_yaml
{
	std::string name;
	int32_t     type = -1;
	std::string range;
	bool        cyclic = false;
};

struct pivot_yaml
{
	std::string headers;
	std::string cells;
};

struct axis_yaml
{
	std::string name;
	int32_t     type = -1;
};

struct grid_yaml
{
	axis_yaml rows;
	axis_yaml columns;
	int32_t   cells = -1;
};

struct order_yaml
{
	std::vector<std::string> columns;
	bool                     unique = false;
};

struct table_yaml
{
	std::string              name;
	std::vector<column_yaml> columns;
	order_yaml               order_by;
	bool                     read_only = false;
	std::vector<input_yaml>  inputs;
	int32_t                  pick = 0;
	std::string              output;
	pivot_yaml               pivot;
	grid_yaml                grid;
	std::string              suffix;
	std::string              weight;
};

// A machine's state column: a hash names members and starts at one of them;
// an int or a double starts at a value.
struct state_yaml
{
	std::string              name;
	int32_t                  type = -1;
	std::vector<std::string> members;
	std::string              initial;
};

// where, given and set map names to sources or columns, and a source is a
// scalar or a {literal: ..} mapping, so the walker reads none of them; the
// clause reads its nodes itself (see ReadClause).
struct clause_yaml
{
	std::string from;
	bool        where = false;   // never written
	bool        given = false;   // never written
	bool        set   = false;   // never written
	std::string emit;
	uint32_t    overrides = UINT32_MAX;   // the clause overridden, by position (Part 4)
};

struct machine_yaml
{
	std::string              name;
	std::vector<state_yaml>  state;
	std::vector<input_yaml>  inputs;
	std::vector<clause_yaml> create;
	std::vector<clause_yaml> entry;
	std::vector<clause_yaml> step;
	std::vector<clause_yaml> exit;
};

struct document_yaml
{
	std::vector<table_yaml>   tables;
	std::vector<machine_yaml> machines;
};

// In column_type's order: the index is the enumerator.
char const* const kTypeNames[] = {"hash", "flags", "int", "double", "condition", "command", "vec2", "string", "band"};
YamlEnum const    kTypes{kTypeNames, 9};

// In pick_policy's order.
char const* const kPickNames[] = {"none", "best", "all", "or", "weighted"};
YamlEnum const    kPicks{kPickNames, 5};

YamlField const kColumnFields[] = {
	{"name", FieldType::String, offsetof(column_yaml, name), true},
	{"type", FieldType::Enum,   offsetof(column_yaml, type), true, 0, nullptr, nullptr, &kTypes},
	{"key",  FieldType::Bool,   offsetof(column_yaml, key)},
	{"input", FieldType::String, offsetof(column_yaml, input)},
	// A domain is the scalar `rows` or a mapping, and the walker has no field
	// of two kinds. A nested field with no child set accepts either and reads
	// nothing; ReadDomain reads the node itself.
	{"domain", FieldType::Nested, offsetof(column_yaml, domain)},
	{"edge", FieldType::Bool,   offsetof(column_yaml, edge)},
	{"fallthrough", FieldType::Bool, offsetof(column_yaml, fallthrough)},
};
YamlSet const kColumnSet{kColumnFields, nullptr, "column"};

YamlField const kDomainMember{"", FieldType::String, 0};
YamlField const kDomainFields[] = {
	{"members", FieldType::Sequence, offsetof(domain_yaml, members), false,
	 0, nullptr, &kDomainMember, nullptr, MakeContainer<std::vector<std::string>>()},
	{"table",   FieldType::String,   offsetof(domain_yaml, table)},
	{"column",  FieldType::String,   offsetof(domain_yaml, column)},
};
YamlSet const kDomainSet{kDomainFields, nullptr, "domain"};

YamlField const kInputFields[] = {
	{"name", FieldType::String, offsetof(input_yaml, name), true},
	{"type", FieldType::Enum,   offsetof(input_yaml, type), true, 0, nullptr, nullptr, &kTypes},
	{"range", FieldType::String, offsetof(input_yaml, range)},
	{"cyclic", FieldType::Bool, offsetof(input_yaml, cyclic)},
};
YamlSet const kInputSet{kInputFields, nullptr, "input"};

YamlField const kPivotFields[] = {
	{"headers", FieldType::String, offsetof(pivot_yaml, headers), true},
	{"cells",   FieldType::String, offsetof(pivot_yaml, cells), true},
};
YamlSet const kPivotSet{kPivotFields, nullptr, "pivot"};

YamlField const kAxisFields[] = {
	{"name", FieldType::String, offsetof(axis_yaml, name), true},
	{"type", FieldType::Enum,   offsetof(axis_yaml, type), true, 0, nullptr, nullptr, &kTypes},
};
YamlSet const kAxisSet{kAxisFields, nullptr, "axis"};

YamlField const kGridFields[] = {
	{"rows",    FieldType::Nested, offsetof(grid_yaml, rows), true, 0, &kAxisSet},
	{"columns", FieldType::Nested, offsetof(grid_yaml, columns), true, 0, &kAxisSet},
	{"cells",   FieldType::Enum,   offsetof(grid_yaml, cells), true, 0, nullptr, nullptr, &kTypes},
};
YamlSet const kGridSet{kGridFields, nullptr, "grid"};

YamlField const kOrderColumn{"", FieldType::String, 0};
YamlField const kOrderFields[] = {
	{"columns", FieldType::Sequence, offsetof(order_yaml, columns), true,
	 0, nullptr, &kOrderColumn, nullptr, MakeContainer<std::vector<std::string>>()},
	{"unique", FieldType::Bool, offsetof(order_yaml, unique)},
};
YamlSet const kOrderSet{kOrderFields, nullptr, "order_by"};

YamlField const kTableFields[] = {
	{"name",      FieldType::String,   offsetof(table_yaml, name), true},
	{"columns",   FieldType::Sequence, offsetof(table_yaml, columns), false,
	 0, &kColumnSet, nullptr, nullptr, MakeContainer<std::vector<column_yaml>>()},
	{"order_by",  FieldType::Nested,   offsetof(table_yaml, order_by), false, 0, &kOrderSet},
	{"read_only", FieldType::Bool,     offsetof(table_yaml, read_only)},
	{"inputs",    FieldType::Sequence, offsetof(table_yaml, inputs), false,
	 0, &kInputSet, nullptr, nullptr, MakeContainer<std::vector<input_yaml>>()},
	{"pick",      FieldType::Enum,     offsetof(table_yaml, pick), false, 0, nullptr, nullptr, &kPicks},
	{"output",    FieldType::String,   offsetof(table_yaml, output)},
	{"pivot",     FieldType::Nested,   offsetof(table_yaml, pivot), false, 0, &kPivotSet},
	{"grid",      FieldType::Nested,   offsetof(table_yaml, grid), false, 0, &kGridSet},
	{"suffix",    FieldType::String,   offsetof(table_yaml, suffix)},
	{"weight",    FieldType::String,   offsetof(table_yaml, weight)},
};
YamlSet const kTableSet{kTableFields, nullptr, "table"};

YamlField const kStateMember{"", FieldType::String, 0};
YamlField const kStateFields[] = {
	{"name",    FieldType::String,   offsetof(state_yaml, name), true},
	{"type",    FieldType::Enum,     offsetof(state_yaml, type), true, 0, nullptr, nullptr, &kTypes},
	{"members", FieldType::Sequence, offsetof(state_yaml, members), false,
	 0, nullptr, &kStateMember, nullptr, MakeContainer<std::vector<std::string>>()},
	{"initial", FieldType::String,   offsetof(state_yaml, initial), true},
};
YamlSet const kStateSet{kStateFields, nullptr, "state column"};

YamlField const kClauseFields[] = {
	{"from",  FieldType::String, offsetof(clause_yaml, from), true},
	{"where", FieldType::Nested, offsetof(clause_yaml, where)},
	{"given", FieldType::Nested, offsetof(clause_yaml, given)},
	{"set",   FieldType::Nested, offsetof(clause_yaml, set)},
	{"emit",  FieldType::String, offsetof(clause_yaml, emit)},
	{"overrides", FieldType::UInt32, offsetof(clause_yaml, overrides)},
};
YamlSet const kClauseSet{kClauseFields, nullptr, "clause"};

YamlField const kMachineFields[] = {
	{"name",   FieldType::String,   offsetof(machine_yaml, name), true},
	{"state",  FieldType::Sequence, offsetof(machine_yaml, state), false,
	 0, &kStateSet, nullptr, nullptr, MakeContainer<std::vector<state_yaml>>()},
	{"inputs", FieldType::Sequence, offsetof(machine_yaml, inputs), false,
	 0, &kInputSet, nullptr, nullptr, MakeContainer<std::vector<input_yaml>>()},
	{"create", FieldType::Sequence, offsetof(machine_yaml, create), false,
	 0, &kClauseSet, nullptr, nullptr, MakeContainer<std::vector<clause_yaml>>()},
	{"entry",  FieldType::Sequence, offsetof(machine_yaml, entry), false,
	 0, &kClauseSet, nullptr, nullptr, MakeContainer<std::vector<clause_yaml>>()},
	{"step",   FieldType::Sequence, offsetof(machine_yaml, step), false,
	 0, &kClauseSet, nullptr, nullptr, MakeContainer<std::vector<clause_yaml>>()},
	{"exit",   FieldType::Sequence, offsetof(machine_yaml, exit), false,
	 0, &kClauseSet, nullptr, nullptr, MakeContainer<std::vector<clause_yaml>>()},
};
YamlSet const kMachineSet{kMachineFields, nullptr, "machine"};

YamlField const kDocumentFields[] = {
	{"tables", FieldType::Sequence, offsetof(document_yaml, tables), true,
	 0, &kTableSet, nullptr, nullptr, MakeContainer<std::vector<table_yaml>>()},
	{"machines", FieldType::Sequence, offsetof(document_yaml, machines), false,
	 0, &kMachineSet, nullptr, nullptr, MakeContainer<std::vector<machine_yaml>>()},
};
YamlSet const kDocumentSet{kDocumentFields, nullptr, "table definitions"};

// A column's `domain:` as the builder takes it. Hash members are hashed as a
// CSV cell is, so a member names the same key a row's cell does; the builder
// refuses members on any key but a hash or an int, so another type's members
// are passed uninterpreted and never read.
tl::expected<key_domain, ErrorContext> ReadDomain(LF::Yaml::Node const& node, column_yaml const& column,
	domain_yaml& mapped, std::vector<cell>& members)
{
	char const* const kShapes = "; a domain is rows, {members: [..]} or {table: T, column: C}";
	if (node.is_scalar())
	{
		if (node.as_scalar().value != "rows")
		{
			return tl::make_unexpected(ErrorContext("column '", column.name, "' has domain '", node.as_scalar().value,
				"'", kShapes));
		}
		return key_domain::Rows();
	}
	if (!node.is_mapping())
	{
		return tl::make_unexpected(ErrorContext("column '", column.name, "' has a ", node.kind_name(), " for a domain",
			kShapes));
	}
	TreeErrorContext errors;
	LF::Yaml::ReadSet(&mapped, node, kDomainSet, errors);
	if (errors.has_errors())
	{
		return tl::make_unexpected(ErrorContext(errors.format()).with("the domain of column '", column.name, "'"));
	}
	bool const listed  = node.get("members") != nullptr;
	bool const foreign = node.get("table") != nullptr || node.get("column") != nullptr;
	if (listed == foreign)
	{
		return tl::make_unexpected(ErrorContext("the domain of column '", column.name,
			"' names members or a table's column, one of the two", kShapes));
	}
	if (foreign)
	{
		if (node.get("table") == nullptr || node.get("column") == nullptr)
		{
			return tl::make_unexpected(ErrorContext("the domain of column '", column.name,
				"' names a table and its column, both"));
		}
		return key_domain{domain_kind::Column, {}, mapped.table, mapped.column};
	}
	auto const type = static_cast<column_type>(column.type);
	if (type != column_type::Hash && type != column_type::Int)
	{
		members.assign(mapped.members.size(), cell::Integer(0));
		return key_domain{domain_kind::Members, members, {}, {}};
	}
	column_def const   asColumn{column.name, type};
	detail::hash_names names;
	auto const&        items = node.get("members")->as_sequence().items;
	for (size_t m = 0; m < mapped.members.size(); ++m)
	{
		// The walker skips a null item, so the nodes line up with the texts only
		// when none was skipped.
		uint32_t const line = items.size() == mapped.members.size() ? static_cast<uint32_t>(items[m]->mark.line) : 0;
		auto value = detail::ReadCell(asColumn, mapped.members[m], names, line);
		if (!value)
		{
			return tl::make_unexpected(std::move(value.error()).with("member ", m, " of the domain of column '",
				column.name, "'"));
		}
		members.push_back(*value);
	}
	return key_domain{domain_kind::Members, members, {}, {}};
}

// An input, of a table or a machine. Presence, not emptiness, decides whether
// it is ranged: `range: ""` is a mistake, not "no range".
// whose names the input's owner in a refusal: " of table 't'", or nothing.
tl::expected<add_input, ErrorContext> ReadInput(input_yaml const& i, LF::Yaml::Node const& node, std::string const& whose)
{
	add_input a{i.name, static_cast<column_type>(i.type)};
	a.cyclic = i.cyclic;
	if (node.get("range") != nullptr)
	{
		a.ranged = detail::ParseRange(i.range, a.lo, a.hi);
		if (!a.ranged)
		{
			return tl::make_unexpected(ErrorContext("input '", i.name, "'", whose, " has range '", i.range,
				"'; a range is written lo..hi"));
		}
	}
	return a;
}

tl::expected<schema, ErrorContext> BuildTables(document_yaml const& document, LF::Yaml::Node const& root,
	std::string_view label)
{
	// Which tables wrote order_by: the walker leaves an absent mapping at its default.
	LF::Yaml::Node const* tableNodes = root.get("tables");

	std::vector<std::vector<add_column>> columns(document.tables.size());
	std::vector<std::vector<add_input>>  inputs(document.tables.size());
	std::vector<std::vector<uint32_t>>   orderColumns(document.tables.size());
	std::vector<order_by>                orders(document.tables.size());
	std::vector<pivot_by>                pivots(document.tables.size());
	std::vector<grid_shape>              grids(document.tables.size());
	// Sized before any view into them is taken, so none moves.
	std::vector<std::vector<domain_yaml>>       domainTexts(document.tables.size());
	std::vector<std::vector<std::vector<cell>>> domainMembers(document.tables.size());
	std::vector<create_table>            commands;
	commands.reserve(document.tables.size());
	for (size_t t = 0; t < document.tables.size(); ++t)
	{
		table_yaml const& in = document.tables[t];
		LF::Yaml::Node const* inputNodes = tableNodes->as_sequence().items[t]->get("inputs");
		for (uint32_t ii = 0; ii < in.inputs.size(); ++ii)
		{
			auto a = ReadInput(in.inputs[ii], *inputNodes->as_sequence().items[ii], cc(std::string(" of table '"), in.name, "'"));
			if (!a)
			{
				return tl::make_unexpected(std::move(a.error()).with(std::string(label)));
			}
			inputs[t].push_back(*a);
		}
		auto inputOf = [&](std::string const& name) -> uint32_t
		{
			for (uint32_t i = 0; i < in.inputs.size(); ++i)
			{
				if (in.inputs[i].name == name)
				{
					return i;
				}
			}
			return UINT32_MAX;
		};
		auto columnOf = [&](std::string const& name) -> uint32_t
		{
			for (uint32_t c = 0; c < in.columns.size(); ++c)
			{
				if (in.columns[c].name == name)
				{
					return c;
				}
			}
			return UINT32_MAX;
		};
		// Presence, not emptiness: `input: ""` is a mistake, not "no input".
		LF::Yaml::Node const* tableNode   = tableNodes->as_sequence().items[t].get();
		LF::Yaml::Node const* columnNodes = tableNode->get("columns");
		domainTexts[t].resize(in.columns.size());
		domainMembers[t].resize(in.columns.size());
		for (uint32_t ci = 0; ci < in.columns.size(); ++ci)
		{
			column_yaml const& c     = in.columns[ci];
			uint32_t           input = UINT32_MAX;
			key_domain         domain;
			if (LF::Yaml::Node const* domainNode = columnNodes->as_sequence().items[ci]->get("domain"))
			{
				auto read = ReadDomain(*domainNode, c, domainTexts[t][ci], domainMembers[t][ci]);
				if (!read)
				{
					return tl::make_unexpected(std::move(read.error()).with("table '", in.name, "'")
						.with(std::string(label)));
				}
				domain = *read;
			}
			// A fall-through key's domain is open, so it declares no other:
			// the declaration is the domain (the builder's FallThrough kind).
			if (c.fallthrough)
			{
				if (columnNodes->as_sequence().items[ci]->get("domain") != nullptr)
				{
					return tl::make_unexpected(ErrorContext("column '", c.name, "' of table '", in.name,
						"' falls through and declares a domain; a fall-through key is open, any value may come")
						.with(std::string(label)));
				}
				domain = key_domain::FallThrough();
			}
			if (columnNodes->as_sequence().items[ci]->get("input") != nullptr)
			{
				input = inputOf(c.input);
				if (input == UINT32_MAX)
				{
					return tl::make_unexpected(ErrorContext("column '", c.name, "' of table '", in.name,
						"' reads input '", c.input, "', which the table does not declare").with(std::string(label)));
				}
			}
			columns[t].push_back(add_column{c.name, static_cast<column_type>(c.type), c.key, input, domain, c.edge});
		}
		uint32_t output = UINT32_MAX;
		if (tableNode->get("output") != nullptr)
		{
			output = columnOf(in.output);
			if (output == UINT32_MAX)
			{
				return tl::make_unexpected(ErrorContext("table '", in.name, "' names output '", in.output,
					"', which the table does not have").with(std::string(label)));
			}
		}
		uint32_t weight = UINT32_MAX;
		if (tableNode->get("weight") != nullptr)
		{
			weight = columnOf(in.weight);
			if (weight == UINT32_MAX)
			{
				return tl::make_unexpected(ErrorContext("table '", in.name, "' names weight '", in.weight,
					"', which the table does not have").with(std::string(label)));
			}
		}
		// Presence, not emptiness: `suffix: ""` names no column, and the builder
		// could not tell it from no suffix at all.
		if (tableNode->get("suffix") != nullptr && in.suffix.empty())
		{
			return tl::make_unexpected(ErrorContext("table '", in.name, "' has a suffix with no name; ",
				"the suffix is named after the command column it adds").with(std::string(label)));
		}
		bool ordered = tableNodes->as_sequence().items[t]->get("order_by") != nullptr;
		if (ordered)
		{
			for (std::string const& name : in.order_by.columns)
			{
				uint32_t const found = columnOf(name);
				if (found == UINT32_MAX)
				{
					return tl::make_unexpected(ErrorContext("the order of table '", in.name, "' names column '", name,
						"', which the table does not have").with(std::string(label)));
				}
				orderColumns[t].push_back(found);
			}
			orders[t] = order_by{orderColumns[t], in.order_by.unique};
		}
		bool const pivoted = tableNodes->as_sequence().items[t]->get("pivot") != nullptr;
		if (pivoted)
		{
			pivots[t] = pivot_by{columnOf(in.pivot.headers), columnOf(in.pivot.cells)};
			if (pivots[t].headers == UINT32_MAX || pivots[t].cells == UINT32_MAX)
			{
				return tl::make_unexpected(ErrorContext("the pivot of table '", in.name, "' names a column the table does not have")
					.with(std::string(label)));
			}
		}
		bool const isGrid = tableNodes->as_sequence().items[t]->get("grid") != nullptr;
		if (isGrid)
		{
			grids[t] = grid_shape{{in.grid.rows.name, static_cast<column_type>(in.grid.rows.type)},
			                      {in.grid.columns.name, static_cast<column_type>(in.grid.columns.type)},
			                      static_cast<column_type>(in.grid.cells)};
		}
		// A grid and a picked table are always read-only, so leaving the key
		// out is not a refusal; stating false is.
		bool const alwaysReadOnly = isGrid || static_cast<pick_policy>(in.pick) != pick_policy::None;
		bool const readOnly = alwaysReadOnly && tableNode->get("read_only") == nullptr ? true : in.read_only;
		commands.push_back(create_table{in.name, columns[t], ordered ? &orders[t] : nullptr, readOnly,
			inputs[t], static_cast<pick_policy>(in.pick), output, pivoted ? &pivots[t] : nullptr,
			isGrid ? &grids[t] : nullptr, in.suffix, weight});
	}

	auto built = BuildSchema(commands);
	if (!built)
	{
		return tl::make_unexpected(std::move(built.error()).with(std::string(label)));
	}
	return std::move(*built);
}

}

// Everything a document's machine commands view: the parsed tree (where, given
// and set are read from its nodes), the walked document, and per machine the
// command arrays. Sized before any view into it is taken, so nothing moves.
struct machine_storage
{
	std::vector<std::vector<cell>>                                 members;   // per state column
	std::vector<add_state>                                         state;
	std::vector<add_input>                                         inputs;
	std::array<std::vector<std::vector<clause_bind>>, kTriggerCount> where;   // per clause
	std::array<std::vector<std::vector<clause_bind>>, kTriggerCount> given;
	std::array<std::vector<std::vector<set_state>>, kTriggerCount>   sets;
	std::array<std::vector<add_clause>, kTriggerCount>             clauses;
};

struct DocumentStorage
{
	LF::Yaml::NodePtr            root;
	document_yaml                document;
	std::vector<machine_storage> machines;
	std::vector<create_machine>  commands;
};

namespace
{

char const* const kListKeys[kTriggerCount] = {"create", "entry", "step", "exit"};

std::vector<clause_yaml> const& ListOf(machine_yaml const& m, size_t list)
{
	switch (static_cast<trigger>(list))
	{
	case trigger::Create: return m.create;
	case trigger::Entry:  return m.entry;
	case trigger::Step:   return m.step;
	case trigger::Exit:   return m.exit;
	}
	return m.step;
}

// A clause's where or given target, as a cell type, for reading a literal: a
// key column's or a table input's. Unknown names read as an int; CheckMachine
// refuses them by name afterwards, so this never has to.
column_type TargetType(schema const& tables, std::string_view table, std::string_view target, bool isKey)
{
	auto t = tables.FindTable(table);
	if (!t)
	{
		return column_type::Int;
	}
	table_def const& def = tables.tables()[*t];
	if (isKey)
	{
		auto column = tables.FindColumn(*t, target);
		return column ? def.columns[*column].type : column_type::Int;
	}
	for (input_def const& input : def.inputs)
	{
		if (input.name == target)
		{
			return input.type;
		}
	}
	return column_type::Int;
}

constexpr char kSourceShapes[] = "; a source is state.<column>, input.<name>, clause.<n>.<column> or {literal: <text>}";

tl::expected<source, ErrorContext> ReadSource(LF::Yaml::Node const& node, std::string_view target, column_type type,
	detail::hash_names& names)
{
	if (node.is_mapping())
	{
		LF::Yaml::Node const* literal = node.get("literal");
		if (literal == nullptr || node.as_mapping().entries.size() != 1)
		{
			return tl::make_unexpected(ErrorContext("'", target, "' has a mapping for a source", kSourceShapes));
		}
		if (!literal->is_scalar())
		{
			return tl::make_unexpected(ErrorContext("the literal for '", target, "' is a ", literal->kind_name(),
				"; a literal is a scalar"));
		}
		auto value = detail::ReadCell(column_def{std::string(target), type}, literal->as_scalar().value, names,
			static_cast<uint32_t>(literal->mark.line));
		if (!value)
		{
			return tl::make_unexpected(std::move(value.error()).with("the literal for '", target, "'"));
		}
		source out;
		out.literal = *value;
		return out;
	}
	if (!node.is_scalar())
	{
		return tl::make_unexpected(ErrorContext("'", target, "' has a ", node.kind_name(), " for a source", kSourceShapes));
	}
	std::string_view const text = node.as_scalar().value;
	source                 out;
	if (text.starts_with("state."))
	{
		out.kind = source_kind::State;
		out.name = text.substr(6);
	}
	else if (text.starts_with("input."))
	{
		out.kind = source_kind::Input;
		out.name = text.substr(6);
	}
	else if (text.starts_with("clause."))
	{
		std::string_view const rest = text.substr(7);
		size_t const           dot  = rest.find('.');
		uint32_t               index = 0;
		if (dot == std::string_view::npos || !detail::ParseWhole(rest.substr(0, dot), index))
		{
			return tl::make_unexpected(ErrorContext("'", target, "' reads '", text,
				"'; an earlier clause is named clause.<n>.<column>, n its position in the list"));
		}
		out.kind   = source_kind::Clause;
		out.clause = index;
		out.name   = rest.substr(dot + 1);
	}
	else
	{
		return tl::make_unexpected(ErrorContext("'", target, "' reads '", text, "'", kSourceShapes));
	}
	if (out.name.empty())
	{
		return tl::make_unexpected(ErrorContext("'", target, "' reads '", text, "', which names nothing"));
	}
	return out;
}

// A where or given mapping, into binds whose views point into the tree.
tl::expected<void, ErrorContext> ReadBinds(LF::Yaml::Node const* node, char const* key, schema const& tables,
	std::string_view table, detail::hash_names& names, std::vector<clause_bind>& out)
{
	if (node == nullptr)
	{
		return {};
	}
	if (!node->is_mapping())
	{
		return tl::make_unexpected(ErrorContext(key, " is a ", node->kind_name(), "; it maps names to sources"));
	}
	for (auto const& [name, value] : node->as_mapping().entries)
	{
		if (!value)
		{
			return tl::make_unexpected(ErrorContext(key, " gives '", name, "' no source"));
		}
		auto from = ReadSource(*value, name, TargetType(tables, table, name, std::string_view(key) == "where"), names);
		if (!from)
		{
			return tl::make_unexpected(std::move(from.error()).with(key));
		}
		out.push_back(clause_bind{name, *from});
	}
	return {};
}

tl::expected<void, ErrorContext> ReadSets(LF::Yaml::Node const* node, std::vector<set_state>& out)
{
	if (node == nullptr)
	{
		return {};
	}
	if (!node->is_mapping())
	{
		return tl::make_unexpected(ErrorContext("set is a ", node->kind_name(), "; it maps state columns to value columns"));
	}
	for (auto const& [state, value] : node->as_mapping().entries)
	{
		if (!value || !value->is_scalar())
		{
			return tl::make_unexpected(ErrorContext("set writes '", state, "' from something that is not a column name"));
		}
		out.push_back(set_state{state, value->as_scalar().value});
	}
	return {};
}

tl::expected<void, ErrorContext> ReadMachine(machine_yaml const& in, LF::Yaml::Node const& node, schema const& tables,
	machine_storage& store, create_machine& out)
{
	detail::hash_names names;
	store.members.resize(in.state.size());
	LF::Yaml::Node const* stateNodes = node.get("state");
	for (size_t s = 0; s < in.state.size(); ++s)
	{
		state_yaml const&     st     = in.state[s];
		auto const            type   = static_cast<column_type>(st.type);
		column_def const      column{st.name, type};
		LF::Yaml::Node const& stNode = *stateNodes->as_sequence().items[s];
		if (stNode.get("members") != nullptr)
		{
			auto const& items = stNode.get("members")->as_sequence().items;
			for (size_t m = 0; m < st.members.size(); ++m)
			{
				// The walker skips a null item, so the nodes line up with the texts
				// only when none was skipped.
				uint32_t const line = items.size() == st.members.size() ? static_cast<uint32_t>(items[m]->mark.line) : 0;
				auto value = detail::ReadCell(column, st.members[m], names, line);
				if (!value)
				{
					return tl::make_unexpected(std::move(value.error()).with("member ", m, " of state column '", st.name,
						"'").with("machine '", in.name, "'"));
				}
				store.members[s].push_back(*value);
			}
		}
		auto initial = detail::ReadCell(column, st.initial, names, static_cast<uint32_t>(stNode.get("initial")->mark.line));
		if (!initial)
		{
			return tl::make_unexpected(std::move(initial.error()).with("the initial value of state column '", st.name,
				"'").with("machine '", in.name, "'"));
		}
		store.state.push_back(add_state{st.name, type, store.members[s], *initial});
	}

	LF::Yaml::Node const* inputNodes = node.get("inputs");
	for (size_t i = 0; i < in.inputs.size(); ++i)
	{
		auto a = ReadInput(in.inputs[i], *inputNodes->as_sequence().items[i], std::string());
		if (!a)
		{
			return tl::make_unexpected(std::move(a.error()).with("machine '", in.name, "'"));
		}
		store.inputs.push_back(*a);
	}

	for (size_t list = 0; list < kTriggerCount; ++list)
	{
		std::vector<clause_yaml> const& clauses = ListOf(in, list);
		LF::Yaml::Node const*           nodes   = node.get(kListKeys[list]);
		store.where[list].resize(clauses.size());
		store.given[list].resize(clauses.size());
		store.sets[list].resize(clauses.size());
		for (size_t c = 0; c < clauses.size(); ++c)
		{
			clause_yaml const&    clause     = clauses[c];
			LF::Yaml::Node const& clauseNode = *nodes->as_sequence().items[c];
			auto here = [&]() { return detail::ClausePlace(in.name, list, c, clause.from); };
			auto where = ReadBinds(clauseNode.get("where"), "where", tables, clause.from, names, store.where[list][c]);
			if (!where)
			{
				return tl::make_unexpected(std::move(where.error()).with(here()));
			}
			auto given = ReadBinds(clauseNode.get("given"), "given", tables, clause.from, names, store.given[list][c]);
			if (!given)
			{
				return tl::make_unexpected(std::move(given.error()).with(here()));
			}
			auto sets = ReadSets(clauseNode.get("set"), store.sets[list][c]);
			if (!sets)
			{
				return tl::make_unexpected(std::move(sets.error()).with(here()));
			}
			// Presence, not emptiness: `emit: ""` names no column, and the
			// command could not tell it from no emit at all.
			if (clauseNode.get("emit") != nullptr && clause.emit.empty())
			{
				return tl::make_unexpected(ErrorContext("the clause emits a column with no name").with(here()));
			}
			store.clauses[list].push_back(add_clause{clause.from, store.where[list][c], store.given[list][c],
				store.sets[list][c], clause.emit, clause.overrides});
		}
	}
	out = create_machine{in.name, store.state, store.inputs, store.clauses[0], store.clauses[1], store.clauses[2],
		store.clauses[3]};
	return {};
}

}

schema_document::schema_document() = default;
schema_document::~schema_document() = default;
schema_document::schema_document(schema_document&&) noexcept = default;
schema_document& schema_document::operator=(schema_document&&) noexcept = default;

std::span<create_machine const> schema_document::machines() const
{
	if (!storage)
	{
		return {};
	}
	return storage->commands;
}

tl::expected<schema_document, ErrorContext> DocumentFromYaml(std::string_view text, std::string_view label)
{
	auto             held = std::make_unique<DocumentStorage>();
	TreeErrorContext errors;
	held->root = LF::Yaml::ParseTree(text, errors, label);
	if (held->root)
	{
		LF::Yaml::ReadSet(&held->document, *held->root, kDocumentSet, errors);
	}
	// Any diagnostic fails the load, an unknown key included: a misspelled
	// "uniqe" silently ignored is a table that is not unique.
	if (errors.has_errors() || !held->root)
	{
		return tl::make_unexpected(ErrorContext(errors.format()).with(std::string(label)));
	}
	auto tables = BuildTables(held->document, *held->root, label);
	if (!tables)
	{
		return tl::make_unexpected(std::move(tables.error()));
	}

	std::vector<machine_yaml> const& machines = held->document.machines;
	held->machines.resize(machines.size());
	held->commands.resize(machines.size());
	LF::Yaml::Node const* machineNodes = held->root->get("machines");
	for (size_t m = 0; m < machines.size(); ++m)
	{
		auto read = ReadMachine(machines[m], *machineNodes->as_sequence().items[m], *tables, held->machines[m],
			held->commands[m]);
		if (!read)
		{
			return tl::make_unexpected(std::move(read.error()).with(std::string(label)));
		}
		auto checked = CheckMachine(held->commands[m], *tables);
		if (!checked)
		{
			return tl::make_unexpected(std::move(checked.error()).with(std::string(label)));
		}
	}

	schema_document document;
	document.tables  = std::move(*tables);
	document.storage = std::move(held);
	return document;
}

tl::expected<schema, ErrorContext> SchemaFromYaml(std::string_view text, std::string_view label)
{
	auto document = DocumentFromYaml(text, label);
	if (!document)
	{
		return tl::make_unexpected(std::move(document.error()));
	}
	if (!document->machines().empty())
	{
		return tl::make_unexpected(ErrorContext("the document defines machines, which a schema alone would drop; ",
			"read it with DocumentFromYaml").with(std::string(label)));
	}
	return std::move(document->tables);
}

}
