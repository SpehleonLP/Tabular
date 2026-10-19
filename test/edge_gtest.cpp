#include <gtest/gtest.h>
#include "Support/allocationtestingsupport.h"
#include "tabular/instance.h"
#include "tabular/machine.h"
#include "tabular/pick.h"
#include "tabular/relation.h"
#include "tabular/rows_csv.h"
#include "tabular/schema_yaml.h"
#include "cityhash/src/city.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace tabular;

// Edges (P13): a band column marked edge holds when its input crossed into
// the band between the previous value and this one, so a short band is never
// skipped. An edge row is a conditional row for the proof, as a scripted one
// is; the instance keeps every input's previous value; the interpreted picks
// take the previous values beside the inputs and are the step's oracle.

namespace
{

template<typename T>
std::string RefusalOf(tl::expected<T, ErrorContext> const& e)
{
	return e ? std::string("(accepted)") : e.error().format();
}

bool Mentions(std::string const& message, std::string_view text)
{
	return message.find(text) != std::string::npos;
}

int64_t Hash(std::string_view text)
{
	return static_cast<int64_t>(CityHash32(text.data(), text.size()));
}

double const kNaN = std::numeric_limits<double>::quiet_NaN();

// One best table: label (key), zone (an edge over t), clip.
constexpr std::string_view kZone = R"(
tables:
  - name: zone
    read_only: true
    pick: best
    inputs:
      - {name: t, type: double, range: 0..1}
    columns:
      - {name: label, type: int, key: true, domain: rows}
      - {name: zone,  type: band, input: t, edge: true}
      - {name: clip,  type: int}
    order_by: {columns: [label]}
)";

relation_store StoreFrom(std::string_view yaml)
{
	auto s = SchemaFromYaml(yaml, "edge.yaml");
	EXPECT_TRUE(s.has_value()) << RefusalOf(s);
	return relation_store(s ? std::move(*s) : schema{});
}

tl::expected<void, ErrorContext> LoadText(relation_store& store, uint32_t table, std::string_view text, uint64_t owner = 1,
	bool pivoted = false)
{
	std::vector<std::string> names;
	csv_rows load;
	load.table      = table;
	load.owner      = owner;
	load.text       = text;
	load.label      = "rows.csv";
	load.pivoted    = pivoted;
	load.flag_names = pivoted ? &names : nullptr;
	return LoadCsv(store, load);
}

// One row of table 0 through the builder, owner 1.
tl::expected<void, ErrorContext> LoadOne(relation_store& store, std::span<cell const> values)
{
	load_rows load;
	load.table  = 0;
	load.owner  = 1;
	load.count  = 1;
	load.values = values;
	return store.Load(load);
}

// The clip PickBest answers for (previous, now), -1 for none.
int64_t ZoneClip(relation_store const& store, double previous, double now)
{
	cell const keys[]     = {cell::Integer(1)};
	cell const inputs[]   = {cell::Real(now)};
	cell const previous_[] = {cell::Real(previous)};
	auto picked = PickBest(store, 0, pick_query{keys, inputs, nullptr, previous_});
	EXPECT_TRUE(picked.has_value()) << RefusalOf(picked);
	if (!picked || !*picked)
	{
		return -1;
	}
	auto clip = store.Value(0, **picked, 2);
	return clip ? clip->i : -1;
}

}

// ---- Declaration ------------------------------------------------------------------

TEST(EdgeSchema, AnEdgeIsABandColumnAndNoOtherKind)
{
	auto accepted = SchemaFromYaml(kZone, "edge.yaml");
	ASSERT_TRUE(accepted.has_value()) << RefusalOf(accepted);
	table_def const& def = accepted->tables()[0];
	EXPECT_TRUE(def.columns[1].edge);
	EXPECT_FALSE(def.columns[2].edge);
	EXPECT_TRUE(def.edges);

	auto onInt = SchemaFromYaml(R"(
tables:
  - name: zone
    read_only: true
    pick: best
    inputs:
      - {name: t, type: double, range: 0..1}
    columns:
      - {name: label, type: int, key: true, domain: rows}
      - {name: zone,  type: band, input: t}
      - {name: clip,  type: int, edge: true}
    order_by: {columns: [label]}
)", "edge.yaml");
	EXPECT_TRUE(Mentions(RefusalOf(onInt), "column 'clip' of table 'zone' is an int marked edge; only a band is an edge"))
		<< RefusalOf(onInt);

	// The builder refuses it the same way: it is the one place the rule lives.
	add_column const columns[] = {{"flag", column_type::Flags, false, UINT32_MAX, {}, true}};
	create_table table;
	table.name    = "t";
	table.columns = columns;
	EXPECT_TRUE(Mentions(RefusalOf(BuildSchema(std::span<create_table const>(&table, 1))), "only a band is an edge"));
}

TEST(EdgeSchema, OrAndAllTablesCarryEdgesAndAPlainBandTableHasNone)
{
	auto s = SchemaFromYaml(R"(
tables:
  - name: pulse
    read_only: true
    pick: or
    inputs:
      - {name: t, type: double}
    columns:
      - {name: zone, type: band, input: t, edge: true}
      - {name: bit,  type: flags}
    output: bit
  - name: every
    read_only: true
    pick: all
    inputs:
      - {name: t, type: double}
    columns:
      - {name: zone, type: band, input: t, edge: true}
      - {name: clip, type: int}
  - name: plain
    read_only: true
    pick: all
    inputs:
      - {name: t, type: double}
    columns:
      - {name: zone, type: band, input: t}
      - {name: clip, type: int}
)", "edge.yaml");
	ASSERT_TRUE(s.has_value()) << RefusalOf(s);
	EXPECT_TRUE(s->tables()[0].edges);
	EXPECT_TRUE(s->tables()[1].edges);
	EXPECT_FALSE(s->tables()[2].edges);
}

TEST(EdgeSchema, ABlankEdgeCellIsNoEdgeAndOnlyAnEdgeTakesOne)
{
	relation_store store = StoreFrom(kZone);
	ASSERT_EQ(RefusalOf(LoadText(store, 0, "label,zone,clip\n1,0.4..0.5,10\n1,,11\n")), "(accepted)");
	EXPECT_EQ(store.RowCount(0), 2u);

	// A plain band has no blank: its catch-all is written as the whole range.
	relation_store plain = StoreFrom(R"(
tables:
  - name: zone
    read_only: true
    pick: best
    inputs:
      - {name: t, type: double, range: 0..1}
    columns:
      - {name: label, type: int, key: true, domain: rows}
      - {name: zone,  type: band, input: t}
      - {name: clip,  type: int}
    order_by: {columns: [label]}
)");
	EXPECT_TRUE(Mentions(RefusalOf(LoadText(plain, 0, "label,zone,clip\n1,,11\n")), "column 'zone' is empty"));
	cell const noEdgeRow[] = {cell::Integer(1), NoEdge(), cell::Integer(11)};
	EXPECT_FALSE(LoadOne(plain, noEdgeRow).has_value()) << "NoEdge is no band";

	// Through the builder: NoEdge is accepted in the edge column, half a NaN is not.
	relation_store edged = StoreFrom(kZone);
	EXPECT_EQ(RefusalOf(LoadOne(edged, noEdgeRow)), "(accepted)");
	cell const halfRow[] = {cell::Integer(1), cell::Vector(std::numeric_limits<float>::quiet_NaN(), 0.5f), cell::Integer(11)};
	EXPECT_FALSE(LoadOne(edged, halfRow).has_value());
}

// ---- The proof: an edge row is a conditional row ---------------------------------

TEST(EdgeLayout, ARunWhoseOnlyCatchAllIsAnEdgeIsNotTotal)
{
	relation_store store = StoreFrom(kZone);
	std::string const refused = RefusalOf(LoadText(store, 0, "label,zone,clip\n1,0.3..0.5,10\n"));
	EXPECT_TRUE(Mentions(refused, "key (1): t 0 has no row with no script condition and no edge")) << refused;
}

TEST(EdgeLayout, AnEdgeRowBeforeACatchAllIsNotDeadAndOneAfterItIs)
{
	relation_store store = StoreFrom(kZone);
	EXPECT_EQ(RefusalOf(LoadText(store, 0, "label,zone,clip\n1,0.3..0.5,10\n1,,11\n")), "(accepted)");
	relation_store late = StoreFrom(kZone);
	std::string const refused = RefusalOf(LoadText(late, 0, "label,zone,clip\n1,,11\n1,0.3..0.5,10\n"));
	EXPECT_TRUE(Mentions(refused, "is never picked")) << refused;
	EXPECT_TRUE(Mentions(refused, "covered by line 2")) << refused;
}

TEST(EdgeLayout, AnEdgeNoInputCanCrossIntoIsDead)
{
	// The band is the whole range: no clamped value lies outside it, so no
	// previous value can cross into it.
	relation_store store = StoreFrom(kZone);
	std::string const refused = RefusalOf(LoadText(store, 0, "label,zone,clip\n1,0..1,10\n1,,11\n"));
	EXPECT_TRUE(Mentions(refused, "is never picked; its bands hold for no input")) << refused;
}

TEST(EdgeLayout, AnEdgeHoldsWhereACrossingCanEndNotOnlyInsideItsBand)
{
	// Upward into 0.6..1 ends at 0.6 or above; a plain row covering 0.6..1
	// before it leaves it no cell, so it is dead. Below 0.6 it never ends.
	relation_store store = StoreFrom(R"(
tables:
  - name: zone
    read_only: true
    pick: best
    inputs:
      - {name: t, type: double, range: 0..1}
    columns:
      - {name: label,  type: int, key: true, domain: rows}
      - {name: zone,   type: band, input: t, edge: true}
      - {name: window, type: band, input: t}
      - {name: clip,   type: int}
    order_by: {columns: [label]}
)");
	std::string const dead = RefusalOf(LoadText(store, 0, "label,zone,window,clip\n1,,0.6..1,9\n1,0.6..1,0..1,10\n1,,0..1,11\n"));
	EXPECT_TRUE(Mentions(dead, "is never picked")) << dead;
	// Downward into 0..0.2 ends below 0.2, so a plain row over 0..0.2 before it
	// leaves it no cell either.
	std::string const deadBelow = RefusalOf(LoadText(store, 0, "label,zone,window,clip\n1,,0..0.2,9\n1,0..0.2,0..1,10\n1,,0..1,11\n"));
	EXPECT_TRUE(Mentions(deadBelow, "is never picked")) << deadBelow;
	// A short band crossed downwards ends anywhere below it, so a plain row
	// covering only the band itself does not shadow it.
	EXPECT_EQ(RefusalOf(LoadText(store, 0, "label,zone,window,clip\n1,,0.4..0.42,9\n1,0.4..0.42,0..1,10\n1,,0..1,11\n")),
		"(accepted)");
}

TEST(EdgeLayout, AnOverlapRefusalWithAnEdgeRowSaysWhereAnEdgeHolds)
{
	relation_store store = StoreFrom(R"(
tables:
  - name: zone
    read_only: true
    pick: best
    inputs:
      - {name: t, type: double, range: 0..1}
      - {name: f, type: flags}
    columns:
      - {name: label,  type: int, key: true, domain: rows}
      - {name: near,   type: flags, input: f}
      - {name: zone,   type: band, input: t, edge: true}
      - {name: window, type: band, input: t}
      - {name: clip,   type: int}
    order_by: {columns: [label]}
)");
	ASSERT_EQ(RefusalOf(LoadText(store, 0, "label,near,zone,window,clip\n1,0,,0..1,9\n1,1,0.4..0.42,0..1,10\n", 1)), "(accepted)");
	// Owner 1's edge holds wherever a crossing into 0.4..0.42 ends, which is
	// all of 0..1, so owner 2's 0.8..1 under equal flags overlaps it.
	std::string const refused = RefusalOf(LoadText(store, 0, "label,near,zone,window,clip\n1,1,,0.8..1,12\n", 2));
	EXPECT_TRUE(Mentions(refused, "zone edge 0.4..0.42")) << refused;
	EXPECT_TRUE(Mentions(refused, "; two owners may not claim the same inputs; an edge holds wherever a crossing into it can end"))
		<< refused;

	// Two plain rows overlapping say nothing of edges.
	relation_store plain = StoreFrom(kZone);
	ASSERT_EQ(RefusalOf(LoadText(plain, 0, "label,zone,clip\n1,,9\n", 1)), "(accepted)");
	std::string const noEdge = RefusalOf(LoadText(plain, 0, "label,zone,clip\n1,,9\n", 2));
	EXPECT_TRUE(Mentions(noEdge, "two owners may not claim the same inputs")) << noEdge;
	EXPECT_FALSE(Mentions(noEdge, "an edge holds")) << noEdge;
}

// ---- The interpreted picks -------------------------------------------------------

TEST(EdgePick, PickBestHoldsAnEdgeOnlyWhenTheInputCrossedIntoItsBand)
{
	relation_store store = StoreFrom(kZone);
	ASSERT_EQ(RefusalOf(LoadText(store, 0, "label,zone,clip\n1,0.4..0.5,10\n1,,11\n")), "(accepted)");
	EXPECT_EQ(ZoneClip(store, 0.1, 0.45), 10) << "into the band from below";
	EXPECT_EQ(ZoneClip(store, 0.9, 0.45), 10) << "into the band from above";
	EXPECT_EQ(ZoneClip(store, 0.45, 0.47), 11) << "staying inside is no crossing";
	EXPECT_EQ(ZoneClip(store, 0.45, 0.9), 11) << "leaving from inside is no crossing";
	EXPECT_EQ(ZoneClip(store, 0.1, 0.9), 10) << "a jump through the band upwards";
	EXPECT_EQ(ZoneClip(store, 0.9, 0.1), 10) << "a jump through the band downwards";
	EXPECT_EQ(ZoneClip(store, 0.1, 0.4), 10) << "landing exactly on lo is inside";
	EXPECT_EQ(ZoneClip(store, 0.1, 0.5), 10) << "reaching hi from below passed through the band";
	EXPECT_EQ(ZoneClip(store, 0.9, 0.5), 11) << "reaching hi from above never entered it";
	EXPECT_EQ(ZoneClip(store, 0.5, 0.45), 10) << "previous on hi is outside, so this enters";
	EXPECT_EQ(ZoneClip(store, 0.4, 0.1), 11) << "previous on lo is inside";
	EXPECT_EQ(ZoneClip(store, 0.1, 0.39), 11);
	EXPECT_EQ(ZoneClip(store, 0.6, 0.9), 11);
	EXPECT_EQ(ZoneClip(store, 0.45, 0.45), 11);
	// Previous values are clamped as inputs are.
	EXPECT_EQ(ZoneClip(store, -5, 0.45), 10);
	EXPECT_EQ(ZoneClip(store, 7, 0.45), 10);
	EXPECT_EQ(ZoneClip(store, -5, -1), 11);
}

TEST(EdgePick, APickOfAnEdgeTableNeedsThePreviousInputsAndChecksThem)
{
	relation_store store = StoreFrom(kZone);
	ASSERT_EQ(RefusalOf(LoadText(store, 0, "label,zone,clip\n1,0.4..0.5,10\n1,,11\n")), "(accepted)");
	cell const keys[]   = {cell::Integer(1)};
	cell const inputs[] = {cell::Real(0.45)};
	// The wrong-sized, non-empty span first: without the size check it would
	// read in bounds and answer, so a missing check fails here, by assertion,
	// before the empty span below could be read past its end.
	cell const two[] = {cell::Real(0.1), cell::Real(0.1)};
	std::string const wrongSize = RefusalOf(PickBest(store, 0, pick_query{keys, inputs, nullptr, two}));
	ASSERT_TRUE(Mentions(wrongSize, "given 2 previous inputs")) << wrongSize;
	std::string const none = RefusalOf(PickBest(store, 0, pick_query{keys, inputs}));
	ASSERT_TRUE(Mentions(none, "a pick of 'zone' reads an edge and was given 0 previous inputs; the table declares 1")) << none;
	cell const nan[] = {cell::Real(kNaN)};
	std::string const refused = RefusalOf(PickBest(store, 0, pick_query{keys, inputs, nullptr, nan}));
	EXPECT_TRUE(Mentions(refused, "input 't' of 'zone' is NaN")) << refused;
	EXPECT_TRUE(Mentions(refused, "the previous value")) << refused;
}

TEST(EdgePick, OnlyThePreviousValuesAnEdgeReadsAreChecked)
{
	// u is a plain ranged input: its previous value is never looked at, so a NaN
	// there is no refusal; t is read by the edge, so a NaN there is.
	relation_store store = StoreFrom(R"(
tables:
  - name: zone
    read_only: true
    pick: best
    inputs:
      - {name: t, type: double, range: 0..1}
      - {name: u, type: double, range: 0..1}
    columns:
      - {name: label, type: int, key: true, domain: rows}
      - {name: zone,  type: band, input: t, edge: true}
      - {name: level, type: band, input: u}
      - {name: clip,  type: int}
    order_by: {columns: [label]}
)");
	ASSERT_EQ(RefusalOf(LoadText(store, 0, "label,zone,level,clip\n1,0.4..0.5,0..1,10\n1,,0..1,11\n")), "(accepted)");
	cell const keys[]     = {cell::Integer(1)};
	cell const inputs[]   = {cell::Real(0.45), cell::Real(0.5)};
	cell const plainNaN[] = {cell::Real(0.1), cell::Real(kNaN)};
	auto accepted = PickBest(store, 0, pick_query{keys, inputs, nullptr, plainNaN});
	EXPECT_TRUE(accepted.has_value()) << RefusalOf(accepted);
	cell const edgeNaN[] = {cell::Real(kNaN), cell::Real(0.1)};
	std::string const refused = RefusalOf(PickBest(store, 0, pick_query{keys, inputs, nullptr, edgeNaN}));
	EXPECT_TRUE(Mentions(refused, "input 't' of 'zone' is NaN")) << refused;
	EXPECT_TRUE(Mentions(refused, "the previous value of input 't'")) << refused;
}

TEST(EdgePick, PickOrAndPickAllReadEdgesToo)
{
	relation_store store = StoreFrom(R"(
tables:
  - name: pulse
    read_only: true
    pick: or
    inputs:
      - {name: t, type: double}
    columns:
      - {name: zone, type: band, input: t, edge: true}
      - {name: bit,  type: flags}
    output: bit
  - name: every
    read_only: true
    pick: all
    inputs:
      - {name: t, type: double}
    columns:
      - {name: zone, type: band, input: t, edge: true}
      - {name: clip, type: int}
)");
	ASSERT_EQ(RefusalOf(LoadText(store, 0, "zone,bit\n0.4..0.42,1\n0..0.2,2\n,4\n")), "(accepted)");
	ASSERT_EQ(RefusalOf(LoadText(store, 1, "zone,clip\n0.4..0.42,1\n0..0.2,2\n,4\n")), "(accepted)");
	auto bits = [&](double previous, double now)
	{
		cell const inputs[]    = {cell::Real(now)};
		cell const previous_[] = {cell::Real(previous)};
		auto ored = PickOr(store, 0, pick_query{{}, inputs, nullptr, previous_});
		EXPECT_TRUE(ored.has_value()) << RefusalOf(ored);
		return ored ? *ored : 0u;
	};
	EXPECT_EQ(bits(0.1, 0.9), 5u) << "through the short band";
	EXPECT_EQ(bits(0.9, 0.1), 7u) << "through it downwards, and into 0..0.2";
	EXPECT_EQ(bits(0.41, 0.41), 4u);
	cell const inputs[]    = {cell::Real(0.1)};
	cell const previous_[] = {cell::Real(0.9)};
	auto all = PickAll(store, 1, pick_query{{}, inputs, nullptr, previous_});
	ASSERT_TRUE(all.has_value()) << RefusalOf(all);
	EXPECT_EQ(*all, (std::vector<size_t>{0, 1, 2}));
	EXPECT_FALSE(PickAll(store, 1, pick_query{{}, inputs}).has_value()) << "no previous inputs";
}

// ---- Machines ----------------------------------------------------------------------

namespace
{

struct edge_host
{
	double threat  = 0.1;
	double energy  = 0.5;
	bool   unwired = false;   // the threat getter cannot answer
};

edge_host const& HostOf(void const* self)
{
	return *static_cast<edge_host const*>(self);
}

cell ReadThreat(void*, void const* self, ErrorContext* error_out)
{
	if (HostOf(self).unwired)
	{
		*error_out = ErrorContext("no target");
		return cell::Real(0.77);
	}
	return cell::Real(HostOf(self).threat);
}

cell ReadEnergy(void*, void const* self, ErrorContext*)
{
	return cell::Real(HostOf(self).energy);
}

// The commands a binding's sink received, in order.
struct sink_log
{
	std::vector<std::string> commands;
};

host_binding EdgeGetters(sink_log* log)
{
	host_binding host;
	host.resolve = [](std::string_view name) -> tl::expected<input_getter, ErrorContext>
	{
		if (name == "threat")
		{
			return input_getter{&ReadThreat, nullptr, nullptr};
		}
		if (name == "energy")
		{
			return input_getter{&ReadEnergy, nullptr, nullptr};
		}
		return tl::make_unexpected(ErrorContext("no wire named '", name, "'"));
	};
	host.sink = [log](uint64_t, uint32_t, std::string_view command)
	{
		if (log)
		{
			log->commands.emplace_back(command);
		}
	};
	return host;
}

struct edge_fixture
{
	schema_document            document;
	relation_store             store;
	counted_ptr<Binding const> binding;
	sink_log                   log;

	// pivoted names the tables whose rows are loaded as pivoted CSV.
	edge_fixture(std::string_view yaml, std::vector<std::string_view> const& rows, std::vector<uint32_t> const& pivoted = {})
		: document(Parse(yaml)), store(std::move(document.tables))
	{
		if (document.machines().empty())
		{
			return;
		}
		for (uint32_t t = 0; t < rows.size(); ++t)
		{
			bool const pivot = std::find(pivoted.begin(), pivoted.end(), t) != pivoted.end();
			EXPECT_EQ(RefusalOf(LoadText(store, t, rows[t], 1, pivot)), "(accepted)") << "table " << t;
		}
		auto prepared = Prepare(document.machines()[0], store);
		EXPECT_TRUE(prepared.has_value()) << RefusalOf(prepared);
		if (!prepared)
		{
			return;
		}
		auto bound = Bind(*prepared, EdgeGetters(&log));
		EXPECT_TRUE(bound.has_value()) << RefusalOf(bound);
		if (bound)
		{
			binding = *bound;
		}
	}

	static schema_document Parse(std::string_view yaml)
	{
		auto doc = DocumentFromYaml(yaml, "edge.yaml");
		EXPECT_TRUE(doc.has_value()) << RefusalOf(doc);
		return doc ? std::move(*doc) : schema_document{};
	}
};

// The worked example: a grazer startles when threat crosses 0.5 upwards. The
// startled state lasts one step; its entry emits the startle.
constexpr std::string_view kStartle = R"(
tables:
  - name: startle
    read_only: true
    pick: best
    inputs:
      - {name: threat, type: double, range: 0..1}
      - {name: energy, type: double, range: 0..1}
    columns:
      - {name: mode,   type: hash, key: true, domain: rows}
      - {name: rise,   type: band, input: threat, edge: true}
      - {name: tired,  type: band, input: energy}
      - {name: next,   type: hash}
    order_by: {columns: [mode]}
  - name: startle_enter
    read_only: true
    pick: best
    columns:
      - {name: mode, type: hash, key: true, domain: rows}
    order_by: {columns: [mode]}
    suffix: command
machines:
  - name: grazer
    state:
      - {name: mode, type: hash, members: [graze, startled], initial: graze}
    inputs:
      - {name: threat, type: double, range: 0..1}
      - {name: energy, type: double, range: 0..1}
    step:
      - from:  startle
        where: {mode: state.mode}
        given: {threat: input.threat, energy: input.energy}
        set:   {mode: next}
    entry:
      - from:  startle_enter
        where: {mode: state.mode}
        emit:  command
)";

constexpr std::string_view kStartleRows = R"(mode, rise, tired, next
graze,    0.5..1, 0..1, startled
graze,    ,       0..1, graze
startled, ,       0..1, graze
)";

constexpr std::string_view kStartleEnterRows = "mode,command\ngraze,(anim graze)\nstartled,(anim startle)\n";

edge_fixture Startle()
{
	return edge_fixture(kStartle, {kStartleRows, kStartleEnterRows});
}

int Startles(sink_log const& log)
{
	int n = 0;
	for (std::string const& c : log.commands)
	{
		n += c == "(anim startle)" ? 1 : 0;
	}
	return n;
}

std::string MachineRefusal(std::string_view given)
{
	std::string yaml(kStartle);
	std::string const from = "given: {threat: input.threat, energy: input.energy}";
	yaml.replace(yaml.find(from), from.size(), given);
	return RefusalOf(DocumentFromYaml(yaml, "edge.yaml"));
}

}

TEST(GrazerEdge, AStartleFiresOnceWhenThreatCrossesUpwards)
{
	edge_fixture f = Startle();
	ASSERT_TRUE(f.binding);
	edge_host host;
	instance  grazer = CreateInstance(*f.binding, 1, 0, &host);
	f.log.commands.clear();
	// 0.6 crosses 0.5; staying above never refires; down and up again fires;
	// a jump from 0.1 to 0.9 fires once.
	double const walk[]  = {0.6, 0.7, 0.8, 0.95, 0.3, 0.55, 0.55, 0.1, 0.9, 0.9, 0.49, 0.5};
	int const    fired[] = {1,   1,   1,   1,    1,   2,    2,    2,   3,   3,   3,    4};
	for (size_t i = 0; i < std::size(walk); ++i)
	{
		host.threat = walk[i];
		Step(grazer, &host, i);
		EXPECT_EQ(Startles(f.log), fired[i]) << "step " << i << " threat " << walk[i];
	}
}

TEST(MachineEdge, TheFirstStepSeesACrossingOnlyIfTheInputMovedSinceCreation)
{
	edge_fixture f = Startle();
	ASSERT_TRUE(f.binding);
	edge_host high;
	high.threat = 0.6;
	instance created = CreateInstance(*f.binding, 1, 0, &high);
	EXPECT_EQ(created.State(0).i, Hash("graze")) << "creation runs no step list";
	Step(created, &high, 1);
	EXPECT_EQ(created.State(0).i, Hash("graze")) << "previous is the value read at creation";

	edge_host low;
	instance  moved = CreateInstance(*f.binding, 2, 0, &low);
	low.threat = 0.6;
	Step(moved, &low, 1);
	EXPECT_EQ(moved.State(0).i, Hash("startled"));
}

TEST(MachineEdge, AnInputNeverReadHasNoPreviousValueAndCrossesNothing)
{
	// The getter fails at creation, so the first value read has nothing to
	// cross from; the one after it does.
	edge_fixture f = Startle();
	ASSERT_TRUE(f.binding);
	edge_host host;
	host.unwired    = true;
	instance grazer = CreateInstance(*f.binding, 1, 0, &host);
	host.unwired = false;
	host.threat  = 0.6;
	Step(grazer, &host, 1);
	EXPECT_EQ(grazer.Answer(trigger::Step, 0).outcome, clause_outcome::Answered);
	EXPECT_EQ(grazer.State(0).i, Hash("graze"));
	host.threat = 0.2;
	Step(grazer, &host, 2);
	host.threat = 0.6;
	Step(grazer, &host, 3);
	EXPECT_EQ(grazer.State(0).i, Hash("startled"));
}

TEST(MachineEdge, AHeldStepStillAdvancesThePreviousValue)
{
	// Inputs are read once per step, whatever the clauses do with them: a
	// crossing on a step whose clause holds (a NaN energy) is spent.
	edge_fixture f = Startle();
	ASSERT_TRUE(f.binding);
	edge_host host;
	instance  grazer = CreateInstance(*f.binding, 1, 0, &host);
	host.threat = 0.6;
	host.energy = kNaN;
	Step(grazer, &host, 1);
	EXPECT_EQ(grazer.Answer(trigger::Step, 0).outcome, clause_outcome::Failed);
	host.energy = 0.5;
	host.threat = 0.7;
	Step(grazer, &host, 2);
	EXPECT_EQ(grazer.Answer(trigger::Step, 0).outcome, clause_outcome::Answered);
	EXPECT_EQ(grazer.State(0).i, Hash("graze")) << "the crossing was on the held step";
}

TEST(MachineEdge, AValueThatIsNotReadLeavesThePreviousValueAtTheLastOneRead)
{
	// A getter that fails and a NaN are no value: the next crossing is
	// measured from the last value read, so it is not skipped.
	edge_fixture f = Startle();
	ASSERT_TRUE(f.binding);
	edge_host host;
	instance  grazer = CreateInstance(*f.binding, 1, 0, &host);
	host.unwired = true;
	Step(grazer, &host, 1);
	EXPECT_EQ(grazer.Answer(trigger::Step, 0).outcome, clause_outcome::SourceHeld);
	host.unwired = false;
	host.threat  = 0.6;
	Step(grazer, &host, 2);
	EXPECT_EQ(grazer.State(0).i, Hash("startled")) << "from 0.1, not from what the failed getter returned";

	Step(grazer, &host, 3);   // back to graze, threat still 0.6
	host.threat = 0.1;
	Step(grazer, &host, 4);
	host.threat = kNaN;
	Step(grazer, &host, 5);
	EXPECT_EQ(grazer.Answer(trigger::Step, 0).outcome, clause_outcome::Failed);
	host.threat = 0.6;
	Step(grazer, &host, 6);
	EXPECT_EQ(grazer.State(0).i, Hash("startled")) << "from 0.1, across the NaN";
}

TEST(MachineEdge, AnEdgeReadsAMachineInputThroughItsGetter)
{
	EXPECT_EQ(MachineRefusal("given: {threat: input.threat, energy: input.energy}"), "(accepted)");
	std::string const literal = MachineRefusal("given: {threat: {literal: 0.6}, energy: input.energy}");
	EXPECT_TRUE(Mentions(literal, "input 'threat' is read by edge 'rise', which compares it with its value on the "
		"previous step; it comes from a machine input's getter, and here from a literal")) << literal;
	std::string const state = MachineRefusal("given: {threat: input.energy, energy: input.energy}");
	EXPECT_EQ(state, "(accepted)") << "any getter's input will do";

	std::string withMessage(kStartle);
	std::string const inputs = "      - {name: energy, type: double, range: 0..1}\n    step:";
	withMessage.replace(withMessage.find(inputs), inputs.size(),
		"      - {name: energy, type: double, range: 0..1}\n      - {name: kind, type: hash}\n"
		"    message: {type: kind, arguments: [threat]}\n    step:");
	std::string const argument = RefusalOf(DocumentFromYaml(withMessage, "edge.yaml"));
	EXPECT_TRUE(Mentions(argument, "here from message argument 0")) << argument;
}

TEST(MachineEdge, AnEdgeFromAStateOrAnEarlierClauseIsRefused)
{
	auto refused = DocumentFromYaml(R"(
tables:
  - name: level
    read_only: true
    pick: best
    columns:
      - {name: mode,  type: hash, key: true, domain: rows}
      - {name: value, type: double}
    order_by: {columns: [mode]}
  - name: zone
    read_only: true
    pick: best
    inputs:
      - {name: t, type: double, range: 0..1}
    columns:
      - {name: mode, type: hash, key: true, domain: rows}
      - {name: zone, type: band, input: t, edge: true}
      - {name: next, type: hash}
    order_by: {columns: [mode]}
machines:
  - name: m
    state:
      - {name: mode, type: hash, members: [a], initial: a}
      - {name: level, type: double, initial: 0}
    step:
      - from:  level
        where: {mode: state.mode}
      - from:  zone
        where: {mode: state.mode}
        given: {t: clause.0.value}
        set:   {mode: next}
)", "edge.yaml");
	EXPECT_TRUE(Mentions(RefusalOf(refused), "and here from column 'value' of clause 0")) << RefusalOf(refused);
	auto fromState = DocumentFromYaml(R"(
tables:
  - name: zone
    read_only: true
    pick: best
    inputs:
      - {name: t, type: double, range: 0..1}
    columns:
      - {name: mode, type: hash, key: true, domain: rows}
      - {name: zone, type: band, input: t, edge: true}
      - {name: next, type: hash}
    order_by: {columns: [mode]}
machines:
  - name: m
    state:
      - {name: mode, type: hash, members: [a], initial: a}
      - {name: level, type: double, initial: 0}
    step:
      - from:  zone
        where: {mode: state.mode}
        given: {t: state.level}
        set:   {mode: next}
)", "edge.yaml");
	EXPECT_TRUE(Mentions(RefusalOf(fromState), "and here from state column 'level'")) << RefusalOf(fromState);
}

TEST(MachineEdge, PrepareProvesAnEdgeRowConditionalOverTheClausesDomain)
{
	// Load proves the table's declared domain, [graze]; the machine's state
	// closes the key over [graze, startled], and startled's only row is an
	// edge: an orphan to Load, not total to Prepare.
	std::string yaml(kStartle);
	std::string const domain = "{name: mode,   type: hash, key: true, domain: rows}";
	yaml.replace(yaml.find(domain), domain.size(), "{name: mode,   type: hash, key: true, domain: {members: [graze]}}");
	auto doc = DocumentFromYaml(yaml, "edge.yaml");
	ASSERT_TRUE(doc.has_value()) << RefusalOf(doc);
	relation_store store(std::move(doc->tables));
	ASSERT_EQ(RefusalOf(LoadText(store, 0, "mode, rise, tired, next\ngraze, 0.5..1, 0..1, startled\ngraze, , 0..1, graze\n"
		"startled, 0.5..1, 0..1, graze\n")), "(accepted)");
	ASSERT_EQ(RefusalOf(LoadText(store, 1, kStartleEnterRows)), "(accepted)");
	std::string const refused = RefusalOf(Prepare(doc->machines()[0], store));
	EXPECT_TRUE(Mentions(refused, "has no row with no script condition and no edge")) << refused;
}

// ---- Parity: Step against PickBest and PickOr over random walks ------------------

namespace
{

// A short edge band 0.40..0.42 crossed either way, an edge entered only from
// above (0..0.2), one entered only from below and ending high (0.6..1 inside
// 0.8..1), and plain bands; an or table pulses on the same edges.
constexpr std::string_view kWalker = R"(
tables:
  - name: walk
    read_only: true
    pick: best
    inputs:
      - {name: x, type: double, range: 0..1}
    columns:
      - {name: mode,   type: hash, key: true, domain: rows}
      - {name: zone,   type: band, input: x, edge: true}
      - {name: window, type: band, input: x}
      - {name: next,   type: hash}
    order_by: {columns: [mode]}
  - name: pulse
    read_only: true
    pick: or
    inputs:
      - {name: x, type: double, range: 0..1}
    columns:
      - {name: zone, type: band, input: x, edge: true}
      - {name: bit,  type: flags}
    output: bit
machines:
  - name: walker
    state:
      - {name: mode, type: hash, members: [a, b, c, d], initial: a}
    inputs:
      - {name: threat, type: double, range: 0..1}
    step:
      - from:  walk
        where: {mode: state.mode}
        given: {x: input.threat}
        set:   {mode: next}
      - from:  pulse
        given: {x: input.threat}
)";

constexpr std::string_view kWalkRows = R"(mode, zone, window, next
a, 0.40..0.42, 0..1,   b
a, 0..0.2,     0..1,   c
a, 0.6..1,     0.8..1, d
a, ,           0..0.5, a
a, ,           0.5..1, c
b, 0.40..0.42, 0..1,   a
b, ,           0..1,   b
c, ,           0..1,   a
d, 0..0.2,     0..1,   a
d, ,           0..1,   d
)";

constexpr std::string_view kPulseRows = "zone, bit\n0.40..0.42, 1\n0..0.2, 2\n0.6..1, 4\n, 8\n";

// Values that probe an edge: its ends, just either side of them, big jumps,
// staying inside, the range's ends and beyond, and NaN.
double WalkValue(std::mt19937_64& rng, double last)
{
	static double const probes[] = {0.40, 0.42, 0.41, 0.41, double(std::nextafter(0.4f, 0.f)),
		double(std::nextafter(0.42f, 1.f)), 0.1, 0.9, 0.0, 0.2, 0.6, 0.8, 1.0, -1.0, 2.0, kNaN};
	std::uniform_int_distribution<size_t> pick(0, std::size(probes) + 3);
	size_t const i = pick(rng);
	if (i < std::size(probes))
	{
		return probes[i];
	}
	if (i == std::size(probes))
	{
		return last;   // staying put
	}
	return std::uniform_real_distribution<double>(-0.1, 1.1)(rng);
}

template<typename Picked>
void ExpectSameAnswer(tl::expected<std::optional<size_t>, ErrorContext> const& oracle, clause_answer got,
	Picked const& where)
{
	if (!oracle)
	{
		EXPECT_EQ(got.outcome, clause_outcome::Failed) << where() << oracle.error().format();
		return;
	}
	ASSERT_TRUE(oracle->has_value()) << where() << "a proved run always picks";
	EXPECT_EQ(got.outcome, clause_outcome::Answered) << where();
	EXPECT_EQ(got.row, **oracle) << where();
}

}

namespace
{

// Walks the walker's threat at random and checks each step against PickBest
// (walk) and PickOr (pulse) given the same value and previous value.
void WalkParity(std::string_view yaml, uint64_t seed)
{
	edge_fixture f(yaml, {kWalkRows, kPulseRows});
	ASSERT_TRUE(f.binding);
	auto walk  = f.store.Version(0);
	auto pulse = f.store.Version(1);
	ASSERT_TRUE(walk && pulse);
	std::mt19937_64 rng(seed);
	edge_host       host;
	host.threat     = 0.1;
	instance walker = CreateInstance(*f.binding, 1, 0, &host);
	double   last   = 0.1;   // the last value read that was not NaN
	int      crossings = 0;
	int      failed    = 0;
	int      pulses    = 0;
	for (int i = 0; i < 6000; ++i)
	{
		host.threat = WalkValue(rng, host.threat);
		cell const before = walker.State(0);
		cell const keys[]     = {before};
		cell const inputs[]   = {cell::Real(host.threat)};
		cell const previous[] = {cell::Real(last)};
		auto const oracle = PickBest(**walk, pick_query{keys, inputs, nullptr, previous});
		auto const ored   = PickOr(**pulse, pick_query{{}, inputs, nullptr, previous});
		Step(walker, &host, uint64_t(i));
		auto where = [&]() { return "step " + std::to_string(i) + " x " + std::to_string(host.threat) + " previous "
			+ std::to_string(last) + " "; };
		ExpectSameAnswer(oracle, walker.Answer(trigger::Step, 0), where);
		clause_answer const bitsGot = walker.Answer(trigger::Step, 1);
		if (ored)
		{
			EXPECT_EQ(bitsGot.outcome, clause_outcome::Answered) << where();
			EXPECT_EQ(bitsGot.bits, *ored) << where();
			pulses += (*ored & 7u) != 0 ? 1 : 0;
		}
		else
		{
			EXPECT_EQ(bitsGot.outcome, clause_outcome::Failed) << where();
			++failed;
		}
		if (oracle && *oracle && walker.State(0).i != before.i)
		{
			++crossings;
		}
		if (!std::isnan(host.threat))
		{
			last = host.threat;
		}
		if (::testing::Test::HasFailure())
		{
			break;
		}
	}
	EXPECT_GT(crossings, 600);
	EXPECT_GT(failed, 200) << "NaN";
	EXPECT_GT(pulses, 1000);
}

}

TEST(EdgeParity, RandomWalksMatchPickBestAndPickOrStepByStep)
{
	WalkParity(kWalker, 0xed6e);
}

// A two-input table (an edge on threat, a plain band on energy) walked on both
// inputs: Step answers what PickBest answers for the same values.
TEST(EdgeParity, TwoInputsWalkedTogetherMatchPickBestStepByStep)
{
	edge_fixture f = Startle();
	ASSERT_TRUE(f.binding);
	auto startle = f.store.Version(0);
	ASSERT_TRUE(startle.has_value());
	std::mt19937_64 rng(0x2d);
	edge_host       host;
	instance        grazer = CreateInstance(*f.binding, 1, 0, &host);
	double          lastThreat = host.threat;
	double          lastEnergy = host.energy;
	int             startled   = 0;
	int             failed     = 0;
	for (int i = 0; i < 4000; ++i)
	{
		host.threat = WalkValue(rng, host.threat);
		host.energy = WalkValue(rng, host.energy);
		cell const before     = grazer.State(0);
		cell const keys[]     = {before};
		cell const inputs[]   = {cell::Real(host.threat), cell::Real(host.energy)};
		cell const previous[] = {cell::Real(lastThreat), cell::Real(lastEnergy)};
		auto const oracle     = PickBest(**startle, pick_query{keys, inputs, nullptr, previous});
		Step(grazer, &host, uint64_t(i));
		auto where = [&]() { return "step " + std::to_string(i) + " threat " + std::to_string(host.threat) + " from "
			+ std::to_string(lastThreat) + " energy " + std::to_string(host.energy) + " "; };
		ExpectSameAnswer(oracle, grazer.Answer(trigger::Step, 0), where);
		startled += grazer.State(0).i == Hash("startled") ? 1 : 0;
		failed   += oracle ? 0 : 1;
		if (!std::isnan(host.threat))
		{
			lastThreat = host.threat;
		}
		if (!std::isnan(host.energy))
		{
			lastEnergy = host.energy;
		}
		if (::testing::Test::HasFailure())
		{
			break;
		}
	}
	EXPECT_GT(startled, 100);
	EXPECT_GT(failed, 200) << "NaN on either input";
}

// An edge row in a ranked group: when its edge does not hold the pick falls to
// the group below, here the default.
constexpr std::string_view kRanked = R"(
tables:
  - name: ranked
    read_only: true
    pick: best
    inputs:
      - {name: x, type: double, range: 0..1}
      - {name: f, type: flags}
    columns:
      - {name: mode, type: hash, key: true, domain: rows}
      - {name: near, type: flags, input: f}
      - {name: rise, type: band, input: x, edge: true}
      - {name: next, type: hash}
    order_by: {columns: [mode]}
machines:
  - name: ranker
    state:
      - {name: mode, type: hash, members: [a, b], initial: a}
    inputs:
      - {name: threat, type: double, range: 0..1}
    step:
      - from:  ranked
        where: {mode: state.mode}
        given: {x: input.threat, f: {literal: 1}}
        set:   {mode: next}
)";

constexpr std::string_view kRankedRows = R"(mode, near, rise, next
a, 1, 0.5..1, b
a, 0, ,       a
b, 0, ,       a
)";

TEST(EdgeParity, ARankedEdgeRowThatDoesNotHoldFallsToTheGroupBelow)
{
	edge_fixture f(kRanked, {kRankedRows});
	ASSERT_TRUE(f.binding);
	auto ranked = f.store.Version(0);
	ASSERT_TRUE(ranked.has_value());
	edge_host host;
	instance  ranker = CreateInstance(*f.binding, 1, 0, &host);
	double    last   = host.threat;
	auto step = [&](double now, uint64_t at)
	{
		cell const keys[]     = {ranker.State(0)};
		cell const inputs[]   = {cell::Real(now), cell::Integer(1)};
		cell const previous[] = {cell::Real(last), cell::Integer(1)};
		auto const oracle     = PickBest(**ranked, pick_query{keys, inputs, nullptr, previous});
		host.threat = now;
		Step(ranker, &host, at);
		last = now;
		EXPECT_TRUE(oracle.has_value() && oracle->has_value()) << RefusalOf(oracle);
		clause_answer const got = ranker.Answer(trigger::Step, 0);
		EXPECT_EQ(got.outcome, clause_outcome::Answered);
		if (oracle && *oracle)
		{
			EXPECT_EQ(got.row, **oracle) << "at " << now;
			return **oracle;
		}
		return size_t(SIZE_MAX);
	};
	EXPECT_EQ(step(0.2, 1), 1u) << "inside no edge: the ranked row's edge does not hold, the default answers";
	EXPECT_EQ(ranker.State(0).i, Hash("a"));
	EXPECT_EQ(step(0.7, 2), 0u) << "the crossing: the ranked row wins";
	EXPECT_EQ(ranker.State(0).i, Hash("b"));
	EXPECT_EQ(step(0.8, 3), 2u) << "b has only its default";
	// Back in a, with the input already inside the edge's box: the ranked row is
	// in the cell's list, its edge does not hold, and the pick goes on down.
	EXPECT_EQ(step(0.9, 4), 1u) << "staying inside 0.5..1 crosses nothing";
	EXPECT_EQ(step(0.1, 5), 1u) << "leaving downwards is not into 0.5..1";
	EXPECT_EQ(step(0.9, 6), 0u);
}

// A pivoted or table whose cells column is an edge: the pivot only lays the
// rows out, so the edge reads the same through PickOr and an or clause.
constexpr std::string_view kPivotedPulse = R"(
tables:
  - name: pulse
    read_only: true
    pick: or
    output: bits
    inputs:
      - {name: x, type: double, range: 0..1}
    columns:
      - {name: bits, type: flags}
      - {name: zone, type: band, input: x, edge: true}
    pivot: {headers: bits, cells: zone}
machines:
  - name: pulser
    inputs:
      - {name: threat, type: double, range: 0..1}
    step:
      - from:  pulse
        given: {x: input.threat}
)";

TEST(EdgeParity, APivotedOrTableWithAnEdgeCellsColumnPicksAndStepsAlike)
{
	edge_fixture f(kPivotedPulse, {"Low,High\n0.40..0.42,\n0..0.2,0.6..1\n"}, {0});
	ASSERT_TRUE(f.binding);
	auto pulse = f.store.Version(0);
	ASSERT_TRUE(pulse.has_value());
	auto bits = [&](double previous, double now)
	{
		cell const inputs[]    = {cell::Real(now)};
		cell const previous_[] = {cell::Real(previous)};
		auto ored = PickOr(**pulse, pick_query{{}, inputs, nullptr, previous_});
		EXPECT_TRUE(ored.has_value()) << RefusalOf(ored);
		return ored ? *ored : 0xFFFFFFFFu;
	};
	EXPECT_EQ(bits(0.1, 0.9), 3u) << "through the short band, and into 0.6..1";
	EXPECT_EQ(bits(0.9, 0.1), 1u) << "down through it, and into 0..0.2";
	EXPECT_EQ(bits(0.41, 0.41), 0u);

	std::mt19937_64 rng(0x91);
	edge_host       host;
	instance        pulser = CreateInstance(*f.binding, 1, 0, &host);
	double          last   = host.threat;
	int             pulses = 0;
	for (int i = 0; i < 2000; ++i)
	{
		host.threat = WalkValue(rng, host.threat);
		cell const inputs[]    = {cell::Real(host.threat)};
		cell const previous[]  = {cell::Real(last)};
		auto const ored        = PickOr(**pulse, pick_query{{}, inputs, nullptr, previous});
		Step(pulser, &host, uint64_t(i));
		clause_answer const got = pulser.Answer(trigger::Step, 0);
		if (ored)
		{
			EXPECT_EQ(got.outcome, clause_outcome::Answered) << "step " << i;
			EXPECT_EQ(got.bits, *ored) << "step " << i << " x " << host.threat << " from " << last;
			pulses += *ored != 0 ? 1 : 0;
		}
		else
		{
			EXPECT_EQ(got.outcome, clause_outcome::Failed) << "step " << i;
		}
		if (!std::isnan(host.threat))
		{
			last = host.threat;
		}
		if (::testing::Test::HasFailure())
		{
			break;
		}
	}
	EXPECT_GT(pulses, 300);
}

// ---- Allocation --------------------------------------------------------------------

TEST(MachineEdge, StepsThatCrossEdgesAllocateNothing)
{
	if (!kCountsAllocations)
	{
		GTEST_SKIP() << "a sanitizer owns operator new in this build, so nothing counts allocations";
	}
	edge_fixture f(kWalker, {kWalkRows, kPulseRows});
	ASSERT_TRUE(f.binding);
	edge_host host;
	instance  walker = CreateInstance(*f.binding, 1, 0, &host);
	double const walk[] = {0.9, 0.1, 0.41, 0.41, 0.9, kNaN, 0.3, 0.05, 0.85, 0.5};
	int answered = 0;
	AllocationCount count;
	for (int i = 0; i < 200; ++i)
	{
		host.threat  = walk[i % std::size(walk)];
		host.unwired = i % 17 == 0;
		Step(walker, &host, uint64_t(i));
		answered += walker.Answer(trigger::Step, 0).outcome == clause_outcome::Answered ? 1 : 0;
	}
	EXPECT_EQ(count.Count(), 0u);
	EXPECT_GT(answered, 100);
}

// ---- Cyclic inputs -----------------------------------------------------------------

// A cyclic input's range is a period, as an animation phase's is: a pick wraps
// it into the range, and a crossing goes the shorter way around, exactly half
// a period upward. A footstep at the walk cycle's middle fires as the phase
// passes 0.5, not when it wraps from 0.95 to 0.05.

namespace
{

std::string Cyclic(std::string_view yaml)
{
	std::string text(yaml);
	std::string const from = "{name: x, type: double, range: 0..1}";
	std::string const to   = "{name: x, type: double, range: 0..1, cyclic: true}";
	for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
	{
		text.replace(at, from.size(), to);
	}
	return text;
}

constexpr std::string_view kStep = R"(
tables:
  - name: zone
    read_only: true
    pick: best
    inputs:
      - {name: x, type: double, range: 0..1}
    columns:
      - {name: label, type: int, key: true, domain: rows}
      - {name: zone,  type: band, input: x, edge: true}
      - {name: clip,  type: int}
    order_by: {columns: [label]}
)";

}

TEST(CyclicSchema, ARangedDoubleInputMayWrapAndNoOtherInput)
{
	auto accepted = SchemaFromYaml(Cyclic(kStep), "edge.yaml");
	ASSERT_TRUE(accepted.has_value()) << RefusalOf(accepted);
	EXPECT_TRUE(accepted->tables()[0].inputs[0].cyclic);
	auto linear = SchemaFromYaml(kStep, "edge.yaml");
	ASSERT_TRUE(linear.has_value()) << RefusalOf(linear);
	EXPECT_FALSE(linear->tables()[0].inputs[0].cyclic);

	std::string unranged(kStep);
	std::string const from = "{name: x, type: double, range: 0..1}";
	unranged.replace(unranged.find(from), from.size(), "{name: x, type: double, cyclic: true}");
	std::string const noRange = RefusalOf(SchemaFromYaml(unranged, "edge.yaml"));
	EXPECT_TRUE(Mentions(noRange, "input 'x' of table 'zone' is cyclic and has no range; a cyclic input's range is its period"))
		<< noRange;

	// The table input whose bands read a machine input wraps; the machine's
	// own declaration would be accepted and ignored.
	std::string machine(kStartle);
	std::string const machineInputs = "    inputs:\n      - {name: threat, type: double, range: 0..1}\n      - {name: energy, type: double, range: 0..1}\n    step:";
	machine.replace(machine.find(machineInputs), machineInputs.size(),
		"    inputs:\n      - {name: threat, type: double, range: 0..1, cyclic: true}\n      - {name: energy, type: double, range: 0..1}\n    step:");
	std::string const onMachine = RefusalOf(DocumentFromYaml(machine, "edge.yaml"));
	EXPECT_TRUE(Mentions(onMachine, "input 'threat' of the machine is cyclic; a table input whose bands read it declares that"))
		<< onMachine;
}

TEST(CyclicPick, AFootstepFiresAsThePhasePassesItNotWhenThePhaseWraps)
{
	relation_store store = StoreFrom(Cyclic(kStep));
	ASSERT_EQ(RefusalOf(LoadText(store, 0, "label,zone,clip\n1,0.5..0.55,10\n1,,11\n")), "(accepted)");
	EXPECT_EQ(ZoneClip(store, 0.95, 0.05), 11) << "the wrap goes up through 1, not down through the band";
	EXPECT_EQ(ZoneClip(store, 0.45, 0.6), 10) << "up through the band";
	EXPECT_EQ(ZoneClip(store, 0.6, 0.45), 10) << "down through the band";
	EXPECT_EQ(ZoneClip(store, 0.05, 0.95), 11) << "down through 0, nowhere near the band";
	EXPECT_EQ(ZoneClip(store, 0.52, 0.53), 11) << "staying inside is no crossing";
	// Exactly half a period is taken upward, so one pair of values crosses one way only.
	EXPECT_EQ(ZoneClip(store, 0.25, 0.75), 10);
	EXPECT_EQ(ZoneClip(store, 0.75, 0.25), 11);
	// Values wrap into the period instead of clamping, previous values too.
	EXPECT_EQ(ZoneClip(store, 0.45, 1.52), 10) << "1.52 is 0.52";
	EXPECT_EQ(ZoneClip(store, -0.55, 0.52), 10) << "-0.55 is 0.45";
	EXPECT_EQ(ZoneClip(store, 0.45, 7.0), 11) << "7 is 0, not the top of the range";

	// The same rows on the linear input: the wrap sweeps down through every band.
	relation_store linear = StoreFrom(kStep);
	ASSERT_EQ(RefusalOf(LoadText(linear, 0, "label,zone,clip\n1,0.5..0.55,10\n1,,11\n")), "(accepted)");
	EXPECT_EQ(ZoneClip(linear, 0.95, 0.05), 10);
}

TEST(CyclicPick, ABandAtEitherEndIsEnteredAcrossTheWrap)
{
	relation_store top = StoreFrom(Cyclic(kStep));
	ASSERT_EQ(RefusalOf(LoadText(top, 0, "label,zone,clip\n1,0.9..1,10\n1,,11\n")), "(accepted)");
	EXPECT_EQ(ZoneClip(top, 0.05, 0.95), 10) << "down across the wrap into the top band";
	EXPECT_EQ(ZoneClip(top, 0.95, 0.05), 11) << "up out of it";
	EXPECT_EQ(ZoneClip(top, 0.05, 0.85), 10) << "down across the wrap and through it";

	relation_store bottom = StoreFrom(Cyclic(kStep));
	ASSERT_EQ(RefusalOf(LoadText(bottom, 0, "label,zone,clip\n1,0..0.1,10\n1,,11\n")), "(accepted)");
	EXPECT_EQ(ZoneClip(bottom, 0.95, 0.05), 10) << "up across the wrap into the bottom band";
	EXPECT_EQ(ZoneClip(bottom, 0.05, 0.95), 11) << "down out of it";
}

TEST(CyclicPick, ACyclicInputRefusesAnInfinityAsWellAsNaN)
{
	relation_store store = StoreFrom(Cyclic(kStep));
	ASSERT_EQ(RefusalOf(LoadText(store, 0, "label,zone,clip\n1,0.5..0.55,10\n1,,11\n")), "(accepted)");
	cell const keys[]     = {cell::Integer(1)};
	cell const inputs[]   = {cell::Real(std::numeric_limits<double>::infinity())};
	cell const previous[] = {cell::Real(0.1)};
	std::string const refused = RefusalOf(PickBest(store, 0, pick_query{keys, inputs, nullptr, previous}));
	EXPECT_TRUE(Mentions(refused, "a cyclic input is wrapped into its period, which has no place for it")) << refused;
	cell const nan[] = {cell::Real(kNaN)};
	EXPECT_FALSE(PickBest(store, 0, pick_query{keys, nan, nullptr, previous}).has_value());

	// A linear range clamps an infinity to its top.
	relation_store linear = StoreFrom(kStep);
	ASSERT_EQ(RefusalOf(LoadText(linear, 0, "label,zone,clip\n1,0.5..0.55,10\n1,,11\n")), "(accepted)");
	EXPECT_TRUE(PickBest(linear, 0, pick_query{keys, inputs, nullptr, previous}).has_value());
}

TEST(CyclicLayout, ACrossingCanEndAnywhereOnThePeriod)
{
	// Up through 0.6..1 the short way ends past 1, below 0.6 once wrapped, so a
	// plain row over 0.6..1 before the edge no longer leaves it no cell.
	constexpr std::string_view kWindowed = R"(
tables:
  - name: zone
    read_only: true
    pick: best
    inputs:
      - {name: x, type: double, range: 0..1}
    columns:
      - {name: label,  type: int, key: true, domain: rows}
      - {name: zone,   type: band, input: x, edge: true}
      - {name: window, type: band, input: x}
      - {name: clip,   type: int}
    order_by: {columns: [label]}
)";
	constexpr std::string_view kRows = "label,zone,window,clip\n1,,0.6..1,9\n1,0.6..1,0..1,10\n1,,0..1,11\n";
	relation_store linear = StoreFrom(kWindowed);
	EXPECT_TRUE(Mentions(RefusalOf(LoadText(linear, 0, kRows)), "is never picked"));
	relation_store cyclic = StoreFrom(Cyclic(kWindowed));
	EXPECT_EQ(RefusalOf(LoadText(cyclic, 0, kRows)), "(accepted)");

	// A band that is the whole period still holds for no input.
	relation_store whole = StoreFrom(Cyclic(kStep));
	std::string const refused = RefusalOf(LoadText(whole, 0, "label,zone,clip\n1,0..1,10\n1,,11\n"));
	EXPECT_TRUE(Mentions(refused, "is never picked; its bands hold for no input")) << refused;
}

TEST(CyclicMachine, AFootstepStepsOncePerPassOfThePhase)
{
	// kStartle with its edge on a cyclic phase: startled is the footstep.
	std::string yaml(kStartle);
	std::string const table = "      - {name: threat, type: double, range: 0..1}\n      - {name: energy, type: double, range: 0..1}\n    columns:";
	yaml.replace(yaml.find(table), table.size(),
		"      - {name: threat, type: double, range: 0..1, cyclic: true}\n      - {name: energy, type: double, range: 0..1}\n    columns:");
	edge_fixture f(yaml, {"mode, rise, tired, next\ngraze, 0.5..0.55, 0..1, startled\ngraze, , 0..1, graze\nstartled, , 0..1, graze\n",
		kStartleEnterRows});
	ASSERT_TRUE(f.binding);
	edge_host host;
	host.threat     = 0.1;
	instance walker = CreateInstance(*f.binding, 1, 0, &host);
	f.log.commands.clear();
	double const phase[] = {0.3, 0.52, 0.7, 0.95, 0.05, 0.2, 0.45, 0.6, 0.9, 0.15, 0.4, 0.8};
	int const    steps[] = {0,   1,    1,   1,    1,    1,   1,    2,   2,   2,    2,   3};
	for (size_t i = 0; i < std::size(phase); ++i)
	{
		host.threat = phase[i];
		Step(walker, &host, i);
		EXPECT_EQ(Startles(f.log), steps[i]) << "step " << i << " phase " << phase[i];
	}
}

TEST(CyclicParity, RandomWalksOnAPeriodMatchPickBestAndPickOrStepByStep)
{
	// The walk's values beyond 0..1 wrap, and its jumps go the short way round.
	WalkParity(Cyclic(kWalker), 0xc1c1);
}

TEST(CyclicMachine, StepsThatWrapAllocateNothing)
{
	if (!kCountsAllocations)
	{
		GTEST_SKIP() << "a sanitizer owns operator new in this build, so nothing counts allocations";
	}
	edge_fixture f(Cyclic(kWalker), {kWalkRows, kPulseRows});
	ASSERT_TRUE(f.binding);
	edge_host host;
	instance  walker = CreateInstance(*f.binding, 1, 0, &host);
	double const walk[] = {0.95, 0.05, 0.41, 1.41, -0.6, kNaN, 0.3, 0.8, 0.3, 7.5};
	int answered = 0;
	AllocationCount count;
	for (int i = 0; i < 200; ++i)
	{
		host.threat = walk[i % std::size(walk)];
		Step(walker, &host, uint64_t(i));
		answered += walker.Answer(trigger::Step, 0).outcome == clause_outcome::Answered ? 1 : 0;
	}
	EXPECT_EQ(count.Count(), 0u);
	EXPECT_GT(answered, 100);
}
