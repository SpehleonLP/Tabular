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
      - {name: label, type: int, key: true, domain: rows}
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
	auto clip = store.StringAt(0, **picked, 2);
	EXPECT_TRUE(clip.has_value()) << (clip ? std::string() : clip.error().format());
	return clip ? std::string(std::string_view(*clip)) : std::string();
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
		auto picked = PickOr(store, 0, pick_query{keys, inputs});
		EXPECT_TRUE(picked.has_value()) << (picked ? std::string() : picked.error().format());
		return picked ? *picked : 0xFFFFFFFFu;
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
      - {name: label, type: int, key: true, domain: rows}
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
      - {name: label, type: int, key: true, domain: rows}
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

// Two names for one flag set would make two columns with one rank, and a tie
// only the pivot could break. Headers are distinct flag sets, so the author is
// told instead.
TEST(PivotCsv, EqualFlagSetsAreRefusedNamingBothHeaders)
{
	relation_store store = StoreFrom(kLookup);
	std::string error = Error(LoadPivot(store, "label,None,Injured,Crouch,Crouch|Injured,Injured|Crouch\n1,n,,,left,right\n"));
	EXPECT_TRUE(Mentions(error, "'Crouch|Injured'")) << error;
	EXPECT_TRUE(Mentions(error, "'Injured|Crouch'")) << error;
	EXPECT_TRUE(Mentions(error, "same flag set")) << error;
	EXPECT_TRUE(Mentions(error, "lookup.csv:1")) << error;
	EXPECT_EQ(store.RowCount(0), 0u);
	EXPECT_TRUE(Mentions(Error(LoadPivot(store, "label,None,Injured,Injured|Injured\n1,n,i,j\n")), "same flag set"))
		<< "a combo of one header is that header";
}

TEST(PivotCsv, ALaterNoneHeaderIsRefusedNotMadeAFlagBit)
{
	relation_store store = StoreFrom(kLookup);
	for (char const* none : {"none", "None", "NONE", "nOnE"})
	{
		std::vector<std::string> names = {"kept"};
		std::string error = Error(LoadPivot(store, std::string("label,None,Injured,") + none + "\n1,n,i,x\n", &names));
		EXPECT_TRUE(Mentions(error, "None")) << none << ": " << error;
		EXPECT_TRUE(Mentions(error, "0 value")) << none << ": " << error;
		EXPECT_EQ(names, (std::vector<std::string>{"kept"})) << none;
	}
	EXPECT_EQ(store.RowCount(0), 0u);
}

TEST(PivotCsv, AHeaderWithNoFlagHeadersIsRefused)
{
	relation_store store = StoreFrom(kLookup);
	ASSERT_EQ(Error(LoadPivot(store, "label,None\n1,n\n")), "");
	std::string error = Error(LoadPivot(store, "label\n1\n"));
	EXPECT_TRUE(Mentions(error, "no flag headers")) << error;
	EXPECT_EQ(Clip(store, 1, 0), "n") << "the refused load did not retract the owner's rows";
}

TEST(PivotCsv, ARefusedLoadLeavesTheFlagNames)
{
	relation_store store = StoreFrom(kLookup);
	std::vector<std::string> names = {"kept"};
	EXPECT_TRUE(Mentions(Error(LoadPivot(store, "label,None,Injured\n7,,c7_injured\n", &names)), "no row with no flags"));
	EXPECT_EQ(names, (std::vector<std::string>{"kept"}));
}

TEST(PivotCsv, KeysThatCollideAreRefused)
{
	relation_store store = StoreFrom(R"(
tables:
  - name: lookup
    read_only: true
    pick: best
    inputs:
      - {name: mode, type: flags}
    columns:
      - {name: label, type: hash, key: true, domain: rows}
      - {name: req, type: flags, input: mode}
      - {name: clip, type: string}
    order_by: {columns: [label]}
    pivot: {headers: req, cells: clip}
)");
	std::string error = Error(LoadPivot(store, "label,None,Injured\nn26963,a,b\nn50336,c,d\n"));
	EXPECT_TRUE(Mentions(error, "'n26963'")) << error;
	EXPECT_TRUE(Mentions(error, "lookup.csv:3")) << error;
}

TEST(PivotCsv, HashCellsThatCollideAreRefused)
{
	relation_store store = StoreFrom(R"(
tables:
  - name: lookup
    read_only: true
    pick: best
    inputs:
      - {name: mode, type: flags}
    columns:
      - {name: label, type: int, key: true, domain: rows}
      - {name: req, type: flags, input: mode}
      - {name: clip, type: hash}
    order_by: {columns: [label]}
    pivot: {headers: req, cells: clip}
)");
	std::string error = Error(LoadPivot(store, "label,None,Injured\n1,n26963,n50336\n"));
	EXPECT_TRUE(Mentions(error, "'n50336' has the same hash as 'n26963'")) << error;
	EXPECT_TRUE(Mentions(error, "lookup.csv:2")) << error;
}

// The same rows must meet the same verdict whichever front end brings them in.
// Owner 2's row for label 1 with req A overlaps owner 1's, which has the
// same flags, however the file spelled it: refused as an overlap between owners.
TEST(PivotCsv, RowsAndPivotedCsvGiveOneOutcome)
{
	constexpr std::string_view kBands = R"(
tables:
  - name: lookup
    read_only: true
    pick: best
    inputs:
      - {name: mode, type: flags}
      - {name: t, type: double, range: 0..1}
    columns:
      - {name: label, type: int, key: true, domain: rows}
      - {name: req, type: flags, input: mode}
      - {name: window, type: band, input: t}
    order_by: {columns: [label]}
    pivot: {headers: req, cells: window}
)";
	for (bool pivoted : {false, true})
	{
		relation_store store = StoreFrom(kBands);
		ASSERT_EQ(Error(LoadPivot(store, "label,None,A\n1,0..1,0..1\n")), "") << "owner 1, pivoted";
		std::string error;
		if (pivoted)
		{
			error = Error(LoadPivot(store, "label,None,A\n1,,0..0.5\n", nullptr, 2));
		}
		else
		{
			error = Error(LoadCsv(store, csv_rows{0, 2, "label,req,window\n1,1,0..0.5\n", "rows.csv"}));
		}
		EXPECT_TRUE(Mentions(error, "owner 1's row with key (1), flags 1, window 0..1 and owner 2's loaded row 0 have equal flags and overlap"))
			<< (pivoted ? "pivoted: " : "rows: ") << error;
		EXPECT_EQ(store.RowCount(0), 2u) << "the refused load changes nothing";
	}
}

TEST(PivotCsv, APivotedLoadFillsTheReportWithItsOrphans)
{
	relation_store store = StoreFrom(R"(
tables:
  - name: lookup
    read_only: true
    pick: best
    inputs:
      - {name: mode, type: flags}
    columns:
      - {name: label, type: int, key: true, domain: {members: [1]}}
      - {name: req, type: flags, input: mode}
      - {name: clip, type: string}
    order_by: {columns: [label]}
    pivot: {headers: req, cells: clip}
)");
	std::vector<run_note> report;
	csv_rows rows{0, 1, "label,None,A\n1,c1,c1a\n2,c2,\n", "lookup.csv"};
	rows.pivoted = true;
	rows.report  = &report;
	ASSERT_EQ(Error(LoadCsv(store, rows)), "");
	ASSERT_EQ(report.size(), 1u);
	EXPECT_EQ(report[0].status, run_status::Orphan);
	ASSERT_EQ(report[0].key.size(), 1u);
	EXPECT_EQ(report[0].key[0].i, 2);
}
