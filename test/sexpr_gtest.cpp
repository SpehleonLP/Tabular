#include <gtest/gtest.h>
#include "../src/sexpr.h"

using namespace tabular::detail;

TEST(Sexpr, ReadsNestedLists)
{
	auto list = ParseList("(table t (pick or bits) (key c hash))", 1, "t");
	ASSERT_TRUE(list.has_value()) << list.error().message;
	ASSERT_EQ(list->items.size(), 4u);
	EXPECT_EQ(list->items[0].text, "table");
	ASSERT_EQ(list->items[2].items.size(), 3u);
	EXPECT_EQ(list->items[2].items[2].text, "bits");
	EXPECT_EQ(list->items[3].items[0].text, "key");
}

TEST(Sexpr, TracksLines)
{
	auto list = ParseList("(a\n (b))", 10, "t");
	ASSERT_TRUE(list.has_value()) << list.error().message;
	EXPECT_EQ(list->line, 10u);
	EXPECT_EQ(list->items[0].line, 10u);
	EXPECT_EQ(list->items[1].line, 11u);
}

TEST(Sexpr, StringsMayHoldParensAndBrackets)
{
	auto list = ParseList("(x \"a ) ] (\")", 1, "t");
	ASSERT_TRUE(list.has_value()) << list.error().message;
	ASSERT_EQ(list->items.size(), 2u);
	EXPECT_EQ(list->items[1].what, sexpr::kind::String);
	EXPECT_EQ(list->items[1].text, "a ) ] (");
}

TEST(Sexpr, UnescapeUndoesQuoteAndBackslash)
{
	auto list = ParseList(R"((x "a\"b\\c"))", 1, "t");
	ASSERT_TRUE(list.has_value()) << list.error().message;
	EXPECT_EQ(list->items[1].text, R"(a\"b\\c)");
	EXPECT_EQ(Unescape(list->items[1]), R"(a"b\c)");
}

TEST(Sexpr, RefusesUnclosed)
{
	auto list = ParseList("(a (b)", 10, "t");
	ASSERT_FALSE(list.has_value());
	EXPECT_EQ(list.error().context.size(), 1u);
	EXPECT_EQ(list.error().context[0], "t:10") << list.error().format();
	EXPECT_EQ(list.error().message.find("line"), std::string::npos) << list.error().message;
	EXPECT_FALSE(ParseList("(a \"b)", 3, "t").has_value());
}

TEST(Sexpr, RefusesTrailingText)
{
	EXPECT_FALSE(ParseList("(a) b", 1, "t").has_value());
	EXPECT_FALSE(ParseList("(a) (b)", 1, "t").has_value());
	EXPECT_FALSE(ParseList("a", 1, "t").has_value());
	EXPECT_FALSE(ParseList("", 1, "t").has_value());
}

TEST(Sexpr, CommentsRunToTheEndOfTheLine)
{
	auto list = ParseList("(a # (b \"c\n  d)", 1, "t");
	ASSERT_TRUE(list.has_value()) << list.error().format();
	ASSERT_EQ(list->items.size(), 2u);
	EXPECT_EQ(list->items[1].text, "d");
	EXPECT_EQ(list->items[1].line, 2u);
	// A '#' inside an atom or a string is text.
	auto text = ParseList("(a#b \"#c\")", 1, "t");
	ASSERT_TRUE(text.has_value()) << text.error().format();
	EXPECT_EQ(text->items[0].text, "a#b");
	EXPECT_EQ(text->items[1].text, "#c");
}

TEST(Sexpr, OnlyQuoteAndBackslashEscape)
{
	auto bad = ParseList("(a\n \"b\\n\")", 5, "t");
	ASSERT_FALSE(bad.has_value());
	EXPECT_EQ(bad.error().context[0], "t:6") << bad.error().format();
	EXPECT_FALSE(ParseList("(\"b\\", 1, "t").has_value());
}

TEST(Sexpr, NestingIsCappedAt64)
{
	auto nested = [](size_t levels)
	{
		return std::string(levels, '(') + std::string(levels, ')');
	};
	EXPECT_TRUE(ParseList(nested(64), 1, "t").has_value());
	auto deep = ParseList("\n" + nested(65), 1, "t");
	ASSERT_FALSE(deep.has_value());
	EXPECT_EQ(deep.error().context[0], "t:2") << deep.error().format();
}
