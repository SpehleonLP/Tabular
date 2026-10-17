#include <gtest/gtest.h>
#include "tabular/relation.h"
#include "tabular/rows_csv.h"

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

add_input const kInputs[] = {{"mode", column_type::Flags}, {"t", column_type::Double}};

// label (key), req (ranks by mode), window (band over t), clip.
std::vector<add_column> Columns()
{
	return {
		{"label",  column_type::Int,   true},
		{"req",    column_type::Flags, false, 0},
		{"window", column_type::Band,  false, 1},
		{"clip",   column_type::Int},
	};
}

uint32_t const kByLabel[] = {0};
order_by const kOrder{kByLabel, false};

tl::expected<schema, ErrorContext> Build(std::vector<add_column> const& columns,
	std::span<add_input const> inputs = kInputs, pick_policy pick = pick_policy::Best,
	uint32_t output = UINT32_MAX, order_by const* order = &kOrder, bool read_only = true)
{
	create_table const t{"lookup", columns, order, read_only, inputs, pick, output};
	return BuildSchema(std::span<create_table const>(&t, 1));
}

std::string Refusal(tl::expected<schema, ErrorContext> const& s)
{
	return s ? std::string() : s.error().message;
}

}

TEST(PickSchema, APickedTableResolvesItsShape)
{
	auto s = Build(Columns());
	ASSERT_TRUE(s.has_value()) << s.error().message;
	table_def const& def = s->tables()[0];
	EXPECT_EQ(def.pick, pick_policy::Best);
	EXPECT_EQ(def.ranking, 1u);
	EXPECT_EQ(def.key_count, 1u);
	EXPECT_EQ(def.columns[2].input, 1u);
	ASSERT_EQ(def.inputs.size(), 2u);
	EXPECT_EQ(def.inputs[0].name, "mode");
	EXPECT_EQ(def.inputs[1].type, column_type::Double);
}

TEST(PickSchema, InputsAreNamedOnceAndAreDoublesOrFlags)
{
	add_input const twice[] = {{"mode", column_type::Flags}, {"mode", column_type::Double}};
	EXPECT_TRUE(Mentions(Refusal(Build(Columns(), twice)), "input 'mode' twice")) << Refusal(Build(Columns(), twice));
	add_input const wrong[] = {{"mode", column_type::Flags}, {"t", column_type::Int}};
	EXPECT_TRUE(Mentions(Refusal(Build(Columns(), wrong)), "a double or a flag set")) << Refusal(Build(Columns(), wrong));
}

TEST(PickSchema, AColumnReadsAnInputOfItsKind)
{
	auto columns = Columns();
	columns[2].input = UINT32_MAX;
	EXPECT_TRUE(Mentions(Refusal(Build(columns)), "names the double input")) << Refusal(Build(columns));

	columns = Columns();
	columns[2].input = 0;   // a band over the flags input
	EXPECT_TRUE(Mentions(Refusal(Build(columns)), "reads input 'mode'")) << Refusal(Build(columns));

	columns = Columns();
	columns[3].input = 1;   // an int column reading an input
	EXPECT_TRUE(Mentions(Refusal(Build(columns)), "only a band or a flags column")) << Refusal(Build(columns));

	columns = Columns();
	columns[1].input = 5;
	EXPECT_TRUE(Mentions(Refusal(Build(columns)), "reads input 5")) << Refusal(Build(columns));
}

TEST(PickSchema, OneRankingColumnAndOneScriptCondition)
{
	auto columns = Columns();
	columns.push_back({"also", column_type::Flags, false, 0});
	EXPECT_TRUE(Mentions(Refusal(Build(columns)), "ranks by two")) << Refusal(Build(columns));

	columns = Columns();
	columns.push_back({"when", column_type::Condition});
	columns.push_back({"unless", column_type::Condition});
	EXPECT_TRUE(Mentions(Refusal(Build(columns)), "one script condition")) << Refusal(Build(columns));
}

TEST(PickSchema, APickedTableIsReadOnlyAndOrderedByItsKeys)
{
	EXPECT_TRUE(Mentions(Refusal(Build(Columns(), kInputs, pick_policy::Best, UINT32_MAX, &kOrder, false)),
		"read-only"));
	uint32_t const byClip[] = {3};
	order_by const clipOrder{byClip, false};
	EXPECT_TRUE(Mentions(Refusal(Build(Columns(), kInputs, pick_policy::Best, UINT32_MAX, &clipOrder)),
		"starts with its keys")) << Refusal(Build(Columns(), kInputs, pick_policy::Best, UINT32_MAX, &clipOrder));
	EXPECT_TRUE(Mentions(Refusal(Build(Columns(), kInputs, pick_policy::Best, UINT32_MAX, nullptr)),
		"starts with its keys"));
}

TEST(PickSchema, AnOrTableNamesAFlagsOutput)
{
	auto columns = Columns();
	columns[1].input = UINT32_MAX;   // req becomes a plain flags value: the output
	EXPECT_TRUE(Build(columns, kInputs, pick_policy::Or, 1).has_value());
	EXPECT_TRUE(Mentions(Refusal(Build(columns, kInputs, pick_policy::Or)), "names its output"));
	EXPECT_TRUE(Mentions(Refusal(Build(columns, kInputs, pick_policy::Or, 3)), "output 'clip' is not a flag set"));
	EXPECT_TRUE(Mentions(Refusal(Build(Columns(), kInputs, pick_policy::Best, 1)), "only an or table"));
}

TEST(PickSchema, ABandIsAConditionNotAValue)
{
	auto columns = Columns();
	columns[2].key = true;
	EXPECT_TRUE(Mentions(Refusal(Build(columns)), "cannot be a key"));

	uint32_t const byBand[] = {0, 2};
	order_by const bandOrder{byBand, false};
	EXPECT_TRUE(Mentions(Refusal(Build(Columns(), kInputs, pick_policy::Best, UINT32_MAX, &bandOrder)),
		"cannot be in an order"));

	auto s = Build(Columns(), kInputs, pick_policy::None);
	ASSERT_TRUE(s.has_value()) << s.error().message;
	relation_store store(std::move(*s));
	cell const row[] = {cell::Integer(1), cell::Integer(0), cell::Vector(0.0f, 1.0f), cell::Integer(7)};
	ASSERT_TRUE(store.Load(load_rows{0, 1, 1, row}).has_value());
	EXPECT_FALSE(store.Value(0, 0, 2).has_value()) << "a band is read by a pick, not as a value";
	EXPECT_EQ(store.Value(0, 0, 1)->i, 0) << "the ranking column is still a value";
}

TEST(PickSchema, ABandCellIsWrittenAsARange)
{
	auto s = Build(Columns(), kInputs, pick_policy::None);
	ASSERT_TRUE(s.has_value()) << s.error().message;
	relation_store store(std::move(*s));
	EXPECT_TRUE(LoadCsv(store, csv_rows{0, 1, "label,req,window,clip\n1,0,0.25..0.5,7\n", "l.csv"}).has_value());
	auto bad = LoadCsv(store, csv_rows{0, 1, "label,req,window,clip\n1,0,0.25,7\n", "l.csv"});
	ASSERT_FALSE(bad.has_value());
	EXPECT_TRUE(Mentions(bad.error().format(), "is a band, written a..b")) << bad.error().format();
}
