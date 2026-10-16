#include "tabular/relation.h"

#include "Support/strings.h"

#include <utility>

namespace tabular
{

namespace
{

tl::expected<void, ErrorContext> ApplySchemaCommand(std::vector<table_def>& tables, create_table const& c)
{
	if (c.name.empty())
	{
		return tl::make_unexpected(ErrorContext("a table needs a name"));
	}
	for (table_def const& t : tables)
	{
		if (t.name == c.name)
		{
			return tl::make_unexpected(ErrorContext("table '", c.name, "' is defined twice"));
		}
	}
	tables.push_back(table_def{c.name, {}, {}, false});
	return {};
}

tl::expected<void, ErrorContext> ApplySchemaCommand(std::vector<table_def>& tables, add_column const& c)
{
	if (c.table >= tables.size())
	{
		return tl::make_unexpected(ErrorContext("column '", c.name, "' names table ", c.table,
			", but only ", tables.size(), " tables exist so far"));
	}
	table_def& t = tables[c.table];
	if (c.name.empty())
	{
		return tl::make_unexpected(ErrorContext("a column of table '", t.name, "' needs a name"));
	}
	for (column_def const& existing : t.columns)
	{
		if (existing.name == c.name)
		{
			return tl::make_unexpected(ErrorContext("table '", t.name, "' has column '", c.name, "' twice"));
		}
	}
	// Equality on doubles fails on NaN and on -0.0 versus 0.0, so a key never is one.
	if (c.key && c.type == column_type::Double)
	{
		return tl::make_unexpected(ErrorContext("column '", c.name, "' of table '", t.name,
			"' is a double and cannot be a key"));
	}
	t.columns.push_back(column_def{c.name, c.type, c.key});
	return {};
}

tl::expected<void, ErrorContext> ApplySchemaCommand(std::vector<table_def>& tables, order_by const& c)
{
	if (c.table >= tables.size())
	{
		return tl::make_unexpected(ErrorContext("an order names table ", c.table,
			", but only ", tables.size(), " tables exist so far"));
	}
	table_def& t = tables[c.table];
	if (!t.order.empty())
	{
		return tl::make_unexpected(ErrorContext("table '", t.name, "' already has an order"));
	}
	if (c.columns.empty())
	{
		return tl::make_unexpected(ErrorContext("the order of table '", t.name, "' names no columns"));
	}
	for (size_t k = 0; k < c.columns.size(); ++k)
	{
		uint32_t column = c.columns[k];
		if (column >= t.columns.size())
		{
			return tl::make_unexpected(ErrorContext("the order of table '", t.name, "' names column ", column,
				", but the table has ", t.columns.size()));
		}
		for (size_t earlier = 0; earlier < k; ++earlier)
		{
			if (c.columns[earlier] == column)
			{
				return tl::make_unexpected(ErrorContext("the order of table '", t.name, "' names column '",
					t.columns[column].name, "' twice"));
			}
		}
		// Same reason as keys: equality on doubles is not an identity.
		if (t.columns[column].type == column_type::Double)
		{
			return tl::make_unexpected(ErrorContext("column '", t.columns[column].name, "' of table '", t.name,
				"' is a double and cannot be in an order"));
		}
	}
	t.order  = c.columns;
	t.unique = c.unique;
	return {};
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

tl::expected<schema, ErrorContext> BuildSchema(std::span<schema_command const> commands)
{
	std::vector<table_def> tables;
	for (size_t i = 0; i < commands.size(); ++i)
	{
		auto applied = std::visit([&](auto const& c) { return ApplySchemaCommand(tables, c); }, commands[i]);
		if (!applied)
		{
			return tl::make_unexpected(ErrorContext("schema command ", i, ": ", applied.error().message));
		}
	}
	for (table_def const& t : tables)
	{
		if (t.columns.empty())
		{
			return tl::make_unexpected(ErrorContext("table '", t.name, "' has no columns"));
		}
	}
	return schema(std::move(tables));
}

}
