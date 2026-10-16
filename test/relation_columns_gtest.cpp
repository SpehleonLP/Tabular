#include <gtest/gtest.h>
#include "tabular/relation.h"

#include <string>
#include <string_view>
#include <vector>

using namespace tabular;

static bool Mentions(std::string const& message, std::string_view text)
{
	return message.find(text) != std::string::npos;
}

namespace
{

tl::expected<schema, ErrorContext> OneTable(std::span<add_column const> columns, order_by const* order,
	bool read_only)
{
	create_table const table{"t", columns, order, read_only};
	return BuildSchema(std::span<create_table const>(&table, 1));
}

}

TEST(RelationColumns, Vec2HoldsTwoFloatsInOneCell)
{
	static_assert(sizeof(cell) == 8);
	cell c = cell::Vector(1.5f, -2.25f);
	EXPECT_EQ(c.v[0], 1.5f);
	EXPECT_EQ(c.v[1], -2.25f);
}

TEST(RelationColumns, Vec2RoundTripsThroughLoad)
{
	add_column const columns[] = {{"id", column_type::Int, false}, {"at", column_type::Vec2, false}};
	auto s = OneTable(columns, nullptr, true);
	ASSERT_TRUE(s.has_value()) << s.error().message;
	relation_store store(std::move(*s));
	cell const values[] = {cell::Integer(1), cell::Vector(0.25f, 0.5f)};
	ASSERT_TRUE(store.Load(load_rows{0, 1, 1, values, {}}).has_value());
	auto at = store.Value(0, 0, 1);
	ASSERT_TRUE(at.has_value());
	EXPECT_EQ(at->v[0], 0.25f);
	EXPECT_EQ(at->v[1], 0.5f);
}

TEST(RelationColumns, Vec2IsNeitherAKeyNorInAnOrder)
{
	add_column const keyed[] = {{"at", column_type::Vec2, true}};
	auto asKey = OneTable(keyed, nullptr, false);
	ASSERT_FALSE(asKey.has_value());
	EXPECT_TRUE(Mentions(asKey.error().message, "vec2")) << asKey.error().message;

	add_column const plain[] = {{"at", column_type::Vec2, false}};
	uint32_t const   first[] = {0};
	order_by const   order{first, false};
	auto inOrder = OneTable(plain, &order, false);
	ASSERT_FALSE(inOrder.has_value());
	EXPECT_TRUE(Mentions(inOrder.error().message, "cannot be in an order")) << inOrder.error().message;
}
