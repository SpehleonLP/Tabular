#include <gtest/gtest.h>
#include "tabular/relation.h"

#include <string>
#include <string_view>
#include <vector>

using namespace tabular;

// The message mentions the text: errors must name what they refuse.
static bool Mentions(std::string const& message, std::string_view text)
{
	return message.find(text) != std::string::npos;
}

namespace
{

std::vector<schema_command> SpawnPointsSchema()
{
	return {
		create_table{"spawn_points"},
		add_column{0, "package", column_type::Hash,   true},
		add_column{0, "section", column_type::Hash,   true},
		add_column{0, "x",       column_type::Double, false},
		add_column{0, "y",       column_type::Double, false},
	};
}

}

TEST(RelationSchema, EmptyCommandListBuildsNoTables)
{
	auto s = BuildSchema({});
	ASSERT_TRUE(s.has_value()) << s.error().message;
	EXPECT_TRUE(s->tables().empty());
}

TEST(RelationSchema, CommandsBuildTablesAndColumnsInOrder)
{
	auto commands = SpawnPointsSchema();
	commands.push_back(create_table{"second"});
	commands.push_back(add_column{1, "n", column_type::Int, false});

	auto s = BuildSchema(commands);
	ASSERT_TRUE(s.has_value()) << s.error().message;
	ASSERT_EQ(s->tables().size(), 2u);

	table_def const& t = s->tables()[0];
	EXPECT_EQ(t.name, "spawn_points");
	ASSERT_EQ(t.columns.size(), 4u);
	EXPECT_EQ(t.columns[0].name, "package");
	EXPECT_EQ(t.columns[0].type, column_type::Hash);
	EXPECT_TRUE(t.columns[0].key);
	EXPECT_EQ(t.columns[3].name, "y");
	EXPECT_EQ(t.columns[3].type, column_type::Double);
	EXPECT_FALSE(t.columns[3].key);

	EXPECT_EQ(s->tables()[1].name, "second");
}

TEST(RelationSchema, FindTableAndColumnByName)
{
	auto s = BuildSchema(SpawnPointsSchema());
	ASSERT_TRUE(s.has_value()) << s.error().message;

	auto table = s->FindTable("spawn_points");
	ASSERT_TRUE(table.has_value());
	EXPECT_EQ(*table, 0u);

	auto column = s->FindColumn(*table, "y");
	ASSERT_TRUE(column.has_value());
	EXPECT_EQ(*column, 3u);

	auto missingTable = s->FindTable("nope");
	ASSERT_FALSE(missingTable.has_value());
	EXPECT_TRUE(Mentions(missingTable.error().message, "nope")) << missingTable.error().message;

	auto missingColumn = s->FindColumn(*table, "z");
	ASSERT_FALSE(missingColumn.has_value());
	EXPECT_TRUE(Mentions(missingColumn.error().message, "z")) << missingColumn.error().message;

	EXPECT_FALSE(s->FindColumn(7, "x").has_value());
}

TEST(RelationSchema, DuplicateTableNameIsRefused)
{
	std::vector<schema_command> commands = {
		create_table{"a"}, add_column{0, "n", column_type::Int, false},
		create_table{"a"}, add_column{1, "n", column_type::Int, false},
	};
	auto s = BuildSchema(commands);
	ASSERT_FALSE(s.has_value());
	EXPECT_TRUE(Mentions(s.error().message, "'a'")) << s.error().message;
}

TEST(RelationSchema, DuplicateColumnNameIsRefused)
{
	std::vector<schema_command> commands = {
		create_table{"a"},
		add_column{0, "n", column_type::Int, false},
		add_column{0, "n", column_type::Double, false},
	};
	auto s = BuildSchema(commands);
	ASSERT_FALSE(s.has_value());
	EXPECT_TRUE(Mentions(s.error().message, "'n'")) << s.error().message;
}

TEST(RelationSchema, ColumnNamingAnUncreatedTableIsRefused)
{
	std::vector<schema_command> commands = {
		create_table{"a"},
		add_column{1, "n", column_type::Int, false},
	};
	auto s = BuildSchema(commands);
	ASSERT_FALSE(s.has_value());
	EXPECT_TRUE(Mentions(s.error().message, "table 1")) << s.error().message;
}

TEST(RelationSchema, EmptyNamesAreRefused)
{
	std::vector<schema_command> noTableName = { create_table{""} };
	EXPECT_FALSE(BuildSchema(noTableName).has_value());

	std::vector<schema_command> noColumnName = {
		create_table{"a"}, add_column{0, "", column_type::Int, false},
	};
	EXPECT_FALSE(BuildSchema(noColumnName).has_value());
}

TEST(RelationSchema, TableWithoutColumnsIsRefused)
{
	std::vector<schema_command> commands = { create_table{"bare"} };
	auto s = BuildSchema(commands);
	ASSERT_FALSE(s.has_value());
	EXPECT_TRUE(Mentions(s.error().message, "'bare'")) << s.error().message;
}

TEST(RelationSchema, KeyOnDoubleIsRefused)
{
	std::vector<schema_command> commands = {
		create_table{"a"}, add_column{0, "x", column_type::Double, true},
	};
	auto s = BuildSchema(commands);
	ASSERT_FALSE(s.has_value());
	EXPECT_TRUE(Mentions(s.error().message, "'x'")) << s.error().message;
}

TEST(RelationSchema, OrderIsRecorded)
{
	auto commands = SpawnPointsSchema();
	commands.push_back(order_by{0, {1, 0}, true});
	auto s = BuildSchema(commands);
	ASSERT_TRUE(s.has_value()) << s.error().message;
	EXPECT_EQ(s->tables()[0].order, (std::vector<uint32_t>{1, 0}));
	EXPECT_TRUE(s->tables()[0].unique);
}

TEST(RelationSchema, TableWithoutOrderKeepsInsertionOrder)
{
	auto s = BuildSchema(SpawnPointsSchema());
	ASSERT_TRUE(s.has_value()) << s.error().message;
	EXPECT_TRUE(s->tables()[0].order.empty());
	EXPECT_FALSE(s->tables()[0].unique);
}

TEST(RelationSchema, BadOrdersAreRefused)
{
	auto with = [](order_by o)
	{
		auto commands = SpawnPointsSchema();
		commands.push_back(std::move(o));
		return BuildSchema(commands);
	};

	auto unknownTable = with(order_by{4, {0}, false});
	ASSERT_FALSE(unknownTable.has_value());
	EXPECT_TRUE(Mentions(unknownTable.error().message, "table 4")) << unknownTable.error().message;

	auto noColumns = with(order_by{0, {}, false});
	ASSERT_FALSE(noColumns.has_value());
	EXPECT_TRUE(Mentions(noColumns.error().message, "spawn_points")) << noColumns.error().message;

	auto missingColumn = with(order_by{0, {9}, false});
	ASSERT_FALSE(missingColumn.has_value());
	EXPECT_TRUE(Mentions(missingColumn.error().message, "9")) << missingColumn.error().message;

	auto repeated = with(order_by{0, {1, 1}, false});
	ASSERT_FALSE(repeated.has_value());
	EXPECT_TRUE(Mentions(repeated.error().message, "'section'")) << repeated.error().message;

	auto onDouble = with(order_by{0, {2}, false});
	ASSERT_FALSE(onDouble.has_value());
	EXPECT_TRUE(Mentions(onDouble.error().message, "'x'")) << onDouble.error().message;
}

TEST(RelationSchema, SecondOrderIsRefused)
{
	auto commands = SpawnPointsSchema();
	commands.push_back(order_by{0, {0}, false});
	commands.push_back(order_by{0, {1}, true});
	auto s = BuildSchema(commands);
	ASSERT_FALSE(s.has_value());
	EXPECT_TRUE(Mentions(s.error().message, "already has an order")) << s.error().message;
}

TEST(RelationSchema, ErrorNamesTheFailingCommand)
{
	std::vector<schema_command> commands = {
		create_table{"a"},
		add_column{0, "n", column_type::Int, false},
		add_column{0, "n", column_type::Int, false},
	};
	auto s = BuildSchema(commands);
	ASSERT_FALSE(s.has_value());
	EXPECT_TRUE(Mentions(s.error().message, "command 2")) << s.error().message;
}
