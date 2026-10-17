#include "tabular/schema_yaml.h"
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
};

struct input_yaml
{
	std::string name;
	int32_t     type = -1;
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
};

struct document_yaml
{
	std::vector<table_yaml> tables;
};

// In column_type's order: the index is the enumerator.
char const* const kTypeNames[] = {"hash", "flags", "int", "double", "condition", "command", "vec2", "string", "band"};
YamlEnum const    kTypes{kTypeNames, 9};

// In pick_policy's order.
char const* const kPickNames[] = {"none", "best", "all", "or"};
YamlEnum const    kPicks{kPickNames, 4};

YamlField const kColumnFields[] = {
	{"name", FieldType::String, offsetof(column_yaml, name), true},
	{"type", FieldType::Enum,   offsetof(column_yaml, type), true, 0, nullptr, nullptr, &kTypes},
	{"key",  FieldType::Bool,   offsetof(column_yaml, key)},
	{"input", FieldType::String, offsetof(column_yaml, input)},
};
YamlSet const kColumnSet{kColumnFields, nullptr, "column"};

YamlField const kInputFields[] = {
	{"name", FieldType::String, offsetof(input_yaml, name), true},
	{"type", FieldType::Enum,   offsetof(input_yaml, type), true, 0, nullptr, nullptr, &kTypes},
};
YamlSet const kInputSet{kInputFields, nullptr, "input"};

YamlField const kOrderColumn{"", FieldType::String, 0};
YamlField const kOrderFields[] = {
	{"columns", FieldType::Sequence, offsetof(order_yaml, columns), true,
	 0, nullptr, &kOrderColumn, nullptr, MakeContainer<std::vector<std::string>>()},
	{"unique", FieldType::Bool, offsetof(order_yaml, unique)},
};
YamlSet const kOrderSet{kOrderFields, nullptr, "order_by"};

YamlField const kTableFields[] = {
	{"name",      FieldType::String,   offsetof(table_yaml, name), true},
	{"columns",   FieldType::Sequence, offsetof(table_yaml, columns), true,
	 0, &kColumnSet, nullptr, nullptr, MakeContainer<std::vector<column_yaml>>()},
	{"order_by",  FieldType::Nested,   offsetof(table_yaml, order_by), false, 0, &kOrderSet},
	{"read_only", FieldType::Bool,     offsetof(table_yaml, read_only)},
	{"inputs",    FieldType::Sequence, offsetof(table_yaml, inputs), false,
	 0, &kInputSet, nullptr, nullptr, MakeContainer<std::vector<input_yaml>>()},
	{"pick",      FieldType::Enum,     offsetof(table_yaml, pick), false, 0, nullptr, nullptr, &kPicks},
	{"output",    FieldType::String,   offsetof(table_yaml, output)},
};
YamlSet const kTableSet{kTableFields, nullptr, "table"};

YamlField const kDocumentFields[] = {
	{"tables", FieldType::Sequence, offsetof(document_yaml, tables), true,
	 0, &kTableSet, nullptr, nullptr, MakeContainer<std::vector<table_yaml>>()},
};
YamlSet const kDocumentSet{kDocumentFields, nullptr, "table definitions"};

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
	std::vector<create_table>            commands;
	commands.reserve(document.tables.size());
	for (size_t t = 0; t < document.tables.size(); ++t)
	{
		table_yaml const& in = document.tables[t];
		for (input_yaml const& i : in.inputs)
		{
			inputs[t].push_back(add_input{i.name, static_cast<column_type>(i.type)});
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
		for (column_yaml const& c : in.columns)
		{
			uint32_t input = UINT32_MAX;
			if (!c.input.empty())
			{
				input = inputOf(c.input);
				if (input == UINT32_MAX)
				{
					return tl::make_unexpected(ErrorContext("column '", c.name, "' of table '", in.name,
						"' reads input '", c.input, "', which the table does not declare").with(std::string(label)));
				}
			}
			columns[t].push_back(add_column{c.name, static_cast<column_type>(c.type), c.key, input});
		}
		uint32_t output = UINT32_MAX;
		if (!in.output.empty())
		{
			output = columnOf(in.output);
			if (output == UINT32_MAX)
			{
				return tl::make_unexpected(ErrorContext("table '", in.name, "' names output '", in.output,
					"', which the table does not have").with(std::string(label)));
			}
		}
		bool ordered = tableNodes->as_sequence().items[t]->get("order_by") != nullptr;
		if (ordered)
		{
			for (std::string const& name : in.order_by.columns)
			{
				uint32_t found = UINT32_MAX;
				for (uint32_t c = 0; c < in.columns.size(); ++c)
				{
					if (in.columns[c].name == name)
					{
						found = c;
					}
				}
				if (found == UINT32_MAX)
				{
					return tl::make_unexpected(ErrorContext("the order of table '", in.name, "' names column '", name,
						"', which the table does not have").with(std::string(label)));
				}
				orderColumns[t].push_back(found);
			}
			orders[t] = order_by{orderColumns[t], in.order_by.unique};
		}
		commands.push_back(create_table{in.name, columns[t], ordered ? &orders[t] : nullptr, in.read_only,
			inputs[t], static_cast<pick_policy>(in.pick), output});
	}

	auto built = BuildSchema(commands);
	if (!built)
	{
		return tl::make_unexpected(std::move(built.error()).with(std::string(label)));
	}
	return std::move(*built);
}

}
