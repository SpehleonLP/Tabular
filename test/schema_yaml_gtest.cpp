#include <gtest/gtest.h>
#include "tabular/schema_yaml.h"

#include <string>
#include <string_view>

using namespace tabular;

static bool Mentions(std::string const& message, std::string_view text)
{
	return message.find(text) != std::string::npos;
}

namespace
{

constexpr std::string_view kSpawnPoints = R"(
tables:
  - name: spawn_points
    read_only: true
    columns:
      - {name: package, type: hash, key: true}
      - {name: section, type: hash}
      - {name: x, type: double}
      - {name: y, type: double}
    order_by: {columns: [section, package], unique: true}
  - name: rules
    columns:
      - {name: when, type: condition}
      - {name: then, type: command}
      - {name: mask, type: flags}
      - {name: count, type: int}
)";

}

TEST(SchemaYaml, BuildsEveryTableColumnAndOrder)
{
	auto s = SchemaFromYaml(kSpawnPoints, "spawn.yaml");
	ASSERT_TRUE(s.has_value()) << s.error().format();
	ASSERT_EQ(s->tables().size(), 2u);

	table_def const& spawn = s->tables()[0];
	EXPECT_EQ(spawn.name, "spawn_points");
	EXPECT_TRUE(spawn.read_only);
	ASSERT_EQ(spawn.columns.size(), 4u);
	EXPECT_EQ(spawn.columns[0].type, column_type::Hash);
	EXPECT_TRUE(spawn.columns[0].key);
	EXPECT_FALSE(spawn.columns[1].key);
	EXPECT_EQ(spawn.columns[3].type, column_type::Double);
	ASSERT_EQ(spawn.order.size(), 2u);
	EXPECT_EQ(spawn.order[0], 1u);   // by name: section is column 1
	EXPECT_EQ(spawn.order[1], 0u);
	EXPECT_TRUE(spawn.unique);

	table_def const& rules = s->tables()[1];
	EXPECT_FALSE(rules.read_only);
	EXPECT_EQ(rules.order.size(), 0u);
	EXPECT_EQ(rules.columns[0].type, column_type::Condition);
	EXPECT_EQ(rules.columns[1].type, column_type::Command);
	EXPECT_EQ(rules.columns[2].type, column_type::Flags);
	EXPECT_EQ(rules.columns[3].type, column_type::Int);
}

TEST(SchemaYaml, AnOrderNamingAMissingColumnIsRefused)
{
	auto s = SchemaFromYaml(R"(
tables:
  - name: t
    columns: [{name: a, type: int}]
    order_by: {columns: [b]}
)", "t.yaml");
	ASSERT_FALSE(s.has_value());
	EXPECT_TRUE(Mentions(s.error().format(), "'b'")) << s.error().format();
	EXPECT_TRUE(Mentions(s.error().format(), "t.yaml")) << s.error().format();
}

TEST(SchemaYaml, AnUnknownTypeIsRefused)
{
	auto s = SchemaFromYaml("tables:\n  - name: t\n    columns: [{name: a, type: string}]\n", "t.yaml");
	ASSERT_FALSE(s.has_value());
	EXPECT_TRUE(Mentions(s.error().format(), "string")) << s.error().format();
}

TEST(SchemaYaml, AMisspelledKeyIsRefusedNotIgnored)
{
	auto s = SchemaFromYaml(R"(
tables:
  - name: t
    columns: [{name: a, type: int}]
    order_by: {columns: [a], uniqe: true}
)", "t.yaml");
	ASSERT_FALSE(s.has_value());
	EXPECT_TRUE(Mentions(s.error().format(), "uniqe")) << s.error().format();
}

TEST(SchemaYaml, MissingRequiredPartsAreRefused)
{
	EXPECT_FALSE(SchemaFromYaml("tables:\n  - columns: [{name: a, type: int}]\n", "t.yaml").has_value());
	EXPECT_FALSE(SchemaFromYaml("tables:\n  - name: t\n", "t.yaml").has_value());
	EXPECT_FALSE(SchemaFromYaml("tables:\n  - name: t\n    columns: [{name: a}]\n", "t.yaml").has_value());
	EXPECT_FALSE(SchemaFromYaml("something: else\n", "t.yaml").has_value());
}

TEST(SchemaYaml, CoreRefusalsCarryTheLabel)
{
	// Two tables named t: BuildSchema refuses it, and the label says which file.
	auto s = SchemaFromYaml(R"(
tables:
  - {name: t, columns: [{name: a, type: int}]}
  - {name: t, columns: [{name: a, type: int}]}
)", "dup.yaml");
	ASSERT_FALSE(s.has_value());
	EXPECT_TRUE(Mentions(s.error().format(), "dup.yaml")) << s.error().format();
	EXPECT_TRUE(Mentions(s.error().format(), "'t'")) << s.error().format();
}

TEST(SchemaYaml, MalformedYamlIsRefused)
{
	auto s = SchemaFromYaml("tables: [\n", "bad.yaml");
	ASSERT_FALSE(s.has_value());
	EXPECT_TRUE(Mentions(s.error().format(), "bad.yaml")) << s.error().format();
}

TEST(SchemaYaml, ReadsTheVec2Type)
{
	auto s = SchemaFromYaml("tables:\n  - name: t\n    columns:\n      - {name: at, type: vec2}\n", "t.yaml");
	ASSERT_TRUE(s.has_value()) << s.error().format();
	EXPECT_EQ(s->tables()[0].columns[0].type, column_type::Vec2);
}
