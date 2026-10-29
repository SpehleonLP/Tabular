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

add_column const kSpawnColumns[] = {
	{"package", column_type::Hash,   true},
	{"section", column_type::Hash,   true},
	{"x",       column_type::Double, false},
	{"y",       column_type::Double, false},
};

tl::expected<schema, ErrorContext> BuildOne(create_table const& table)
{
	return BuildSchema(std::span<create_table const>(&table, 1));
}

tl::expected<schema, ErrorContext> SpawnPointsWith(order_by const& order)
{
	return BuildOne(create_table{"spawn_points", kSpawnColumns, &order});
}

}

TEST(RelationSchema, EmptyListBuildsNoTables)
{
	auto s = BuildSchema({});
	ASSERT_TRUE(s.has_value()) << s.error().message;
	EXPECT_TRUE(s->tables().empty());
}

TEST(RelationSchema, TablesAndColumnsKeepTheirOrder)
{
	add_column const rules[] = {
		{"when", column_type::Condition, false},
		{"then", column_type::Command,   false},
	};
	create_table const tables[] = {
		{"spawn_points", kSpawnColumns},
		{"rules", rules},
	};
	auto s = BuildSchema(tables);
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

	table_def const& r = s->tables()[1];
	EXPECT_EQ(r.name, "rules");
	EXPECT_EQ(r.columns[0].type, column_type::Condition);
	EXPECT_EQ(r.columns[1].type, column_type::Command);
}

TEST(RelationSchema, CommandViewsNeedNotOutliveTheBuild)
{
	tl::expected<schema, ErrorContext> s;
	{
		std::string name   = "temporary";
		std::string column = "n";
		add_column const columns[] = {{column, column_type::Int, false}};
		s = BuildOne(create_table{name, columns});
		name.assign("overwritten");
		column.assign("z");
	}
	ASSERT_TRUE(s.has_value()) << s.error().message;
	EXPECT_EQ(s->tables()[0].name, "temporary");
	EXPECT_EQ(s->tables()[0].columns[0].name, "n");
}

TEST(RelationSchema, FindTableAndColumnByName)
{
	auto s = BuildOne(create_table{"spawn_points", kSpawnColumns});
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
	EXPECT_TRUE(Mentions(missingColumn.error().message, "'z'")) << missingColumn.error().message;

	EXPECT_FALSE(s->FindColumn(7, "x").has_value());
}

TEST(RelationSchema, DuplicateTableNameIsRefused)
{
	add_column const columns[] = {{"n", column_type::Int, false}};
	create_table const tables[] = {{"a", columns}, {"a", columns}};
	auto s = BuildSchema(tables);
	ASSERT_FALSE(s.has_value());
	EXPECT_TRUE(Mentions(s.error().message, "'a'")) << s.error().message;
	EXPECT_TRUE(Mentions(s.error().message, "table 1")) << s.error().message;
}

TEST(RelationSchema, DuplicateColumnNameIsRefused)
{
	add_column const columns[] = {{"n", column_type::Int, false}, {"n", column_type::Double, false}};
	auto s = BuildOne(create_table{"a", columns});
	ASSERT_FALSE(s.has_value());
	EXPECT_TRUE(Mentions(s.error().message, "'n'")) << s.error().message;
}

TEST(RelationSchema, EmptyNamesAreRefused)
{
	add_column const columns[] = {{"n", column_type::Int, false}};
	EXPECT_FALSE(BuildOne(create_table{"", columns}).has_value());

	add_column const unnamed[] = {{"", column_type::Int, false}};
	EXPECT_FALSE(BuildOne(create_table{"a", unnamed}).has_value());
}

TEST(RelationSchema, TableWithoutColumnsIsRefused)
{
	auto s = BuildOne(create_table{"bare", {}});
	ASSERT_FALSE(s.has_value());
	EXPECT_TRUE(Mentions(s.error().message, "'bare'")) << s.error().message;
}

TEST(RelationSchema, OnlyHashFlagsAndIntCanBeKeys)
{
	for (column_type type : {column_type::Double, column_type::Condition, column_type::Command})
	{
		add_column const columns[] = {{"k", type, true}};
		auto s = BuildOne(create_table{"a", columns});
		ASSERT_FALSE(s.has_value());
		EXPECT_TRUE(Mentions(s.error().message, "'k'")) << s.error().message;
	}
	for (column_type type : {column_type::Hash, column_type::Flags, column_type::Int})
	{
		add_column const columns[] = {{"k", type, true}};
		EXPECT_TRUE(BuildOne(create_table{"a", columns}).has_value());
	}
}

TEST(RelationSchema, OrderIsRecorded)
{
	uint32_t const columns[] = {1, 0};
	auto s = SpawnPointsWith(order_by{columns, true});
	ASSERT_TRUE(s.has_value()) << s.error().message;
	table_def const& t = s->tables()[0];
	ASSERT_EQ(t.order.size(), 2u);
	EXPECT_EQ(t.order[0], 1u);
	EXPECT_EQ(t.order[1], 0u);
	EXPECT_TRUE(t.unique);
}

TEST(RelationSchema, TableWithoutOrderKeepsInsertionOrder)
{
	auto s = BuildOne(create_table{"spawn_points", kSpawnColumns});
	ASSERT_TRUE(s.has_value()) << s.error().message;
	EXPECT_EQ(s->tables()[0].order.size(), 0u);
	EXPECT_FALSE(s->tables()[0].unique);
}

TEST(RelationSchema, BadOrdersAreRefused)
{
	auto noColumns = SpawnPointsWith(order_by{{}, false});
	ASSERT_FALSE(noColumns.has_value());
	EXPECT_TRUE(Mentions(noColumns.error().message, "spawn_points")) << noColumns.error().message;

	uint32_t const missing[] = {9};
	auto missingColumn = SpawnPointsWith(order_by{missing, false});
	ASSERT_FALSE(missingColumn.has_value());
	EXPECT_TRUE(Mentions(missingColumn.error().message, "9")) << missingColumn.error().message;

	uint32_t const twice[] = {1, 1};
	auto repeated = SpawnPointsWith(order_by{twice, false});
	ASSERT_FALSE(repeated.has_value());
	EXPECT_TRUE(Mentions(repeated.error().message, "'section'")) << repeated.error().message;

	uint32_t const real[] = {2};
	auto onDouble = SpawnPointsWith(order_by{real, false});
	ASSERT_FALSE(onDouble.has_value());
	EXPECT_TRUE(Mentions(onDouble.error().message, "'x'")) << onDouble.error().message;

	add_column const rules[] = {{"when", column_type::Condition, false}};
	uint32_t const first[] = {0};
	order_by const byCondition{first, false};
	auto onCondition = BuildOne(create_table{"rules", rules, &byCondition});
	ASSERT_FALSE(onCondition.has_value());
	EXPECT_TRUE(Mentions(onCondition.error().message, "'when'")) << onCondition.error().message;
}

TEST(RelationSchema, ARefusalReportsTheTableItRefused)
{
	uint32_t const   byKey[] = {0};
	order_by const   order{byKey, false};
	add_column const fine[]   = {{"v", column_type::Int}};
	add_column const noDomain[] = {{"k", column_type::Int, true}, {"v", column_type::Int}};
	create_table const tables[] = {
		{"fine", fine},
		{"keyed", noDomain, &order, true, {}, pick_policy::Best},
	};
	uint32_t refused = UINT32_MAX;
	auto s = BuildSchema(tables, &refused);
	ASSERT_FALSE(s.has_value());
	EXPECT_EQ(refused, 1u);

	refused = UINT32_MAX;
	auto good = BuildSchema(std::span<create_table const>(tables, 1), &refused);
	ASSERT_TRUE(good.has_value());
	EXPECT_EQ(refused, UINT32_MAX) << "left alone when nothing is refused";

	// A cycle is reported at the first table of the loop.
	add_column const aColumns[] = {{"k", column_type::Int, true, UINT32_MAX, {domain_kind::Column, {}, "b", "k"}},
	                               {"v", column_type::Int}};
	add_column const bColumns[] = {{"k", column_type::Int, true, UINT32_MAX, {domain_kind::Column, {}, "a", "k"}},
	                               {"v", column_type::Int}};
	create_table const looped[] = {
		{"fine", fine},
		{"a", aColumns, &order, true, {}, pick_policy::Best},
		{"b", bColumns, &order, true, {}, pick_policy::Best},
	};
	refused = UINT32_MAX;
	auto cycle = BuildSchema(looped, &refused);
	ASSERT_FALSE(cycle.has_value());
	EXPECT_EQ(refused, 1u);
}
