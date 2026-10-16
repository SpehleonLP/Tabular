#include <gtest/gtest.h>
#include "tabular/relation.h"
#include "Support/counted_string.h"

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
	ASSERT_TRUE(store.Load(load_rows{.table = 0, .owner = 1, .count = 1, .values = values}).has_value());
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

namespace
{

// places(name string key, priority int), ordered by priority; read_only as given.
relation_store Places(bool read_only)
{
	static add_column const columns[] = {{"name", column_type::String, true}, {"priority", column_type::Int, false}};
	static uint32_t const   order[]   = {1};
	static order_by const   ordering{order, false};
	auto s = OneTable(columns, &ordering, read_only);
	EXPECT_TRUE(s.has_value()) << s.error().message;
	return relation_store(std::move(*s));
}

tl::expected<void, ErrorContext> InsertPlace(relation_store& store, uint64_t owner, string_in name, int64_t priority)
{
	cell const      values[]  = {cell::Integer(priority)};
	string_in const strings[] = {name};
	return store.Insert(insert_row{0, owner, values, {}, {}, strings});
}

}

TEST(RelationColumns, AStringOutlivesTheTextItWasLoadedFrom)
{
	relation_store store = Places(true);
	std::string text = "forest";
	cell const      values[]  = {cell::Integer(1)};
	string_in const strings[] = {std::string_view(text)};
	ASSERT_TRUE(store.Load(load_rows{0, 1, 1, values, {}, nullptr, {}, strings}).has_value());
	text.assign("XXXXXX");
	auto name = store.StringAt(0, 0, 0);
	ASSERT_TRUE(name.has_value()) << name.error().message;
	EXPECT_EQ(std::string_view(*name), "forest");
	auto version = *store.Version(0);
	EXPECT_EQ(std::string_view(*version->StringAt(0, 0)), "forest");
}

TEST(RelationColumns, AStringKeyIsFoundByContent)
{
	relation_store store = Places(true);
	cell const      values[]  = {cell::Integer(1), cell::Integer(2)};
	string_in const strings[] = {std::string_view("meadow"), std::string_view("moor")};
	ASSERT_TRUE(store.Load(load_rows{0, 1, 2, values, {}, nullptr, {}, strings}).has_value());
	std::string probe = std::string("mo") + "or";
	EXPECT_EQ(*store.Find(0, 0, std::string_view(probe)), std::vector<size_t>{1});
	EXPECT_EQ(*(*store.Version(0))->Find(0, std::string_view("meadow")), std::vector<size_t>{0});
	EXPECT_TRUE(store.Find(0, 0, std::string_view("marsh"))->empty());
}

TEST(RelationColumns, AnInternedCountedStringIsStoredAsTheSameHandle)
{
	relation_store store = Places(false);
	counted_string const shared = counted_string::MakeShared(std::string_view("meadow"));
	ASSERT_TRUE(InsertPlace(store, 1, shared, 1).has_value());
	EXPECT_EQ(store.StringAt(0, 0, 0)->ptr_compare(shared), 0);

	counted_string const unique = counted_string::MakeUnique(std::string_view("moor"));
	ASSERT_TRUE(InsertPlace(store, 1, unique, 2).has_value());
	EXPECT_NE(store.StringAt(0, 1, 0)->ptr_compare(unique), 0) << "a unique string is interned by content";
	EXPECT_EQ(*store.Find(0, 0, std::string_view(unique)), std::vector<size_t>{1});
}

TEST(RelationColumns, StringKeyFollowsRowsShiftedByAnOrderedInsert)
{
	relation_store store = Places(false);
	ASSERT_TRUE(InsertPlace(store, 1, std::string_view("b"), 2).has_value());
	ASSERT_TRUE(InsertPlace(store, 1, std::string_view("a"), 1).has_value());   // lands before "b"
	EXPECT_EQ(*store.Find(0, 0, std::string_view("a")), std::vector<size_t>{0});
	EXPECT_EQ(*store.Find(0, 0, std::string_view("b")), std::vector<size_t>{1});
}

TEST(RelationColumns, WritableStringsSurviveRetractingAnotherOwner)
{
	relation_store store = Places(false);
	ASSERT_TRUE(InsertPlace(store, 1, std::string_view("kept"), 1).has_value());
	ASSERT_TRUE(InsertPlace(store, 2, std::string_view("gone"), 2).has_value());
	store.Retract(retract_owner{2});
	ASSERT_EQ(store.RowCount(0), 1u);
	EXPECT_EQ(std::string_view(*store.StringAt(0, 0, 0)), "kept");
	EXPECT_EQ(*store.Find(0, 0, std::string_view("kept")), std::vector<size_t>{0});
	EXPECT_TRUE(store.Find(0, 0, std::string_view("gone"))->empty());
}

TEST(RelationColumns, EmptyStringIsStoredAndFound)
{
	relation_store store = Places(false);
	ASSERT_TRUE(InsertPlace(store, 1, std::string_view(""), 1).has_value());
	EXPECT_EQ(std::string_view(*store.StringAt(0, 0, 0)), "");
	EXPECT_EQ(*store.Find(0, 0, std::string_view("")), std::vector<size_t>{0});
}

// CSV cells are views into the file, so a cell is a prefix of everything after it: storing
// one must keep its own length even when a longer string starting with it is interned.
TEST(RelationColumns, AStringViewIntoALongerBufferKeepsItsLength)
{
	std::string const buffer = "relation-columns-prefix-probe_and_more";
	auto whole = counted_string::MakeShared(buffer);
	relation_store store = Places(false);
	ASSERT_TRUE(InsertPlace(store, 1, std::string_view(buffer).substr(0, 29), 1).has_value());
	EXPECT_EQ(std::string_view(*store.StringAt(0, 0, 0)), "relation-columns-prefix-probe");
	EXPECT_EQ(*store.Find(0, 0, std::string_view("relation-columns-prefix-probe")), std::vector<size_t>{0});
}

TEST(RelationColumns, AStringIsNotInAnOrderAndNotAValue)
{
	add_column const columns[] = {{"name", column_type::String, false}};
	uint32_t const   first[]   = {0};
	order_by const   order{first, false};
	auto inOrder = OneTable(columns, &order, false);
	ASSERT_FALSE(inOrder.has_value());
	EXPECT_TRUE(Mentions(inOrder.error().message, "cannot be in an order")) << inOrder.error().message;

	relation_store store = Places(false);
	ASSERT_TRUE(InsertPlace(store, 1, std::string_view("a"), 1).has_value());
	EXPECT_FALSE(store.Value(0, 0, 0).has_value()) << "a string column is read with StringAt";
	EXPECT_FALSE(store.Find(0, 0, cell::Integer(0)).has_value()) << "a string key is looked up by text";
	EXPECT_FALSE(store.StringAt(0, 0, 1).has_value()) << "column 1 is an int";
}
