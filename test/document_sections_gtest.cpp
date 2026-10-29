#include <gtest/gtest.h>
#include "../src/document_sections.h"

using namespace tabular::detail;

TEST(DocumentSections, SplitsHeadersAndBodies)
{
	std::string_view const text =
		"[table a (key k hash)]\n"
		"k\n"
		"x\n"
		"[table b (key k hash)]\n"
		"k\n"
		"y\n";
	auto sections = SplitSections(text, "t.tabular");
	ASSERT_TRUE(sections.has_value()) << sections.error().message;
	ASSERT_EQ(sections->size(), 2u);
	EXPECT_EQ((*sections)[0].header.items[0].text, "table");
	EXPECT_EQ((*sections)[0].header.items[1].text, "a");
	EXPECT_EQ((*sections)[0].body, "k\nx\n");
	EXPECT_EQ((*sections)[0].body_line, 2u);
	EXPECT_EQ((*sections)[1].body, "k\ny\n");
	EXPECT_EQ((*sections)[1].body_line, 5u);
}

TEST(DocumentSections, MultiLineBracket)
{
	std::string_view const text =
		"[table t\n"
		"  (key k hash)\n"
		"  (column v double)]\n"
		"k,v\n";
	auto sections = SplitSections(text, "t.tabular");
	ASSERT_TRUE(sections.has_value()) << sections.error().message;
	ASSERT_EQ(sections->size(), 1u);
	EXPECT_EQ((*sections)[0].header.items.size(), 4u);
	EXPECT_EQ((*sections)[0].header.items[3].line, 3u);
	EXPECT_EQ((*sections)[0].body, "k,v\n");
	EXPECT_EQ((*sections)[0].body_line, 4u);
}

TEST(DocumentSections, QuotedCellMayStartALineWithBracket)
{
	std::string_view const text =
		"[table t (key k hash)]\n"
		"k\n"
		"\"a\n"
		"[b]\"\n";
	auto sections = SplitSections(text, "t.tabular");
	ASSERT_TRUE(sections.has_value()) << sections.error().message;
	ASSERT_EQ(sections->size(), 1u);
	EXPECT_EQ((*sections)[0].body, "k\n\"a\n[b]\"\n");
}

TEST(DocumentSections, UnclosedBracketNamesItsOpeningLine)
{
	std::string_view const text =
		"# note\n"
		"[table t\n"
		"  (key k hash)\n"
		"  (column v double)\n"
		"k,v\n";
	auto sections = SplitSections(text, "t.tabular");
	ASSERT_FALSE(sections.has_value());
	ASSERT_EQ(sections.error().context.size(), 1u);
	EXPECT_EQ(sections.error().context[0], "t.tabular:2") << sections.error().format();
}

TEST(DocumentSections, RowsBeforeAnyBracketAreRefused)
{
	auto sections = SplitSections("# ok\n\nk,v\n[table t (key k hash)]\n", "t.tabular");
	ASSERT_FALSE(sections.has_value());
	EXPECT_EQ(sections.error().context[0], "t.tabular:3") << sections.error().format();
}

TEST(DocumentSections, TextAfterTheClosingBracketIsRefused)
{
	auto sections = SplitSections("[table t (key k hash)] stray\n", "t.tabular");
	ASSERT_FALSE(sections.has_value());
	EXPECT_EQ(sections.error().context[0], "t.tabular:1") << sections.error().format();
	EXPECT_TRUE(SplitSections("[table t (key k hash)] # fine\n", "t.tabular").has_value());
}

TEST(DocumentSections, CommentsAndBlanksAnywhere)
{
	std::string_view const text =
		"\n"
		"# leading\n"
		"[table t (key k hash)]\n"
		"# before the header row\n"
		"\n"
		"k\n"
		"# [not a bracket] and \"not a quote\n"
		"x\n";
	auto sections = SplitSections(text, "t.tabular");
	ASSERT_TRUE(sections.has_value()) << sections.error().message;
	ASSERT_EQ(sections->size(), 1u);
	EXPECT_EQ((*sections)[0].body_line, 4u);
}

TEST(DocumentSections, AFileWithNoBracketsHasNoSections)
{
	auto sections = SplitSections("# nothing\n\n", "t.tabular");
	ASSERT_TRUE(sections.has_value()) << sections.error().message;
	EXPECT_TRUE(sections->empty());
}

TEST(DocumentSections, CommentsInsideABracketAreSkipped)
{
	auto sections = SplitSections("[table t\n  # see [docs]\n  (key k hash)]\nk", "t.tabular");
	ASSERT_TRUE(sections.has_value()) << sections.error().format();
	ASSERT_EQ(sections->size(), 1u);
	auto const& header = (*sections)[0].header;
	ASSERT_EQ(header.items.size(), 3u);
	EXPECT_EQ(header.items[1].text, "t");
	EXPECT_EQ(header.items[2].items[0].text, "key");
	EXPECT_EQ(header.items[2].line, 3u);
	EXPECT_EQ((*sections)[0].body, "k");
	EXPECT_EQ((*sections)[0].body_line, 4u);
}

TEST(DocumentSections, AnUnclosedParenDoesNotRunIntoLaterBrackets)
{
	auto sections = SplitSections("[table t (key k hash]\nk\nx)\n[table u (key k hash)]\n", "t.tabular");
	ASSERT_FALSE(sections.has_value());
	EXPECT_EQ(sections.error().context[0], "t.tabular:1") << sections.error().format();
	EXPECT_NE(sections.error().message.find("'('"), std::string::npos) << sections.error().message;
}

TEST(DocumentSections, AnUnclosedStringNamesItsOpeningLine)
{
	auto sections = SplitSections("# c\n[table t\n  (key k \"hash)]\nk\n", "t.tabular");
	ASSERT_FALSE(sections.has_value());
	EXPECT_EQ(sections.error().context[0], "t.tabular:3") << sections.error().format();
}

TEST(DocumentSections, HeaderErrorsUseTheFileLabel)
{
	auto sections = SplitSections("[table t\n  (key k \"h\\q\")]\n", "t.tabular");
	ASSERT_FALSE(sections.has_value());
	EXPECT_EQ(sections.error().context[0], "t.tabular:2") << sections.error().format();
}
