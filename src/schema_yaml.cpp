#include "tabular/schema_yaml.h"
#include "csv_cells.h"
#include "Yaml/yaml_markup.h"
#include "Yaml/yaml_parse.h"

#include <cstddef>
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

struct document_yaml
{
	std::vector<table_yaml> tables;
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

YamlField const kDocumentFields[] = {
	{"tables", FieldType::Sequence, offsetof(document_yaml, tables), true,
	 0, &kTableSet, nullptr, nullptr, MakeContainer<std::vector<table_yaml>>()},
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

}

tl::expected<schema, ErrorContext> SchemaFromYaml(std::string_view text, std::string_view label)
{
	TreeErrorContext errors;
	LF::Yaml::NodePtr root = LF::Yaml::ParseTree(text, errors, label);
	document_yaml     document;
	if (root)
	{
		LF::Yaml::ReadSet(&document, *root, kDocumentSet, errors);
	}
	// Any diagnostic fails the load, an unknown key included: a misspelled
	// "uniqe" silently ignored is a table that is not unique.
	if (errors.has_errors() || !root)
	{
		return tl::make_unexpected(ErrorContext(errors.format()).with(std::string(label)));
	}

	// Which tables wrote order_by: the walker leaves an absent mapping at its default.
	LF::Yaml::Node const* tableNodes = root->get("tables");

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
			input_yaml const& i = in.inputs[ii];
			add_input         a{i.name, static_cast<column_type>(i.type)};
			// Presence, not emptiness, as with input: `range: ""` is a mistake.
			if (inputNodes->as_sequence().items[ii]->get("range") != nullptr)
			{
				a.ranged = detail::ParseRange(i.range, a.lo, a.hi);
				if (!a.ranged)
				{
					return tl::make_unexpected(ErrorContext("input '", i.name, "' of table '", in.name, "' has range '",
						i.range, "'; a range is written lo..hi").with(std::string(label)));
				}
			}
			inputs[t].push_back(a);
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
			if (columnNodes->as_sequence().items[ci]->get("input") != nullptr)
			{
				input = inputOf(c.input);
				if (input == UINT32_MAX)
				{
					return tl::make_unexpected(ErrorContext("column '", c.name, "' of table '", in.name,
						"' reads input '", c.input, "', which the table does not declare").with(std::string(label)));
				}
			}
			columns[t].push_back(add_column{c.name, static_cast<column_type>(c.type), c.key, input, domain});
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
