#include <gtest/gtest.h>
#include "tabular/pick.h"
#include "tabular/relation.h"
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

// Rows of (label, req, clip), with their pivot columns if given.
tl::expected<void, ErrorContext> LoadRanked(relation_store& store, std::vector<std::array<int64_t, 3>> const& rows,
	uint64_t owner = 1, std::vector<uint16_t> const& pivot = {}, load_refusal* refusal = nullptr)
{
	std::vector<cell> values;
	for (auto const& r : rows)
	{
		for (int64_t v : r)
		{
			values.push_back(cell::Integer(v));
		}
	}
	return store.Load(load_rows{0, owner, rows.size(), values, {}, refusal, {}, {}, pivot});
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
      - {name: t, type: double}
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
	int released = 0;
	relation_store store = StoreFrom(kGuarded);
	ASSERT_TRUE(LoadGuarded(store, {{1, 0.0f, 0.5f, MakeProbe(0, &released), 10},
	                                {1, 0.5f, 1.0f, MakeProbe(0, &released), 11}}).has_value());
	EXPECT_EQ(GuardedClip(store, 1, 0.25, 0), 10);
	EXPECT_EQ(GuardedClip(store, 1, 0.75, 0), 11);
	EXPECT_EQ(GuardedClip(store, 2, 0.25, 0), -1) << "no rows for the key";
}

TEST(PickBest, FallsThroughARowWhoseScriptConditionFails)
{
	int released = 0;
	relation_store store = StoreFrom(kGuarded);
	ASSERT_TRUE(LoadGuarded(store, {{1, 0.0f, 1.0f, MakeProbe(5, &released), 10},
	                                {1, 0.0f, 1.0f, MakeProbe(0, &released), 11}}).has_value());
	EXPECT_EQ(GuardedClip(store, 1, 0.5, 7), 10);
	EXPECT_EQ(GuardedClip(store, 1, 0.5, 2), 11);
}

TEST(PickBest, BandsAreTestedBeforeTheScript)
{
	int released = 0;
	int calls    = 0;
	relation_store store = StoreFrom(kGuarded);
	ASSERT_TRUE(LoadGuarded(store, {{1, 0.0f, 0.5f, Counting(&calls), 10},
	                                {1, 0.5f, 1.0f, MakeProbe(0, &released), 11}}).has_value());
	EXPECT_EQ(GuardedClip(store, 1, 0.75, 0), 11);
	EXPECT_EQ(calls, 0) << "a script ran for a row whose band does not hold";
}

TEST(PickBest, AFailingScriptConditionFailsThePick)
{
	int released = 0;
	relation_store store = StoreFrom(kGuarded);
	ASSERT_TRUE(LoadGuarded(store, {{1, 0.0f, 1.0f, MakeProbe(0, &released), 10}}).has_value());
	auto picked = PickGuarded(store, 1, 0.5, -1);
	ASSERT_FALSE(picked.has_value());
	EXPECT_TRUE(Mentions(picked.error().format(), "negative input")) << picked.error().format();
	EXPECT_TRUE(Mentions(picked.error().format(), "row 0")) << picked.error().format();
}

// animation's bit_value lookup fixture (tests/fixtures/animation_pipeline/bit_value),
// long-form: headers None=0, Injured=1, Crouch=2, Injured|Crouch=3, Prone=4,
// Crouch|Prone=6 at pivot columns 0..5; clip = case * 10 + pivot column. The
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
	std::vector<uint16_t> pivot;
	for (auto const& r : rows)
	{
		pivot.push_back(static_cast<uint16_t>(r[2] % 10));
	}
	ASSERT_TRUE(LoadRanked(store, rows, 1, pivot).has_value());
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

TEST(PickBest, EqualRanksGoToPivotColumnThenRowOrder)
{
	relation_store store = StoreFrom(kRanked);
	ASSERT_TRUE(LoadRanked(store, {{1, 0, 10}, {1, 1, 11}, {1, 1, 12}, {2, 0, 20}, {2, 1, 21}, {2, 1, 22}}, 1,
		{0, 2, 1, 0, 0, 0}).has_value());
	EXPECT_EQ(BestClip(store, 1, 1), 12) << "the lower pivot column wins";
	EXPECT_EQ(BestClip(store, 2, 1), 21) << "equal pivot columns: row order";
}

TEST(PickBest, AKeyRunWithoutADefaultIsRefusedAtLoad)
{
	relation_store store = StoreFrom(kRanked);
	load_refusal refusal;
	auto loaded = LoadRanked(store, {{1, 0, 10}, {7, 1, 71}, {7, 2, 72}}, 1, {}, &refusal);
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
