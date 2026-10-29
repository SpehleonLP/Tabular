#include <gtest/gtest.h>
#include "../src/document_header.h"

#include <string>
#include <string_view>

using namespace tabular;
using namespace tabular::detail;

namespace
{

bool Mentions(std::string const& message, std::string_view text)
{
	return message.find(text) != std::string::npos;
}

struct expected_cell
{
	std::string_view           text;
	header_cell::mark          what;
	bool                       fallthrough;
	std::string_view           name;
	std::optional<column_type> type;
	std::optional<std::string_view> fallback;
	column_type                resolved;
};

}

TEST(DocumentHeader, ReadsEveryForm)
{
	using mark = header_cell::mark;
	expected_cell const cases[] = {
		{">mode",              mark::Key,  false, "mode",   std::nullopt,          std::nullopt,  column_type::Hash},
		{">event?",            mark::Key,  true,  "event",  std::nullopt,          std::nullopt,  column_type::Hash},
		{">level: int",        mark::Key,  false, "level",  column_type::Int,      std::nullopt,  column_type::Int},
		{"hp: band",           mark::None, false, "hp",     column_type::Band,     std::nullopt,  column_type::Band},
		{"^x",                 mark::Edge, false, "x",      std::nullopt,          std::nullopt,  column_type::Band},
		{"^x: band",           mark::Edge, false, "x",      column_type::Band,     std::nullopt,  column_type::Band},
		{"next = mode",        mark::None, false, "next",   std::nullopt,          "mode",        column_type::Hash},
		{"status: hash = None", mark::None, false, "status", column_type::Hash,    "None",        column_type::Hash},
		{"when",               mark::None, false, "when",   std::nullopt,          std::nullopt,  column_type::Condition},
		{"command",            mark::None, false, "command", std::nullopt,         std::nullopt,  column_type::Command},
		{"weight",             mark::None, false, "weight", std::nullopt,          std::nullopt,  column_type::Double},
		{" >sub?:hash ",       mark::Key,  true,  "sub",    column_type::Hash,     std::nullopt,  column_type::Hash},
	};
	for (expected_cell const& c : cases)
	{
		auto cell = ParseHeaderCell(c.text);
		ASSERT_TRUE(cell.has_value()) << c.text << ": " << cell.error().format();
		EXPECT_EQ(cell->what, c.what) << c.text;
		EXPECT_EQ(cell->fallthrough, c.fallthrough) << c.text;
		EXPECT_EQ(cell->name, c.name) << c.text;
		EXPECT_EQ(cell->type, c.type) << c.text;
		EXPECT_EQ(cell->fallback, c.fallback) << c.text;
		auto type = TypeOfCell(*cell);
		ASSERT_TRUE(type.has_value()) << c.text << ": " << type.error().format();
		EXPECT_EQ(*type, c.resolved) << c.text;
	}
}

TEST(DocumentHeader, RefusesMalformedCells)
{
	// Each is a typo that would otherwise read as some hash column.
	std::string_view const parse[][2] = {
		{">mode:",       ">mode:"},
		{"level: itn",   "itn"},
		{"next =",       "next ="},
		{"?mode",        "?mode"},
		{"^x?",          "^x?"},
		{">mode = idle", ">mode = idle"},
		{"a b",          "a b"},
		{"",             "empty"},
		{">",            ">"},
		{"x: int: int",  "x: int: int"},
		{"x = a = b",    "x = a = b"},
	};
	for (auto const& c : parse)
	{
		auto cell = ParseHeaderCell(c[0]);
		ASSERT_FALSE(cell.has_value()) << "'" << c[0] << "' was read";
		EXPECT_TRUE(Mentions(cell.error().format(), c[1])) << cell.error().format();
	}
	// These parse, but their type contradicts their name or mark.
	std::string_view const typed[][2] = {
		{"^x: int",       "^x: int"},
		{"when: int",     "when: int"},
		{"weight: int",   "weight: int"},
		{"command: hash", "command: hash"},
	};
	for (auto const& c : typed)
	{
		auto cell = ParseHeaderCell(c[0]);
		ASSERT_TRUE(cell.has_value()) << c[0] << ": " << cell.error().format();
		auto type = TypeOfCell(*cell);
		ASSERT_FALSE(type.has_value()) << "'" << c[0] << "' was typed";
		EXPECT_TRUE(Mentions(type.error().format(), c[1])) << type.error().format();
	}
}
