#include <gtest/gtest.h>
#include "tabular/pick.h"
#include "tabular/relation.h"
#include "tabular/rows_csv.h"
#include "tabular/schema_yaml.h"

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

constexpr std::string_view kLookup = R"(
tables:
  - name: lookup
    read_only: true
    pick: best
    inputs:
      - {name: mode, type: flags}
    columns:
      - {name: label, type: int, key: true}
      - {name: req, type: flags, input: mode}
      - {name: clip, type: string}
    order_by: {columns: [label]}
    pivot: {headers: req, cells: clip}
)";

// animation's bit_value lookup fixture with int labels for its cases.
constexpr std::string_view kBitValue =
	"label,None,Injured,Crouch,Injured|Crouch,Prone,Crouch|Prone\n"
	"1,c1_none,c1_injured,c1_crouch,,,\n"
	"2,c2_none,c2_injured,c2_crouch,c2_combo,,\n"
	"3,c3_none,,,c3_combo,c3_prone,\n"
	"4,c4_none,,c4_crouch,c4_combo,,\n"
	"5,c5_none,,,c5_combo,,c5_crouchprone\n"
	"6,c6_none,,,c6_combo,,\n";

relation_store StoreFrom(std::string_view yaml)
{
	auto s = SchemaFromYaml(yaml, "pivot.yaml");
	EXPECT_TRUE(s.has_value()) << (s ? std::string() : s.error().format());
	return relation_store(std::move(*s));
}

tl::expected<void, ErrorContext> LoadPivot(relation_store& store, std::string_view text,
	std::vector<std::string>* names = nullptr, uint64_t owner = 1)
{
	csv_rows rows{0, owner, text, "lookup.csv"};
	rows.pivoted    = true;
	rows.flag_names = names;
	return LoadCsv(store, rows);
}

std::string Clip(relation_store const& store, int64_t label, int64_t mode)
{
	cell const keys[]   = {cell::Integer(label)};
	cell const inputs[] = {cell::Integer(mode)};
	auto picked = PickBest(store, 0, pick_query{keys, inputs});
	EXPECT_TRUE(picked.has_value()) << (picked ? std::string() : picked.error().format());
	if (!picked || !*picked)
	{
		return "";
	}
	return std::string(std::string_view(*store.StringAt(0, **picked, 2)));
}

std::string Error(tl::expected<void, ErrorContext> const& r)
{
	return r ? std::string() : r.error().format();
}

}

TEST(PivotCsv, AnimationsLookupFixturePicksAnimationsCells)
{
	relation_store store = StoreFrom(kLookup);
	std::vector<std::string> names;
	ASSERT_EQ(Error(LoadPivot(store, kBitValue, &names)), "");
	EXPECT_EQ(names, (std::vector<std::string>{"Injured", "Crouch", "Prone"}));
	int64_t const injured = 1, crouch = 2, prone = 4;
	EXPECT_EQ(Clip(store, 1, injured | crouch), "c1_crouch");
	EXPECT_EQ(Clip(store, 2, injured | crouch), "c2_combo");
	EXPECT_EQ(Clip(store, 3, injured | crouch | prone), "c3_prone");
	EXPECT_EQ(Clip(store, 4, injured), "c4_combo");
	EXPECT_EQ(Clip(store, 5, crouch), "c5_crouchprone");
	EXPECT_EQ(Clip(store, 6, prone), "c6_none");
}

TEST(PivotCsv, ALabelWithoutNoneIsRefusedNamingItsLine)
{
	relation_store store = StoreFrom(kLookup);
	std::string text = std::string(kBitValue) + "7,,c7_injured,,,,\n";
	std::string error = Error(LoadPivot(store, text));
	EXPECT_TRUE(Mentions(error, "no row with no flags")) << error;
	EXPECT_TRUE(Mentions(error, "lookup.csv:8")) << error;
}

TEST(PivotCsv, ADefaultMayComeFromAnUnpivotedFile)
{
	relation_store store = StoreFrom(kLookup);
	ASSERT_EQ(Error(LoadCsv(store, csv_rows{0, 2, "label,req,clip\n7,0,c7_none\n", "defaults.csv"})), "");
	ASSERT_EQ(Error(LoadPivot(store, "label,None,Injured\n7,,c7_injured\n", nullptr, 3)), "");
	EXPECT_EQ(Clip(store, 7, 1), "c7_injured");
	EXPECT_EQ(Clip(store, 7, 2), "c7_none");
}

TEST(PivotCsv, HeadersAreTrimmedAndNoneIgnoresCase)
{
	relation_store store = StoreFrom(kLookup);
	std::vector<std::string> names;
	ASSERT_EQ(Error(LoadPivot(store, "label, none ,Injured,Crouch, Injured | Crouch \n1,n,i,c,ic\n", &names)), "");
	EXPECT_EQ(names, (std::vector<std::string>{"Injured", "Crouch"}));
	EXPECT_EQ(Clip(store, 1, 3), "ic");
}

TEST(PivotCsv, BadHeadersAreRefusedByName)
{
	relation_store store = StoreFrom(kLookup);
	EXPECT_TRUE(Mentions(Error(LoadPivot(store, "label,Injured\n1,i\n")), "first flag header is None"));
	EXPECT_TRUE(Mentions(Error(LoadPivot(store, "label,None,Injured,Injured\n1,n,i,j\n")), "'Injured' twice"));
	EXPECT_TRUE(Mentions(Error(LoadPivot(store, "label,None,Injured|Crouch,Crouch\n1,n,ic,c\n")),
		"names 'Injured', which is not a simple header to its left"));
	EXPECT_TRUE(Mentions(Error(LoadPivot(store, "x,None\n1,n\n")), "'label'"));
	std::string wide = "label,None";
	std::string row  = "1,n";
	for (int b = 0; b < 33; ++b)
	{
		wide += cc(std::string(), ",f", b);
		row  += ",x";
	}
	EXPECT_TRUE(Mentions(Error(LoadPivot(store, wide + "\n" + row + "\n")), "32 bits"));
}

TEST(PivotCsv, APhaseTableHasNoNoneColumn)
{
	relation_store store = StoreFrom(R"(
tables:
  - name: phase
    read_only: true
    pick: or
    output: bits
    inputs:
      - {name: t, type: double}
    columns:
      - {name: clip, type: int, key: true}
      - {name: bits, type: flags}
      - {name: window, type: band, input: t}
    order_by: {columns: [clip]}
    pivot: {headers: bits, cells: window}
)");
	std::vector<std::string> names;
	csv_rows rows{0, 1, "clip,Footstep,Pivot\n1,0.0..0.4,\n2,0.0..0.3,0.4..0.5\n2,0.6..0.9,\n", "phase.csv"};
	rows.pivoted    = true;
	rows.flag_names = &names;
	ASSERT_EQ(Error(LoadCsv(store, rows)), "");
	EXPECT_EQ(names, (std::vector<std::string>{"Footstep", "Pivot"}));
	auto bits = [&](int64_t clip, double t)
	{
		cell const keys[]   = {cell::Integer(clip)};
		cell const inputs[] = {cell::Real(t)};
		return *PickOr(store, 0, pick_query{keys, inputs});
	};
	EXPECT_EQ(bits(2, 0.45), 2u);
	EXPECT_EQ(bits(2, 0.7), 1u);
	EXPECT_EQ(bits(1, 0.5), 0u);
}

TEST(PivotCsv, APivotIsDeclaredOnAShapeThatCanHoldIt)
{
	auto withExtra = SchemaFromYaml(R"(
tables:
  - name: lookup
    read_only: true
    pick: best
    inputs:
      - {name: mode, type: flags}
    columns:
      - {name: label, type: int, key: true}
      - {name: req, type: flags, input: mode}
      - {name: clip, type: string}
      - {name: extra, type: int}
    order_by: {columns: [label]}
    pivot: {headers: req, cells: clip}
)", "x.yaml");
	ASSERT_FALSE(withExtra.has_value());
	EXPECT_TRUE(Mentions(withExtra.error().format(), "'extra'")) << withExtra.error().format();

	auto unranked = SchemaFromYaml(R"(
tables:
  - name: lookup
    read_only: true
    pick: best
    columns:
      - {name: label, type: int, key: true}
      - {name: req, type: flags}
      - {name: clip, type: string}
    order_by: {columns: [label]}
    pivot: {headers: req, cells: clip}
)", "x.yaml");
	ASSERT_FALSE(unranked.has_value());
	EXPECT_TRUE(Mentions(unranked.error().format(), "ranking column")) << unranked.error().format();

	relation_store plain = StoreFrom(R"(
tables:
  - name: t
    read_only: true
    columns:
      - {name: x, type: int}
)");
	csv_rows rows{0, 1, "x\n1\n", "t.csv"};
	rows.pivoted = true;
	EXPECT_TRUE(Mentions(Error(LoadCsv(plain, rows)), "has no pivot"));
}

TEST(PivotCsv, EqualRanksFollowHeaderOrder)
{
	relation_store store = StoreFrom(kLookup);
	// Two names for the same flags (duplicates are by name); the left header wins.
	ASSERT_EQ(Error(LoadPivot(store, "label,None,Injured,Crouch,Crouch|Injured,Injured|Crouch\n1,n,,,left,right\n")), "");
	EXPECT_EQ(Clip(store, 1, 3), "left");
}
