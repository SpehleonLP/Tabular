#include <gtest/gtest.h>
#include "tabular/pick.h"
#include "tabular/relation.h"
#include "tabular/rows_csv.h"
#include "tabular/schema_yaml.h"
#include "probe_closure.h"

#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace tabular;
using tabular_test::MakeProbe;

static bool Mentions(std::string const& message, std::string_view text)
{
	return message.find(text) != std::string::npos;
}

namespace
{

// A wrong answer must fail one test, not abort the run: tl::expected asserts
// on a bad access, and asserts are live.
template<typename T>
std::string RefusalOf(tl::expected<T, ErrorContext> const& e)
{
	return e ? std::string("(accepted)") : e.error().format();
}

relation_store StoreFrom(std::string_view yaml)
{
	auto s = SchemaFromYaml(yaml, "pick.yaml");
	EXPECT_TRUE(s.has_value()) << (s ? std::string() : s.error().format());
	return relation_store(std::move(*s));
}

// A lookup table: label (key), req (ranks by mode), clip.
constexpr std::string_view kRanked = R"(
tables:
  - name: lookup
    read_only: true
    pick: best
    inputs:
      - {name: mode, type: flags}
    columns:
      - {name: label, type: int, key: true}
      - {name: req, type: flags, input: mode}
      - {name: clip, type: int}
    order_by: {columns: [label]}
)";

// Rows of (label, req, clip).
tl::expected<void, ErrorContext> LoadRanked(relation_store& store, std::vector<std::array<int64_t, 3>> const& rows,
	uint64_t owner = 1, load_refusal* refusal = nullptr)
{
	std::vector<cell> values;
	for (auto const& r : rows)
	{
		for (int64_t v : r)
		{
			values.push_back(cell::Integer(v));
		}
	}
	return store.Load(load_rows{0, owner, rows.size(), values, {}, refusal});
}

// The picked row's clip, -1 for none.
int64_t BestClip(relation_store const& store, int64_t label, int64_t mode)
{
	cell const keys[]   = {cell::Integer(label)};
	cell const inputs[] = {cell::Integer(mode)};
	auto picked = PickBest(store, 0, pick_query{keys, inputs});
	EXPECT_TRUE(picked.has_value()) << (picked ? std::string() : picked.error().format());
	if (!picked || !*picked)
	{
		return -1;
	}
	auto clip = store.Value(0, **picked, 2);
	EXPECT_TRUE(clip.has_value()) << RefusalOf(clip);
	return clip ? clip->i : -1;
}

// A guarded table: label (key), window (band over t), when (script), clip.
constexpr std::string_view kGuarded = R"(
tables:
  - name: guarded
    read_only: true
    pick: best
    inputs:
      - {name: t, type: double, range: 0..1}
    columns:
      - {name: label, type: int, key: true}
      - {name: window, type: band, input: t}
      - {name: when, type: condition}
      - {name: clip, type: int}
    order_by: {columns: [label]}
)";

struct guarded_row
{
	int64_t label;
	float   lo;
	float   hi;
	closure when;
	int64_t clip;
};

tl::expected<void, ErrorContext> LoadGuarded(relation_store& store, std::vector<guarded_row> const& rows)
{
	std::vector<cell>    values;
	std::vector<closure> conditions;
	for (guarded_row const& r : rows)
	{
		values.push_back(cell::Integer(r.label));
		values.push_back(cell::Vector(r.lo, r.hi));
		values.push_back(cell::Integer(r.clip));
		conditions.push_back(r.when);
	}
	return store.Load(load_rows{0, 1, rows.size(), values, {}, nullptr, conditions});
}

tl::expected<void, ErrorContext> LoadGuardedWith(relation_store& store, std::vector<guarded_row> const& rows,
	load_refusal* refusal)
{
	std::vector<cell>    values;
	std::vector<closure> conditions;
	for (guarded_row const& r : rows)
	{
		values.push_back(cell::Integer(r.label));
		values.push_back(cell::Vector(r.lo, r.hi));
		values.push_back(cell::Integer(r.clip));
		conditions.push_back(r.when);
	}
	return store.Load(load_rows{0, 1, rows.size(), values, {}, refusal, conditions});
}

tl::expected<std::optional<size_t>, ErrorContext> PickGuarded(relation_store const& store, int64_t label, double t,
	int64_t this_in)
{
	cell const keys[]   = {cell::Integer(label)};
	cell const inputs[] = {cell::Real(t)};
	return PickBest(store, 0, pick_query{keys, inputs, &this_in});
}

int64_t GuardedClip(relation_store const& store, int64_t label, double t, int64_t this_in)
{
	auto picked = PickGuarded(store, label, t, this_in);
	EXPECT_TRUE(picked.has_value()) << (picked ? std::string() : picked.error().format());
	if (!picked || !*picked)
	{
		return -1;
	}
	auto clip = store.Value(0, **picked, 3);
	EXPECT_TRUE(clip.has_value()) << RefusalOf(clip);
	return clip ? clip->i : -1;
}

bool CountCall(void* data, void const*, ErrorContext*)
{
	++*static_cast<int*>(data);
	return true;
}

closure Counting(int* calls)
{
	return closure{&CountCall, calls, nullptr};
}

}

TEST(PickBest, WithoutRankingTakesTheFirstRowThatHolds)
{
	relation_store store = StoreFrom(kGuarded);
	ASSERT_TRUE(LoadGuarded(store, {{1, 0.0f, 0.5f, closure{}, 10},
	                                {1, 0.5f, 1.0f, closure{}, 11}}).has_value());
	EXPECT_EQ(GuardedClip(store, 1, 0.25, 0), 10);
	EXPECT_EQ(GuardedClip(store, 1, 0.75, 0), 11);
	EXPECT_EQ(GuardedClip(store, 2, 0.25, 0), -1) << "no rows for the key";
}

TEST(PickBest, FallsThroughARowWhoseScriptConditionFails)
{
	int released = 0;
	relation_store store = StoreFrom(kGuarded);
	ASSERT_TRUE(LoadGuarded(store, {{1, 0.0f, 1.0f, MakeProbe(5, &released), 10},
	                                {1, 0.0f, 1.0f, closure{}, 11}}).has_value());
	EXPECT_EQ(GuardedClip(store, 1, 0.5, 7), 10);
	EXPECT_EQ(GuardedClip(store, 1, 0.5, 2), 11);
}

TEST(PickBest, BandsAreTestedBeforeTheScript)
{
	int calls = 0;
	relation_store store = StoreFrom(kGuarded);
	ASSERT_TRUE(LoadGuarded(store, {{1, 0.0f, 0.5f, Counting(&calls), 10},
	                                {1, 0.5f, 1.0f, closure{}, 11},
	                                {1, 0.0f, 0.5f, closure{}, 12}}).has_value());
	EXPECT_EQ(GuardedClip(store, 1, 0.75, 0), 11);
	EXPECT_EQ(calls, 0) << "a script ran for a row whose band does not hold";
}

TEST(PickBest, AFailingScriptConditionFailsThePick)
{
	int released = 0;
	relation_store store = StoreFrom(kGuarded);
	ASSERT_TRUE(LoadGuarded(store, {{1, 0.0f, 1.0f, MakeProbe(0, &released), 10},
	                                {1, 0.0f, 1.0f, closure{}, 11}}).has_value());
	auto picked = PickGuarded(store, 1, 0.5, -1);
	ASSERT_FALSE(picked.has_value());
	EXPECT_TRUE(Mentions(picked.error().format(), "negative input")) << picked.error().format();
	EXPECT_TRUE(Mentions(picked.error().format(), "row 0")) << picked.error().format();
}

// animation's bit_value lookup fixture (tests/fixtures/animation_pipeline/bit_value),
// long-form: headers None=0, Injured=1, Crouch=2, Injured|Crouch=3, Prone=4,
// Crouch|Prone=6; clip = case * 10 + the header's position. The
// expected cells are animation's LookupBest.BitValueRuling.
TEST(PickBest, RanksLikeAnimationsLookupBest)
{
	relation_store store = StoreFrom(kRanked);
	std::vector<std::array<int64_t, 3>> rows = {
		{1, 0, 10}, {1, 1, 11}, {1, 2, 12},
		{2, 0, 20}, {2, 1, 21}, {2, 2, 22}, {2, 3, 23},
		{3, 0, 30}, {3, 3, 33}, {3, 4, 34},
		{4, 0, 40}, {4, 2, 42}, {4, 3, 43},
		{5, 0, 50}, {5, 3, 53}, {5, 6, 55},
		{6, 0, 60}, {6, 3, 63},
	};
	ASSERT_TRUE(LoadRanked(store, rows).has_value());
	int64_t const injured = 1, crouch = 2, prone = 4;
	EXPECT_EQ(BestClip(store, 1, injured | crouch), 12);
	EXPECT_EQ(BestClip(store, 2, injured | crouch), 23);
	EXPECT_EQ(BestClip(store, 3, injured | crouch | prone), 34);
	EXPECT_EQ(BestClip(store, 4, injured), 43) << "a partial match beats the default; crouch never matches";
	EXPECT_EQ(BestClip(store, 5, crouch), 55) << "equal partial hits go to the larger R";
	EXPECT_EQ(BestClip(store, 6, prone), 60) << "nothing matches: the default";
}

// The corpus above never sets a full match against a larger partial one, so
// without this the "all flags in the input" rule could be dropped unseen.
TEST(PickBest, AFullMatchBeatsALargerPartialOne)
{
	relation_store store = StoreFrom(kRanked);
	ASSERT_TRUE(LoadRanked(store, {{1, 0, 10}, {1, 1, 11}, {1, 14, 12}}).has_value());
	EXPECT_EQ(BestClip(store, 1, 7), 11) << "R=1 is all in F=7; R=14 hits two of its three bits but needs bit 3";
	EXPECT_EQ(BestClip(store, 1, 15), 12) << "with bit 3 present R=14 is a full match with the larger hit";
}

TEST(PickBest, AKeyRunWithoutADefaultIsRefusedAtLoad)
{
	relation_store store = StoreFrom(kRanked);
	load_refusal refusal;
	auto loaded = LoadRanked(store, {{1, 0, 10}, {7, 1, 71}, {7, 2, 72}}, 1, &refusal);
	ASSERT_FALSE(loaded.has_value());
	EXPECT_TRUE(Mentions(loaded.error().format(), "no row with no flags")) << loaded.error().format();
	EXPECT_EQ(refusal.row, 1u) << "the run's first loaded row";
	EXPECT_EQ(store.RowCount(0), 0u) << "a refused load changes nothing";
}

TEST(PickBest, RetractingTheDefaultsOwnerIsNeverRefused)
{
	relation_store store = StoreFrom(kRanked);
	ASSERT_TRUE(LoadRanked(store, {{1, 0, 10}}, 1).has_value());
	ASSERT_TRUE(LoadRanked(store, {{1, 1, 11}}, 2).has_value()) << "the run has owner 1's default";
	store.Retract(retract_owner{1});
	ASSERT_EQ(store.RowCount(0), 1u);
	EXPECT_EQ(BestClip(store, 1, 1), 11);
	EXPECT_EQ(BestClip(store, 1, 2), -1) << "no ranked row matches and the default is gone";
}

TEST(PickBest, QueriesOfTheWrongShapeAreRefused)
{
	relation_store store = StoreFrom(kRanked);
	ASSERT_TRUE(LoadRanked(store, {{1, 0, 10}}).has_value());
	cell const oneKey[]   = {cell::Integer(1)};
	cell const twoKeys[]  = {cell::Integer(1), cell::Integer(2)};
	cell const oneInput[] = {cell::Integer(1)};
	EXPECT_TRUE(Mentions(RefusalOf(PickBest(store, 0, pick_query{twoKeys, oneInput})), "2 keys"));
	EXPECT_TRUE(Mentions(RefusalOf(PickBest(store, 0, pick_query{oneKey, {}})), "0 inputs"));
	for (int64_t bad : {INT64_C(-1), INT64_C(0x100000000)})
	{
		cell const input[] = {cell::Integer(bad)};
		auto picked = PickBest(store, 0, pick_query{oneKey, input});
		ASSERT_FALSE(picked.has_value()) << bad;
		EXPECT_TRUE(Mentions(picked.error().format(), "mode")) << picked.error().format();
	}
}

TEST(PickBest, AHeldVersionPicksItsOwnRows)
{
	relation_store store = StoreFrom(kRanked);
	ASSERT_TRUE(LoadRanked(store, {{1, 0, 10}}).has_value());
	auto held = store.Version(0);
	ASSERT_TRUE(held.has_value());
	ASSERT_TRUE(LoadRanked(store, {{1, 0, 20}}).has_value());
	cell const keys[]   = {cell::Integer(1)};
	cell const inputs[] = {cell::Integer(0)};
	auto old = PickBest(**held, pick_query{keys, inputs});
	ASSERT_TRUE(old.has_value() && *old);
	auto clip = (*held)->Value(**old, 2);
	ASSERT_TRUE(clip.has_value()) << RefusalOf(clip);
	EXPECT_EQ(clip->i, 10);
	EXPECT_EQ(BestClip(store, 1, 0), 20);
}

TEST(PickBest, AWritableTableCannotBePicked)
{
	relation_store store = StoreFrom(R"(
tables:
  - name: live
    columns:
      - {name: x, type: int}
)");
	auto picked = PickBest(store, 0, pick_query{});
	ASSERT_FALSE(picked.has_value());
}

namespace
{

// animation's phase CSV example (src/Animation/authoring-form-spec.md, Phase
// tables), long-form: clip 1 = grendel_walk, 2 = grendel_run; footstep = 1, pivot = 2.
constexpr std::string_view kPhase = R"(
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
)";

relation_store PhaseStore()
{
	relation_store store = StoreFrom(kPhase);
	struct phase_row { int64_t clip; int64_t bits; float lo; float hi; };
	std::vector<phase_row> const rows = {
		{1, 1, 0.0f, 0.4f},
		{2, 1, 0.0f, 0.3f}, {2, 2, 0.4f, 0.5f},
		{2, 1, 0.6f, 0.9f},
	};
	std::vector<cell> values;
	for (phase_row const& r : rows)
	{
		values.push_back(cell::Integer(r.clip));
		values.push_back(cell::Integer(r.bits));
		values.push_back(cell::Vector(r.lo, r.hi));
	}
	EXPECT_TRUE(store.Load(load_rows{0, 1, rows.size(), values}).has_value());
	return store;
}

uint32_t PhaseBits(relation_store const& store, int64_t clip, double t)
{
	cell const keys[]   = {cell::Integer(clip)};
	cell const inputs[] = {cell::Real(t)};
	auto bits = PickOr(store, 0, pick_query{keys, inputs});
	EXPECT_TRUE(bits.has_value()) << (bits ? std::string() : bits.error().format());
	return bits ? *bits : 0xFFFFFFFFu;
}

}

TEST(PickOr, MatchesAnimationsPhaseBits)
{
	relation_store store = PhaseStore();
	EXPECT_EQ(PhaseBits(store, 2, 0.1), 1u);
	EXPECT_EQ(PhaseBits(store, 2, 0.45), 2u);
	EXPECT_EQ(PhaseBits(store, 2, 0.7), 1u) << "rows sharing a key accumulate";
	EXPECT_EQ(PhaseBits(store, 2, 0.55), 0u) << "nothing holding gives 0";
	EXPECT_EQ(PhaseBits(store, 1, 0.5), 0u);
	EXPECT_EQ(PhaseBits(store, 9, 0.1), 0u) << "an unknown key holds nothing";
}

TEST(PickOr, OverlappingBandsOrTogether)
{
	relation_store store = StoreFrom(kPhase);
	cell const values[] = {
		cell::Integer(1), cell::Integer(1), cell::Vector(0.0f, 1.0f),
		cell::Integer(1), cell::Integer(4), cell::Vector(0.25f, 0.75f),
	};
	ASSERT_TRUE(store.Load(load_rows{0, 1, 2, values}).has_value());
	EXPECT_EQ(PhaseBits(store, 1, 0.5), 5u);
}

// "A band runs low to high" is a property of the cell, not of an input's range,
// so a table whose input declares no range holds to it too.
TEST(PickOr, ABandIsWellFormedWhateverTheInputsRange)
{
	relation_store store = StoreFrom(kPhase);
	float const nan = std::nanf("");
	float const inf = INFINITY;
	struct bad { float lo; float hi; };
	for (bad const& b : {bad{0.5f, 0.2f}, bad{0.5f, 0.5f}, bad{nan, 1.0f}, bad{0.0f, nan}, bad{0.0f, inf}, bad{-inf, 1.0f}})
	{
		cell const values[] = {cell::Integer(1), cell::Integer(1), cell::Vector(b.lo, b.hi)};
		load_refusal refusal;
		auto loaded = store.Load(load_rows{0, 1, 1, values, {}, &refusal});
		ASSERT_FALSE(loaded.has_value()) << b.lo << ".." << b.hi;
		EXPECT_TRUE(Mentions(loaded.error().format(), "window")) << loaded.error().format();
		EXPECT_EQ(refusal.row, 0u);
		EXPECT_EQ(store.RowCount(0), 0u);
	}
}

TEST(PickOr, ABandWrittenInCsvIsWellFormed)
{
	relation_store store = StoreFrom(kPhase);
	for (char const* band : {"0.5..0.2", "nan..1", "0..inf", "0.5..0.5"})
	{
		auto loaded = LoadCsv(store, csv_rows{0, 1, std::string("clip,bits,window\n1,1,") + band + "\n", "phase.csv"});
		ASSERT_FALSE(loaded.has_value()) << band;
		EXPECT_TRUE(Mentions(loaded.error().format(), "a band runs low to high")) << loaded.error().format();
		EXPECT_TRUE(Mentions(loaded.error().format(), "phase.csv:2")) << loaded.error().format();
	}
	EXPECT_TRUE(LoadCsv(store, csv_rows{0, 1, "clip,bits,window\n1,1,0.1..0.2\n", "phase.csv"}).has_value());
}

TEST(PickOr, EdgesAreHalfOpenAndNanHoldsNothing)
{
	relation_store store = PhaseStore();
	EXPECT_EQ(PhaseBits(store, 2, 0.0), 1u) << "t == start holds";
	EXPECT_EQ(PhaseBits(store, 2, 0.3), 0u) << "t == end does not";
	EXPECT_EQ(PhaseBits(store, 2, 0.4), 2u);
	EXPECT_EQ(PhaseBits(store, 2, std::nan("")), 0u);
}

TEST(PickAll, ReturnsEveryHoldingRowInRowOrder)
{
	int released = 0;
	relation_store store = StoreFrom(R"(
tables:
  - name: all
    read_only: true
    pick: all
    inputs:
      - {name: t, type: double}
    columns:
      - {name: label, type: int, key: true}
      - {name: window, type: band, input: t}
      - {name: when, type: condition}
      - {name: clip, type: int}
    order_by: {columns: [label]}
)");
	ASSERT_TRUE(LoadGuarded(store, {{1, 0.0f, 1.0f, MakeProbe(0, &released), 10},
	                                {1, 0.5f, 1.0f, MakeProbe(0, &released), 11},
	                                {1, 0.0f, 1.0f, MakeProbe(9, &released), 12},
	                                {1, 0.0f, 1.0f, MakeProbe(0, &released), 13}}).has_value());
	cell const keys[]   = {cell::Integer(1)};
	cell const inputs[] = {cell::Real(0.25)};
	int64_t    thisIn   = 1;
	auto rows = PickAll(store, 0, pick_query{keys, inputs, &thisIn});
	ASSERT_TRUE(rows.has_value()) << rows.error().format();
	EXPECT_EQ(*rows, (std::vector<size_t>{0, 3}));
}

// A clamp has no place for NaN, and that follows from the range declaration,
// not from the pick policy.
TEST(PickPolicy, NanIsRefusedForARangedInputInEveryPolicy)
{
	auto store = [](char const* policy)
	{
		return StoreFrom(std::string(R"(
tables:
  - name: t
    read_only: true
    pick: )") + policy + R"(
    inputs:
      - {name: t, type: double, range: 0..1}
    columns:
      - {name: label, type: int, key: true}
      - {name: window, type: band, input: t}
      - {name: out, type: flags}
    order_by: {columns: [label]}
)" + (std::string_view(policy) == "or" ? "    output: out\n" : ""));
	};
	cell const keys[]   = {cell::Integer(1)};
	cell const inputs[] = {cell::Real(std::nan(""))};
	relation_store all = store("all");
	relation_store ord = store("or");
	EXPECT_TRUE(Mentions(RefusalOf(PickAll(all, 0, pick_query{keys, inputs})), "is NaN"));
	EXPECT_TRUE(Mentions(RefusalOf(PickOr(ord, 0, pick_query{keys, inputs})), "is NaN"));
}

TEST(PickPolicy, EachTableIsPickedOneWay)
{
	relation_store phase = PhaseStore();
	cell const keys[]   = {cell::Integer(2)};
	cell const inputs[] = {cell::Real(0.1)};
	EXPECT_TRUE(Mentions(RefusalOf(PickBest(phase, 0, pick_query{keys, inputs})), "not picked with best"));
	EXPECT_TRUE(Mentions(RefusalOf(PickAll(phase, 0, pick_query{keys, inputs})), "not picked with all"));
	relation_store ranked = StoreFrom(kRanked);
	cell const mode[] = {cell::Integer(0)};
	EXPECT_TRUE(Mentions(RefusalOf(PickOr(ranked, 0, pick_query{keys, mode})), "not picked with or"));
}

namespace
{

// Two keys, and an order that runs on past them: a key run is (a, b), not
// (a, b, req) and not (a).
constexpr std::string_view kTwoKeys = R"(
tables:
  - name: lookup
    read_only: true
    pick: best
    inputs:
      - {name: mode, type: flags}
    columns:
      - {name: a, type: int, key: true}
      - {name: b, type: int, key: true}
      - {name: req, type: flags, input: mode}
      - {name: clip, type: int}
    order_by: {columns: [a, b, req]}
)";

tl::expected<void, ErrorContext> LoadTwoKeys(relation_store& store, std::vector<std::array<int64_t, 4>> const& rows,
	load_refusal* refusal = nullptr)
{
	std::vector<cell> values;
	for (auto const& r : rows)
	{
		for (int64_t v : r)
		{
			values.push_back(cell::Integer(v));
		}
	}
	return store.Load(load_rows{0, 1, rows.size(), values, {}, refusal});
}

int64_t TwoKeyClip(relation_store const& store, int64_t a, int64_t b, int64_t mode)
{
	cell const keys[]   = {cell::Integer(a), cell::Integer(b)};
	cell const inputs[] = {cell::Integer(mode)};
	auto picked = PickBest(store, 0, pick_query{keys, inputs});
	EXPECT_TRUE(picked.has_value()) << RefusalOf(picked);
	if (!picked || !*picked)
	{
		return -1;
	}
	auto clip = store.Value(0, **picked, 3);
	EXPECT_TRUE(clip.has_value()) << RefusalOf(clip);
	return clip ? clip->i : -1;
}

}

TEST(PickBest, AKeyRunIsEveryKeyColumnAndNoMore)
{
	relation_store store = StoreFrom(kTwoKeys);
	// Ordered by req too, so the default sorts first in each run, and (1, 1)'s
	// ranked rows are each their own (a, b, req) group.
	ASSERT_EQ(RefusalOf(LoadTwoKeys(store, {{1, 1, 0, 110}, {1, 1, 1, 111}, {1, 1, 2, 112},
	                                        {1, 2, 0, 120}, {1, 2, 1, 121}})), "(accepted)");
	EXPECT_EQ(TwoKeyClip(store, 1, 1, 2), 112);
	EXPECT_EQ(TwoKeyClip(store, 1, 2, 2), 120) << "only (1, 2)'s rows: (1, 1)'s req 2 row is not in the run";
	EXPECT_EQ(TwoKeyClip(store, 1, 3, 2), -1);

	// (1, 2) without a default, though (1, 1) beside it has one.
	load_refusal refusal;
	auto loaded = LoadTwoKeys(store, {{1, 1, 0, 110}, {1, 2, 1, 121}, {1, 2, 2, 122}}, &refusal);
	ASSERT_FALSE(loaded.has_value());
	EXPECT_TRUE(Mentions(loaded.error().format(), "key (1, 2)")) << loaded.error().format();
	EXPECT_EQ(refusal.row, 1u) << "the run's first loaded row";
}

TEST(PickSchema, TwoKeysLeadTheOrderInColumnOrder)
{
	auto s = SchemaFromYaml(R"(
tables:
  - name: lookup
    read_only: true
    pick: best
    columns:
      - {name: a, type: int, key: true}
      - {name: b, type: int, key: true}
      - {name: clip, type: int}
    order_by: {columns: [b, a]}
)", "two.yaml");
	ASSERT_FALSE(s.has_value());
	EXPECT_TRUE(Mentions(s.error().format(), "starts with its keys")) << s.error().format();
}

TEST(PickBest, ARangedInputIsClampedAndNanIsRefused)
{
	relation_store store = StoreFrom(kGuarded);
	ASSERT_TRUE(LoadGuarded(store, {{1, 0.0f, 0.5f, closure{}, 10}, {1, 0.5f, 1.0f, closure{}, 11}}).has_value());
	EXPECT_EQ(GuardedClip(store, 1, -3.0, 0), 10) << "below lo reads as lo";
	EXPECT_EQ(GuardedClip(store, 1, 1.0, 0), 11) << "hi reads as the largest float below it";
	EXPECT_EQ(GuardedClip(store, 1, 7.0, 0), 11);
	EXPECT_EQ(GuardedClip(store, 1, 1e300, 0), 11) << "past float's range, clamped in double";
	EXPECT_TRUE(Mentions(RefusalOf(PickGuarded(store, 1, std::nan(""), 0)), "is NaN"))
		<< "a total table never answers nothing; NaN is refused instead";
}

TEST(PickBest, ABandOutsideItsInputsRangeIsRefused)
{
	relation_store store = StoreFrom(kGuarded);
	load_refusal refusal;
	std::vector<cell> values = {cell::Integer(1), cell::Vector(0.0f, 1.0f), cell::Integer(10),
	                            cell::Integer(1), cell::Vector(0.5f, 1.5f), cell::Integer(11)};
	std::vector<closure> conditions = {closure{}, closure{}};
	auto loaded = store.Load(load_rows{0, 1, 2, values, {}, &refusal, conditions});
	ASSERT_FALSE(loaded.has_value());
	EXPECT_TRUE(Mentions(loaded.error().format(), "0.5..1.5")) << loaded.error().format();
	EXPECT_TRUE(Mentions(loaded.error().format(), "range 0..1")) << loaded.error().format();
	EXPECT_EQ(refusal.row, 1u);

	std::vector<cell> backwards = {cell::Integer(1), cell::Vector(0.5f, 0.25f), cell::Integer(10)};
	std::vector<closure> one = {closure{}};
	EXPECT_FALSE(store.Load(load_rows{0, 1, 1, backwards, {}, nullptr, one}).has_value());
}

namespace
{

// label (key), req (ranks by mode), window (band over t in 0..1), when (script), clip.
constexpr std::string_view kRankedBand = R"(
tables:
  - name: lookup
    read_only: true
    pick: best
    inputs:
      - {name: mode, type: flags}
      - {name: t, type: double, range: 0..1}
    columns:
      - {name: label, type: int, key: true}
      - {name: req, type: flags, input: mode}
      - {name: window, type: band, input: t}
      - {name: when, type: condition}
      - {name: clip, type: int}
    order_by: {columns: [label]}
)";

struct band_row
{
	int64_t  label;
	int64_t  req;
	float    lo;
	float    hi;
	closure  when;
	int64_t  clip;
};

tl::expected<void, ErrorContext> LoadBand(relation_store& store, std::vector<band_row> const& rows,
	load_refusal* refusal = nullptr, uint64_t owner = 1)
{
	std::vector<cell>     values;
	std::vector<closure>  conditions;
	for (band_row const& r : rows)
	{
		values.push_back(cell::Integer(r.label));
		values.push_back(cell::Integer(r.req));
		values.push_back(cell::Vector(r.lo, r.hi));
		values.push_back(cell::Integer(r.clip));
		conditions.push_back(r.when);
	}
	return store.Load(load_rows{0, owner, rows.size(), values, {}, refusal, conditions});
}

int64_t BandClip(relation_store const& store, int64_t label, int64_t mode, double t)
{
	cell const keys[]   = {cell::Integer(label)};
	cell const inputs[] = {cell::Integer(mode), cell::Real(t)};
	int64_t    thisIn   = 0;
	auto picked = PickBest(store, 0, pick_query{keys, inputs, &thisIn});
	EXPECT_TRUE(picked.has_value()) << RefusalOf(picked);
	if (!picked || !*picked)
	{
		return -1;
	}
	auto clip = store.Value(0, **picked, 4);
	EXPECT_TRUE(clip.has_value()) << RefusalOf(clip);
	return clip ? clip->i : -1;
}

closure Never()
{
	return closure{[](void*, void const*, ErrorContext*) { return false; }, nullptr, nullptr};
}

}

TEST(PickBest, TheReactionTableLoads)
{
	relation_store store = StoreFrom(kGuarded);
	ASSERT_EQ(RefusalOf(LoadGuarded(store, {{1, 0.0f, 0.2f, closure{}, 10},
	                                        {1, 0.2f, 1.0f, Never(), 11},
	                                        {1, 0.2f, 1.0f, closure{}, 12}})), "(accepted)");
	EXPECT_EQ(GuardedClip(store, 1, 0.5, 0), 12);
}

TEST(PickBest, AGapInTheQuiescentRowsIsRefusedNamingAValue)
{
	relation_store store = StoreFrom(kGuarded);
	load_refusal refusal;
	std::vector<cell> values = {cell::Integer(1), cell::Vector(0.0f, 0.2f), cell::Integer(10),
	                            cell::Integer(1), cell::Vector(0.2f, 1.0f), cell::Integer(11)};
	std::vector<closure> conditions = {closure{}, Never()};
	auto loaded = store.Load(load_rows{0, 1, 2, values, {}, &refusal, conditions});
	ASSERT_FALSE(loaded.has_value());
	std::string const error = loaded.error().format();
	EXPECT_TRUE(Mentions(error, "key (1): t 0.2 has no row with no script condition")) << error;
	EXPECT_EQ(refusal.row, 0u) << "the run's first loaded row";
	EXPECT_EQ(store.RowCount(0), 0u);
}

TEST(PickBest, ADefaultWithAScriptConditionIsNotADefault)
{
	relation_store store = StoreFrom(kRankedBand);
	load_refusal refusal;
	auto loaded = LoadBand(store, {{1, 0, 0.0f, 1.0f, Never(), 10}, {1, 1, 0.0f, 1.0f, closure{}, 11}}, &refusal);
	ASSERT_FALSE(loaded.has_value());
	EXPECT_TRUE(Mentions(loaded.error().format(), "no row with no flags and no script condition"))
		<< loaded.error().format();
}

TEST(PickBest, AnUnrankedTableOfScriptedRowsIsRefused)
{
	auto s = SchemaFromYaml(R"(
tables:
  - name: rules
    read_only: true
    pick: best
    columns:
      - {name: label, type: int, key: true}
      - {name: when, type: condition}
      - {name: clip, type: int}
    order_by: {columns: [label]}
)", "rules.yaml");
	ASSERT_TRUE(s.has_value()) << s.error().format();
	relation_store store(std::move(*s));
	cell const values[] = {cell::Integer(1), cell::Integer(10)};
	closure const scripted[] = {Never()};
	auto loaded = store.Load(load_rows{0, 1, 1, values, {}, nullptr, scripted});
	ASSERT_FALSE(loaded.has_value());
	EXPECT_TRUE(Mentions(loaded.error().format(), "key (1) has no row with no script condition"))
		<< loaded.error().format();
	closure const plain[] = {closure{}};
	EXPECT_TRUE(store.Load(load_rows{0, 1, 1, values, {}, nullptr, plain}).has_value());
}

// A reload replaces everything its owner had, so it can take a run's only
// default away from another owner's ranked row.
TEST(PickBest, AReloadThatStrandsARunIsRefused)
{
	relation_store store = StoreFrom(kRankedBand);
	ASSERT_EQ(RefusalOf(LoadBand(store, {{1, 0, 0.0f, 1.0f, closure{}, 10}}, nullptr, 1)), "(accepted)");
	ASSERT_EQ(RefusalOf(LoadBand(store, {{1, 1, 0.0f, 1.0f, closure{}, 11}}, nullptr, 2)), "(accepted)");
	load_refusal refusal;
	std::string const refused = RefusalOf(LoadBand(store, {{2, 0, 0.0f, 1.0f, closure{}, 20}}, &refusal, 1));
	EXPECT_TRUE(Mentions(refused, "key (1)")) << refused;
	EXPECT_TRUE(Mentions(refused, "no row with no flags")) << refused;
	EXPECT_EQ(store.RowCount(0), 2u) << "the old version stays";
	EXPECT_EQ(BandClip(store, 1, 0, 0.5), 10);
	EXPECT_EQ(RefusalOf(LoadBand(store, {{1, 0, 0.0f, 1.0f, closure{}, 12}}, nullptr, 1)), "(accepted)")
		<< "a reload that keeps the run's default is fine";
	EXPECT_EQ(BandClip(store, 1, 0, 0.5), 12);
}

// A published version is total: a load that brings rows checks every run of
// the version it would publish, not only the runs it touches.
TEST(PickBest, ALoadChecksARunItNeitherAddsToNorLeaves)
{
	relation_store store = StoreFrom(kRankedBand);
	ASSERT_EQ(RefusalOf(LoadBand(store, {{1, 0, 0.0f, 1.0f, closure{}, 10}}, nullptr, 1)), "(accepted)");
	ASSERT_EQ(RefusalOf(LoadBand(store, {{1, 1, 0.0f, 1.0f, closure{}, 11}}, nullptr, 3)), "(accepted)");
	store.Retract(retract_owner{1});   // a bare retract is never refused, and strands run 1
	EXPECT_EQ(BandClip(store, 1, 0, 0.5), -1) << "run 1 has no default now, and picks nothing";
	size_t const rows = store.RowCount(0);
	load_refusal refusal;
	std::string const refused = RefusalOf(LoadBand(store, {{2, 0, 0.0f, 1.0f, closure{}, 20}}, &refusal, 2));
	EXPECT_TRUE(Mentions(refused, "key (1)")) << refused;
	EXPECT_TRUE(Mentions(refused, "no row with no flags and no script condition")) << refused;
	EXPECT_TRUE(Mentions(refused, "owner 3's row with key (1), flags 1, window 0..1")) << refused;
	EXPECT_EQ(refusal.row, SIZE_MAX) << "no loaded row is in run 1, so no CSV line is named";
	EXPECT_EQ(store.RowCount(0), rows);
	store.Retract(retract_owner{3});   // and a bare retract of the stranded rows is still never refused
	EXPECT_EQ(RefusalOf(LoadBand(store, {{2, 0, 0.0f, 1.0f, closure{}, 20}}, nullptr, 2)), "(accepted)");
}

// The two-step path (retract, then load) must agree with the one-step reload.
TEST(PickBest, RetractThenLoadAgreesWithAReload)
{
	relation_store store = StoreFrom(kRankedBand);
	ASSERT_EQ(RefusalOf(LoadBand(store, {{1, 0, 0.0f, 1.0f, closure{}, 10}}, nullptr, 1)), "(accepted)");
	ASSERT_EQ(RefusalOf(LoadBand(store, {{1, 1, 0.0f, 1.0f, closure{}, 11}}, nullptr, 2)), "(accepted)");
	store.Retract(retract_owner{1});
	std::string const refused = RefusalOf(LoadBand(store, {{2, 0, 0.0f, 1.0f, closure{}, 20}}, nullptr, 1));
	EXPECT_TRUE(Mentions(refused, "key (1)")) << refused;
	EXPECT_TRUE(Mentions(refused, "no row with no flags")) << refused;
	EXPECT_EQ(RefusalOf(LoadBand(store, {{1, 0, 0.0f, 1.0f, closure{}, 12}}, nullptr, 1)), "(accepted)");
}

// Two owners' rows with equal flags may not overlap in one run: they would be
// two contributors claiming the same inputs, decided by load order.
TEST(PickBest, EqualFlagsOverlapBetweenOwnersIsRefused)
{
	relation_store store = StoreFrom(kRankedBand);
	ASSERT_EQ(RefusalOf(LoadBand(store, {{1, 0, 0.0f, 1.0f, closure{}, 10}}, nullptr, 1)), "(accepted)");
	ASSERT_EQ(RefusalOf(LoadBand(store, {{1, 1, 0.0f, 0.5f, closure{}, 11}}, nullptr, 2)), "(accepted)");
	load_refusal refusal;
	std::string const refused = RefusalOf(LoadBand(store, {{1, 1, 0.0f, 1.0f, closure{}, 12}}, &refusal, 3));
	EXPECT_TRUE(Mentions(refused, "key (1)")) << refused;
	EXPECT_TRUE(Mentions(refused, "owner 2's row with key (1), flags 1, window 0..0.5")) << refused;
	EXPECT_TRUE(Mentions(refused, "owner 3's loaded row 0")) << refused;
	EXPECT_TRUE(Mentions(refused, "overlap")) << refused;
	EXPECT_EQ(refusal.row, 0u) << "the loaded row, so a CSV front end names its line";
	EXPECT_EQ(store.RowCount(0), 2u);

	EXPECT_EQ(RefusalOf(LoadBand(store, {{1, 1, 0.5f, 1.0f, closure{}, 12}}, nullptr, 3)), "(accepted)")
		<< "bands that only touch do not overlap";
	EXPECT_EQ(BandClip(store, 1, 1, 0.25), 11);
	EXPECT_EQ(BandClip(store, 1, 1, 0.75), 12);
	EXPECT_EQ(RefusalOf(LoadBand(store, {{1, 1, 0.0f, 0.5f, closure{}, 11}}, nullptr, 2)), "(accepted)")
		<< "an identical reload is idempotent";
	EXPECT_EQ(BandClip(store, 1, 1, 0.25), 11);
	EXPECT_EQ(BandClip(store, 1, 1, 0.75), 12);
	EXPECT_EQ(BandClip(store, 1, 0, 0.25), 10);
}

TEST(PickBest, EqualFlagsOverlapWithinOneOwnerIsPriority)
{
	relation_store store = StoreFrom(kRankedBand);
	ASSERT_EQ(RefusalOf(LoadBand(store, {{1, 0, 0.0f, 1.0f, closure{}, 10},
	                                     {1, 1, 0.0f, 0.6f, closure{}, 11},
	                                     {1, 1, 0.4f, 1.0f, closure{}, 12}}, nullptr, 1)), "(accepted)");
	EXPECT_EQ(BandClip(store, 1, 1, 0.5), 11);
	EXPECT_EQ(BandClip(store, 1, 1, 0.7), 12);
}

TEST(PickBest, DifferentFlagsOverlapBetweenOwnersIsLegal)
{
	relation_store store = StoreFrom(kRankedBand);
	ASSERT_EQ(RefusalOf(LoadBand(store, {{1, 0, 0.0f, 1.0f, closure{}, 10}}, nullptr, 1)), "(accepted)");
	ASSERT_EQ(RefusalOf(LoadBand(store, {{1, 1, 0.0f, 1.0f, closure{}, 11}}, nullptr, 2)), "(accepted)");
	ASSERT_EQ(RefusalOf(LoadBand(store, {{1, 2, 0.0f, 1.0f, closure{}, 12}}, nullptr, 3)), "(accepted)");
	EXPECT_EQ(BandClip(store, 1, 1, 0.5), 11);
	EXPECT_EQ(BandClip(store, 1, 2, 0.5), 12);
}

TEST(PickBest, AnUnrankedTableRefusesOverlapBetweenOwners)
{
	relation_store store = StoreFrom(kGuarded);
	auto load = [&](float lo, float hi, uint64_t owner)
	{
		cell const values[] = {cell::Integer(1), cell::Vector(lo, hi), cell::Integer(static_cast<int64_t>(owner))};
		closure const plain[] = {closure{}};
		return store.Load(load_rows{0, owner, 1, values, {}, nullptr, plain});
	};
	ASSERT_EQ(RefusalOf(load(0.0f, 1.0f, 1)), "(accepted)");
	std::string const refused = RefusalOf(load(0.0f, 0.5f, 2));
	EXPECT_TRUE(Mentions(refused, "overlap")) << refused;
	EXPECT_FALSE(Mentions(refused, "equal flags")) << "an unranked table has no flags to be equal: " << refused;
	EXPECT_TRUE(Mentions(refused, "owner 1's row with key (1), window 0..1")) << refused;
	EXPECT_TRUE(Mentions(refused, "owner 2's loaded row 0")) << refused;
}

// Scripted rows count too: a script condition does not make two owners' claims
// on the same inputs any less a clash.
TEST(PickBest, ScriptedRowsOfDifferentOwnersWithEqualFlagsAreRefused)
{
	relation_store store = StoreFrom(kRankedBand);
	ASSERT_EQ(RefusalOf(LoadBand(store, {{1, 0, 0.0f, 1.0f, closure{}, 10},
	                                     {1, 1, 0.0f, 0.5f, Never(), 11}}, nullptr, 1)), "(accepted)");
	load_refusal refusal;
	std::string const refused = RefusalOf(LoadBand(store, {{1, 1, 0.25f, 1.0f, Never(), 12}}, &refusal, 2));
	EXPECT_TRUE(Mentions(refused, "have equal flags and overlap")) << refused;
	EXPECT_TRUE(Mentions(refused, "owner 1's row with key (1), flags 1, window 0..0.5")) << refused;
	EXPECT_EQ(refusal.row, 0u);
	EXPECT_EQ(store.RowCount(0), 2u);
	EXPECT_EQ(RefusalOf(LoadBand(store, {{1, 1, 0.5f, 1.0f, Never(), 12}}, nullptr, 2)), "(accepted)")
		<< "scripted bands that only touch do not overlap";
}

// With no band input every row meets every other row of its run.
TEST(PickBest, WithNoBandInputsAnyTwoOwnersRowsOfEqualFlagsClash)
{
	relation_store store = StoreFrom(kRanked);
	ASSERT_EQ(RefusalOf(LoadRanked(store, {{1, 0, 10}, {1, 1, 11}}, 1)), "(accepted)");
	load_refusal refusal;
	std::string const refused = RefusalOf(LoadRanked(store, {{1, 1, 12}}, 2, &refusal));
	EXPECT_TRUE(Mentions(refused, "have equal flags and overlap")) << refused;
	EXPECT_TRUE(Mentions(refused, "owner 1's row with key (1), flags 1")) << refused;
	EXPECT_EQ(refusal.row, 0u);
	EXPECT_EQ(RefusalOf(LoadRanked(store, {{1, 2, 12}}, 2)), "(accepted)") << "other flags, no clash";
	EXPECT_EQ(RefusalOf(LoadRanked(store, {{2, 0, 20}}, 2)), "(accepted)") << "another key is another run";
}

// Rows of equal rank are tried in row order, whichever front end loaded them:
// nothing but the rows' own order breaks a tie.
TEST(PickBest, EqualRanksGoToRowOrder)
{
	relation_store store = StoreFrom(kRankedBand);
	ASSERT_EQ(RefusalOf(LoadBand(store, {{1, 0, 0.0f, 1.0f, closure{}, 10},
	                                     {1, 1, 0.0f, 0.5f, closure{}, 11},
	                                     {1, 1, 0.25f, 1.0f, closure{}, 12},
	                                     {2, 0, 0.0f, 1.0f, closure{}, 20},
	                                     {2, 1, 0.25f, 1.0f, closure{}, 21},
	                                     {2, 1, 0.0f, 0.5f, closure{}, 22}})), "(accepted)");
	EXPECT_EQ(BandClip(store, 1, 1, 0.3), 11) << "both hold at 0.3: the earlier row";
	EXPECT_EQ(BandClip(store, 1, 1, 0.75), 12);
	EXPECT_EQ(BandClip(store, 2, 1, 0.3), 21) << "the same bands in the other order: the other winner";
	EXPECT_EQ(BandClip(store, 2, 1, 0.1), 22);
}

TEST(PickBest, ARowEarlierRowsCoverIsRefusedNamingThem)
{
	relation_store store = StoreFrom(kGuarded);
	load_refusal refusal;
	auto loaded = LoadGuardedWith(store, {{1, 0.0f, 0.6f, closure{}, 10},
	                                      {1, 0.4f, 1.0f, closure{}, 11},
	                                      {1, 0.2f, 0.8f, closure{}, 12}}, &refusal);
	ASSERT_FALSE(loaded.has_value());
	std::string const error = loaded.error().format();
	EXPECT_TRUE(Mentions(error, "loaded row 2 is never picked")) << error;
	EXPECT_TRUE(Mentions(error, "loaded rows 0, 1 cover it")) << error;
	EXPECT_EQ(refusal.row, 2u);
	EXPECT_EQ(refusal.covered_by, 1u);
}

TEST(PickBest, AScriptedEarlierRowShadowsNothing)
{
	relation_store store = StoreFrom(kGuarded);
	EXPECT_EQ(RefusalOf(LoadGuarded(store, {{1, 0.0f, 1.0f, Never(), 10},
	                                        {1, 0.0f, 1.0f, closure{}, 11}})), "(accepted)");
}

TEST(PickBest, APartialOverlapIsPriorityNotDeath)
{
	relation_store store = StoreFrom(kGuarded);
	ASSERT_EQ(RefusalOf(LoadGuarded(store, {{1, 0.0f, 0.6f, closure{}, 10},
	                                        {1, 0.4f, 1.0f, closure{}, 11}})), "(accepted)");
	EXPECT_EQ(GuardedClip(store, 1, 0.5, 0), 10);
	EXPECT_EQ(GuardedClip(store, 1, 0.7, 0), 11);
}

TEST(PickBest, DifferentFlagsNeverShadow)
{
	relation_store store = StoreFrom(kRankedBand);
	EXPECT_EQ(RefusalOf(LoadBand(store, {{1, 0, 0.0f, 1.0f, closure{}, 10},
	                                     {1, 1, 0.0f, 1.0f, closure{}, 11},
	                                     {1, 3, 0.0f, 1.0f, closure{}, 13},
	                                     {1, 2, 0.0f, 1.0f, closure{}, 12}})), "(accepted)");
	EXPECT_EQ(BandClip(store, 1, 1, 0.5), 11);
	EXPECT_EQ(BandClip(store, 1, 3, 0.5), 13);
	EXPECT_EQ(BandClip(store, 1, 2, 0.5), 12);
}

TEST(PickBest, SameFlagsShadowInTieOrder)
{
	relation_store store = StoreFrom(kRankedBand);
	load_refusal refusal;
	auto loaded = LoadBand(store, {{1, 0, 0.0f, 1.0f, closure{}, 10},
	                               {1, 1, 0.0f, 1.0f, closure{}, 11},
	                               {1, 1, 0.0f, 0.5f, closure{}, 12}}, &refusal);
	ASSERT_FALSE(loaded.has_value()) << "row order: row 1 is tried first and covers row 2's band";
	EXPECT_TRUE(Mentions(loaded.error().format(), "loaded row 2 is never picked; loaded row 1 covers it"))
		<< loaded.error().format();
	EXPECT_EQ(refusal.row, 2u);
	EXPECT_EQ(refusal.covered_by, 1u);
}

TEST(PickBest, TwoBandsOnOneInputIntersect)
{
	relation_store store = StoreFrom(R"(
tables:
  - name: twice
    read_only: true
    pick: best
    inputs:
      - {name: t, type: double, range: 0..1}
    columns:
      - {name: label, type: int, key: true}
      - {name: a, type: band, input: t}
      - {name: b, type: band, input: t}
      - {name: clip, type: int}
    order_by: {columns: [label]}
)");
	auto load = [&](std::vector<std::array<float, 4>> const& rows)
	{
		std::vector<cell> values;
		for (auto const& r : rows)
		{
			values.push_back(cell::Integer(1));
			values.push_back(cell::Vector(r[0], r[1]));
			values.push_back(cell::Vector(r[2], r[3]));
			values.push_back(cell::Integer(0));
		}
		return store.Load(load_rows{0, 1, rows.size(), values});
	};
	EXPECT_EQ(RefusalOf(load({{0.0f, 1.0f, 0.0f, 0.6f}, {0.4f, 1.0f, 0.0f, 1.0f}})), "(accepted)")
		<< "row 0 holds on 0..0.6 only, so row 1 still owns 0.6..1";
	EXPECT_TRUE(Mentions(RefusalOf(load({{0.0f, 0.5f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.6f, 1.0f}})), "0.5"))
		<< "row 0 covers 0..0.5 and row 1 0.6..1: 0.5 has no row";
	EXPECT_TRUE(Mentions(RefusalOf(load({{0.0f, 1.0f, 0.0f, 1.0f}, {0.0f, 0.4f, 0.6f, 1.0f}})), "hold for no input"))
		<< "row 1's bands do not overlap";
}

// Review: the covering rows named are those whose boxes meet the dead row's,
// not every row tried before it.
TEST(PickBest, ADeadRowNamesOnlyTheRowsThatMeetIt)
{
	relation_store store = StoreFrom(kGuarded);
	load_refusal refusal;
	auto loaded = LoadGuardedWith(store, {{1, 0.5f, 1.0f, closure{}, 10},
	                                      {1, 0.0f, 0.5f, closure{}, 11},
	                                      {1, 0.5f, 1.0f, closure{}, 12}}, &refusal);
	ASSERT_FALSE(loaded.has_value());
	std::string const error = loaded.error().format();
	EXPECT_TRUE(Mentions(error, "loaded row 2 is never picked; loaded row 0 covers it")) << error;
	EXPECT_EQ(refusal.row, 2u);
	EXPECT_EQ(refusal.covered_by, 0u) << "row 1 is 0..0.5 and never takes row 2's place";
}

TEST(PickBest, AnOverlapWithAnOrderColumnPastTheKeysIsRefusedAsAnOverlap)
{
	relation_store store = StoreFrom(R"(
tables:
  - name: prioritised
    read_only: true
    pick: best
    inputs:
      - {name: t, type: double, range: 0..1}
    columns:
      - {name: label, type: int, key: true}
      - {name: prio, type: int}
      - {name: window, type: band, input: t}
      - {name: clip, type: int}
    order_by: {columns: [label, prio]}
)");
	auto load = [&](int64_t prio, uint64_t owner, load_refusal* refusal)
	{
		cell const values[] = {cell::Integer(1), cell::Integer(prio), cell::Vector(0.0f, 1.0f), cell::Integer(prio)};
		return store.Load(load_rows{0, owner, 1, values, {}, refusal});
	};
	ASSERT_EQ(RefusalOf(load(5, 1, nullptr)), "(accepted)");
	load_refusal refusal;
	auto loaded = load(1, 2, &refusal);
	ASSERT_FALSE(loaded.has_value()) << "prio 1 sorts first and would cover owner 1's row";
	EXPECT_TRUE(Mentions(loaded.error().format(), "owner 1's row with key (1), window 0..1")) << loaded.error().format();
	EXPECT_TRUE(Mentions(loaded.error().format(), "overlap")) << loaded.error().format();
	EXPECT_EQ(refusal.row, 0u);
}

TEST(PickBest, TwoBandInputsAreCheckedTogether)
{
	relation_store store = StoreFrom(R"(
tables:
  - name: plane
    read_only: true
    pick: best
    inputs:
      - {name: x, type: double, range: 0..1}
      - {name: y, type: double, range: 0..1}
    columns:
      - {name: label, type: int, key: true}
      - {name: bx, type: band, input: x}
      - {name: by, type: band, input: y}
      - {name: clip, type: int}
    order_by: {columns: [label]}
)");
	auto load = [&](std::vector<std::array<float, 4>> const& rows)
	{
		std::vector<cell> values;
		for (auto const& r : rows)
		{
			values.push_back(cell::Integer(1));
			values.push_back(cell::Vector(r[0], r[1]));
			values.push_back(cell::Vector(r[2], r[3]));
			values.push_back(cell::Integer(0));
		}
		return store.Load(load_rows{0, 1, rows.size(), values});
	};
	std::string const hole = RefusalOf(load({{0.0f, 0.5f, 0.0f, 1.0f}, {0.5f, 1.0f, 0.0f, 0.5f}}));
	EXPECT_TRUE(Mentions(hole, "x 0.5, y 0.5 has no row with no script condition")) << hole;
	EXPECT_EQ(RefusalOf(load({{0.0f, 0.5f, 0.0f, 1.0f}, {0.5f, 1.0f, 0.0f, 0.5f}, {0.5f, 1.0f, 0.5f, 1.0f}})),
		"(accepted)");
	cell const keys[]   = {cell::Integer(1)};
	cell const inputs[] = {cell::Real(0.75), cell::Real(0.75)};
	auto picked = PickBest(store, 0, pick_query{keys, inputs});
	ASSERT_TRUE(picked.has_value()) << RefusalOf(picked);
	EXPECT_EQ(*picked, std::optional<size_t>(2));
}
