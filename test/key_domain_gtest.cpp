#include <gtest/gtest.h>
#include "tabular/pick.h"
#include "tabular/relation.h"
#include "tabular/document.h"
#include "cityhash/src/city.h"
#include "probe_closure.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace tabular;
using tabular_test::MakeProbe;

// A best table's key says where its domain comes from, and totality is
// checked over the domain: a key in the domain with no run is refused, and a
// run whose key is outside it is an orphan, reported and never checked.

namespace
{

bool Mentions(std::string const& message, std::string_view text)
{
	return message.find(text) != std::string::npos;
}

template<typename T>
std::string RefusalOf(tl::expected<T, ErrorContext> const& e)
{
	return e ? std::string("(accepted)") : e.error().format();
}

relation_store StoreFrom(std::string_view source)
{
	auto s = ReadSchema(source, "domain.tabular");
	EXPECT_TRUE(s.has_value()) << RefusalOf(s);
	return relation_store(std::move(*s));
}

// Each note as "table status (key, ...)", in the order the store gave them.
std::vector<std::string> Notes(std::vector<run_note> const& report)
{
	std::vector<std::string> out;
	for (run_note const& note : report)
	{
		std::string key;
		for (cell const& c : note.key)
		{
			key += (key.empty() ? "" : ", ") + std::to_string(c.i);
		}
		out.push_back(std::to_string(note.table) + (note.status == run_status::Orphan ? " orphan (" : " not total (")
			+ key + ")");
	}
	return out;
}

// ---- The builder ------------------------------------------------------------

add_input const kModeInput[] = {{"mode", column_type::Flags}};

tl::expected<schema, ErrorContext> OneBest(key_domain const& domain, column_type keyType = column_type::Int,
	pick_policy pick = pick_policy::Best)
{
	add_column const columns[] = {
		{"label", keyType, true, UINT32_MAX, domain},
		{"clip",  column_type::Int},
	};
	uint32_t const   byLabel[] = {0};
	order_by const   order{byLabel, false};
	create_table const t{"lookup", columns, &order, true, {}, pick};
	return BuildSchema(std::span<create_table const>(&t, 1));
}

}

TEST(KeyDomain, ABestTablesKeyThatDeclaresNoDomainIsRefused)
{
	auto none = OneBest(key_domain{});
	EXPECT_TRUE(Mentions(RefusalOf(none), "key 'label' of best table 'lookup' declares no domain")) << RefusalOf(none);
	EXPECT_EQ(RefusalOf(OneBest(key_domain::Rows())), "(accepted)");
}

TEST(KeyDomain, OnlyABestTablesKeyDeclaresADomain)
{
	auto onAll = OneBest(key_domain::Rows(), column_type::Int, pick_policy::All);
	EXPECT_TRUE(Mentions(RefusalOf(onAll), "only a best table's keys")) << RefusalOf(onAll);

	add_column const columns[] = {
		{"label", column_type::Int, true, UINT32_MAX, key_domain::Rows()},
		{"clip",  column_type::Int, false, UINT32_MAX, key_domain::Rows()},
	};
	uint32_t const     byLabel[] = {0};
	order_by const     order{byLabel, false};
	create_table const t{"lookup", columns, &order, true, {}, pick_policy::Best};
	auto onValue = BuildSchema(std::span<create_table const>(&t, 1));
	EXPECT_TRUE(Mentions(RefusalOf(onValue), "only a key has one")) << RefusalOf(onValue);
}

TEST(KeyDomain, MembersAreDistinct)
{
	cell const twice[] = {cell::Integer(3), cell::Integer(1), cell::Integer(3)};
	auto built = OneBest(key_domain{domain_kind::Members, twice});
	EXPECT_TRUE(Mentions(RefusalOf(built), "lists member 3 twice")) << RefusalOf(built);

	cell const once[] = {cell::Integer(3), cell::Integer(1)};
	auto ok = OneBest(key_domain{domain_kind::Members, once});
	ASSERT_TRUE(ok.has_value()) << RefusalOf(ok);
	domain_def const& d = ok->tables()[0].domains[0];
	EXPECT_EQ(d.kind, domain_kind::Members);
	ASSERT_EQ(d.members.size(), 2u);
	EXPECT_EQ(d.members[0], 1) << "sorted";
	EXPECT_EQ(d.members[1], 3);

	auto empty = OneBest(key_domain{domain_kind::Members});
	EXPECT_TRUE(Mentions(RefusalOf(empty), "lists no members")) << RefusalOf(empty);
}

TEST(KeyDomain, MembersAreOnAHashOrAnIntKey)
{
	cell const members[] = {cell::Integer(1)};
	auto onFlags = OneBest(key_domain{domain_kind::Members, members}, column_type::Flags);
	EXPECT_TRUE(Mentions(RefusalOf(onFlags), "only a hash or an int key lists its members")) << RefusalOf(onFlags);
	EXPECT_EQ(RefusalOf(OneBest(key_domain{domain_kind::Members, members}, column_type::Hash)), "(accepted)");

	cell const wide[] = {cell::Integer(INT64_C(0x100000000))};
	auto tooWide = OneBest(key_domain{domain_kind::Members, wide}, column_type::Hash);
	EXPECT_TRUE(Mentions(RefusalOf(tooWide), "32-bit")) << RefusalOf(tooWide);
}

namespace
{

// kinds(kind, name): the foreign table, possibly declared after lookup.
tl::expected<schema, ErrorContext> WithKinds(key_domain const& domain, column_type kindType = column_type::Int,
	bool kindsFirst = false)
{
	add_column const lookupColumns[] = {
		{"label", column_type::Int, true, UINT32_MAX, domain},
		{"clip",  column_type::Int},
	};
	add_column const kindsColumns[] = {
		{"kind", kindType},
		{"name", column_type::Hash},
	};
	uint32_t const     byLabel[] = {0};
	order_by const     order{byLabel, false};
	create_table const lookup{"lookup", lookupColumns, &order, true, {}, pick_policy::Best};
	create_table const kinds{"kinds", kindsColumns, nullptr, true};
	std::array<create_table, 2> const tables = kindsFirst ? std::array<create_table, 2>{kinds, lookup}
	                                                      : std::array<create_table, 2>{lookup, kinds};
	return BuildSchema(tables);
}

}

TEST(KeyDomain, AForeignDomainNamesAnotherTablesColumn)
{
	auto later = WithKinds(key_domain{domain_kind::Column, {}, "kinds", "kind"});
	ASSERT_TRUE(later.has_value()) << RefusalOf(later) << "a domain may name a table later in the list";
	EXPECT_EQ(later->tables()[0].domains[0].table, 1u);
	EXPECT_EQ(later->tables()[0].domains[0].column, 0u);
	EXPECT_EQ(RefusalOf(WithKinds(key_domain{domain_kind::Column, {}, "kinds", "kind"}, column_type::Int, true)),
		"(accepted)");

	auto noTable = WithKinds(key_domain{domain_kind::Column, {}, "kins", "kind"});
	EXPECT_TRUE(Mentions(RefusalOf(noTable), "table 'kins', which the schema does not have")) << RefusalOf(noTable);
	auto noColumn = WithKinds(key_domain{domain_kind::Column, {}, "kinds", "knd"});
	EXPECT_TRUE(Mentions(RefusalOf(noColumn), "column 'knd' of table 'kinds', which has no such column"))
		<< RefusalOf(noColumn);
}

TEST(KeyDomain, AForeignDomainsColumnHasTheKeysType)
{
	auto wrong = WithKinds(key_domain{domain_kind::Column, {}, "kinds", "name"});
	EXPECT_TRUE(Mentions(RefusalOf(wrong), "is a int, and the column 'name' of table 'kinds' its domain comes from is a hash"))
		<< RefusalOf(wrong);
	auto flags = WithKinds(key_domain{domain_kind::Column, {}, "kinds", "kind"}, column_type::Flags);
	EXPECT_TRUE(Mentions(RefusalOf(flags), "is a flag set")) << RefusalOf(flags);
}

TEST(KeyDomain, DomainsTakenFromOtherTablesFormNoCycle)
{
	auto bestOf = [](std::string_view name, add_column const (&columns)[2], order_by const& order)
	{
		return create_table{name, columns, &order, true, {}, pick_policy::Best};
	};
	uint32_t const   byKey[] = {0};
	order_by const   order{byKey, false};
	add_column const aColumns[] = {{"k", column_type::Int, true, UINT32_MAX, {domain_kind::Column, {}, "b", "k"}},
	                               {"v", column_type::Int}};
	add_column const bColumns[] = {{"k", column_type::Int, true, UINT32_MAX, {domain_kind::Column, {}, "a", "k"}},
	                               {"v", column_type::Int}};
	create_table const pair[] = {bestOf("a", aColumns, order), bestOf("b", bColumns, order)};
	auto cycle = BuildSchema(pair);
	EXPECT_TRUE(Mentions(RefusalOf(cycle), "key domains form a cycle: 'a' -> 'b' -> 'a'")) << RefusalOf(cycle);

	add_column const selfColumns[] = {{"k", column_type::Int, true, UINT32_MAX, {domain_kind::Column, {}, "a", "v"}},
	                                  {"v", column_type::Int}};
	create_table const self[] = {bestOf("a", selfColumns, order)};
	auto own = BuildSchema(self);
	EXPECT_TRUE(Mentions(RefusalOf(own), "cycle: 'a' -> 'a'")) << RefusalOf(own);
}

// ---- YAML -------------------------------------------------------------------

TEST(KeyDomainYaml, TheThreeFormsRead)
{
	auto s = ReadSchema(R"(
[table display (column display hash) (pick none)]
[table by_rows (key k int (domain rows)) (order k)]
[table by_members
  (key mode hash (domain (members idle walk)))
  (key n int (domain (members 3 -1)))
  (order mode n)]
[table by_column (key d hash (domain (from display display))) (order d)]
)", "forms.tabular");
	ASSERT_TRUE(s.has_value()) << RefusalOf(s);
	EXPECT_EQ(s->tables()[1].domains[0].kind, domain_kind::Rows);
	domain_def const& mode = s->tables()[2].domains[0];
	ASSERT_EQ(mode.kind, domain_kind::Members);
	ASSERT_EQ(mode.members.size(), 2u);
	int64_t const idle = CityHash32("idle", 4);
	int64_t const walk = CityHash32("walk", 4);
	EXPECT_EQ(mode.members[0], std::min(idle, walk)) << "a hash member is its CityHash32, as a CSV cell is";
	EXPECT_EQ(mode.members[1], std::max(idle, walk));
	domain_def const& n = s->tables()[2].domains[1];
	ASSERT_EQ(n.members.size(), 2u);
	EXPECT_EQ(n.members[0], -1);
	EXPECT_EQ(n.members[1], 3);
	domain_def const& d = s->tables()[3].domains[0];
	EXPECT_EQ(d.kind, domain_kind::Column);
	EXPECT_EQ(d.table, 0u);
	EXPECT_EQ(d.column, 0u);
}

TEST(KeyDomainYaml, AMalformedDomainIsRefused)
{
	auto with = [](std::string_view domain)
	{
		return ReadSchema(std::string(R"(
[table display (column display hash) (pick none)]
[table t
  (key k hash )") + std::string(domain) + R"()
  (order k)]
)", "bad.tabular");
	};
	EXPECT_TRUE(Mentions(RefusalOf(with("(domain row)")), "has the unknown domain 'row'")) << RefusalOf(with("(domain row)"));
	EXPECT_TRUE(Mentions(RefusalOf(with("(domain (a b))")), "unknown domain form 'a'")) << RefusalOf(with("(domain (a b))"));
	EXPECT_TRUE(Mentions(RefusalOf(with("(domain (members a) (from display display))")), "one of the two"))
		<< RefusalOf(with("(domain (members a) (from display display))"));
	EXPECT_TRUE(Mentions(RefusalOf(with("(domain (from display))")), "names a table and its column, both"))
		<< RefusalOf(with("(domain (from display))"));
	EXPECT_TRUE(Mentions(RefusalOf(with("(domain (members a) unique)")), "unknown option 'unique'"))
		<< RefusalOf(with("(domain (members a) unique)"));
	EXPECT_TRUE(Mentions(RefusalOf(with("(domain rows (members a))")), "is rows and has more after it"))
		<< RefusalOf(with("(domain rows (members a))"));
	EXPECT_TRUE(Mentions(RefusalOf(with("(domain (members a a))")), "twice")) << RefusalOf(with("(domain (members a a))"));
	EXPECT_TRUE(Mentions(RefusalOf(ReadSchema(R"(
[table t (key k int) (order k)]
)", "none.tabular")), "declares no domain"));
}

// ---- Totality over the domain -----------------------------------------------

namespace
{

// lookup(label int key with members 1 and 2, req ranks by mode, when, clip).
constexpr std::string_view kMembers = R"(
[table lookup
  (input mode flags)
  (key label int (domain (members 1 2)))
  (column req flags (input mode))
  (column when condition)
  (column clip int)
  (order label)]
)";

struct lookup_row
{
	int64_t label;
	int64_t req;
	closure when;
	int64_t clip;
};

tl::expected<void, ErrorContext> LoadLookup(relation_store& store, std::vector<lookup_row> const& rows, uint64_t owner = 1,
	std::vector<run_note>* report = nullptr)
{
	std::vector<cell>    values;
	std::vector<closure> conditions;
	for (lookup_row const& r : rows)
	{
		values.push_back(cell::Integer(r.label));
		values.push_back(cell::Integer(r.req));
		values.push_back(cell::Integer(r.clip));
		conditions.push_back(r.when);
	}
	load_rows command{0, owner, rows.size(), values, {}, nullptr, conditions};
	command.report = report;
	return store.Load(command);
}

}

TEST(DomainTotality, AMemberWithNoRunIsRefusedNamingItsKey)
{
	relation_store store = StoreFrom(kMembers);
	auto loaded = LoadLookup(store, {{1, 0, closure{}, 10}});
	EXPECT_TRUE(Mentions(RefusalOf(loaded), "table 'lookup' key (2) has no row with no flags and no script condition"))
		<< RefusalOf(loaded);
	EXPECT_TRUE(Mentions(RefusalOf(loaded), "no row has this key, and the key is in the table's domain")) << RefusalOf(loaded);
	EXPECT_EQ(RefusalOf(LoadLookup(store, {{1, 0, closure{}, 10}, {2, 0, closure{}, 20}})), "(accepted)");
}

TEST(DomainTotality, ARunOutsideTheDomainIsAnOrphanReportedAndNotChecked)
{
	relation_store        store = StoreFrom(kMembers);
	int                   released = 0;
	std::vector<run_note> report;
	// Key 3 is no member; its only row is scripted, so it would not be total.
	auto loaded = LoadLookup(store, {{1, 0, closure{}, 10}, {2, 0, closure{}, 20}, {3, 0, MakeProbe(5, &released), 30}},
		1, &report);
	ASSERT_EQ(RefusalOf(loaded), "(accepted)");
	EXPECT_EQ(Notes(report), (std::vector<std::string>{"0 orphan (3)"}));
	EXPECT_EQ(store.RowCount(0), 3u) << "an orphan's rows are kept";
	EXPECT_EQ(released, 0);
}

namespace
{

// Loads rows of int cells, as many per row as the table has columns.
tl::expected<void, ErrorContext> LoadTuples(relation_store& store, std::vector<int64_t> const& values,
	std::vector<run_note>* report = nullptr)
{
	std::vector<cell> cells;
	for (int64_t v : values)
	{
		cells.push_back(cell::Integer(v));
	}
	size_t const width = store.definition().tables()[0].columns.size();
	load_rows command{0, 1, cells.size() / width, cells};
	command.report = report;
	return store.Load(command);
}

}

TEST(DomainTotality, KeysOverTheirRowsTogetherAreExactlyTheTuplesTheRowsName)
{
	relation_store store = StoreFrom(R"(
[table lookup
  (key a int (domain rows))
  (key b int (domain rows))
  (column clip int)
  (order a b)]
)");
	std::vector<run_note> report{run_note{}};
	// A product of each column's values would demand (1, 2) and (2, 1), which
	// no source declares.
	EXPECT_EQ(RefusalOf(LoadTuples(store, {1, 1, 11, 2, 2, 22}, &report)), "(accepted)");
	EXPECT_TRUE(report.empty()) << "every run is in the domain, and the domain has no tuple without a run";
}

TEST(DomainTotality, ARowsKeyBesideAMembersKeyDemandsEveryMemberForEachValueTheRowsName)
{
	relation_store store = StoreFrom(R"(
[table lookup
  (key a int (domain rows))
  (key b int (domain (members 1 2)))
  (column clip int)
  (order a b)]
)");
	auto loaded = LoadTuples(store, {1, 1, 11});
	EXPECT_TRUE(Mentions(RefusalOf(loaded), "table 'lookup' key (1, 2) has no row with no script condition; no row has this key"))
		<< RefusalOf(loaded);
	std::vector<run_note> report;
	EXPECT_EQ(RefusalOf(LoadTuples(store, {1, 1, 11, 1, 2, 12, 3, 1, 31, 3, 2, 32, 3, 3, 33}, &report)), "(accepted)");
	EXPECT_EQ(Notes(report), (std::vector<std::string>{"0 orphan (3, 3)"})) << "3 is named by the rows; member 3 is not";
}

TEST(DomainTotality, RowsKeysAfterAMembersKeyProjectOutOfRowOrder)
{
	relation_store store = StoreFrom(R"(
[table lookup
  (key a int (domain (members 1 2)))
  (key b int (domain rows))
  (key c int (domain rows))
  (column clip int)
  (order a b c)]
)");
	// The rows name (b, c) = (5, 1) under a = 1 and (3, 9) under a = 2; each is
	// demanded under both members, and (3, 9) sorts first.
	auto loaded = LoadTuples(store, {1, 5, 1, 151, 2, 3, 9, 239});
	EXPECT_TRUE(Mentions(RefusalOf(loaded), "table 'lookup' key (1, 3, 9) has no row with no script condition"))
		<< RefusalOf(loaded);
	// In row order the projection is (3, 9), (5, 1), (3, 9), (5, 1): a copy that
	// is not dropped would be a second tuple with no run left to match it.
	std::vector<run_note> report{run_note{}};
	EXPECT_EQ(RefusalOf(LoadTuples(store, {1, 5, 1, 151, 2, 3, 9, 239, 1, 3, 9, 139, 2, 5, 1, 251}, &report)), "(accepted)");
	EXPECT_TRUE(report.empty()) << Notes(report).size();
}

TEST(DomainTotality, ABareRetractIsNeverRefusedAndReportsWhatItLeaves)
{
	relation_store store = StoreFrom(kMembers);
	// Every load publishes a total version, so owner 1 brings both members' defaults.
	ASSERT_EQ(RefusalOf(LoadLookup(store, {{1, 0, closure{}, 10}, {2, 0, closure{}, 20}}, 1)), "(accepted)");
	ASSERT_EQ(RefusalOf(LoadLookup(store, {{1, 4, closure{}, 14}}, 2)), "(accepted)") << "owner 2 ranks key 1";

	std::vector<run_note> report;
	store.Retract(retract_owner{1, &report});
	EXPECT_EQ(Notes(report), (std::vector<std::string>{"0 not total (1)", "0 not total (2)"}))
		<< "key 1 keeps only a ranked row, and key 2 has no run";

	store.Retract(retract_owner{2, &report});
	EXPECT_EQ(Notes(report), (std::vector<std::string>{"0 not total (1)", "0 not total (2)"}))
		<< "the members stay in the domain with no run";
	EXPECT_EQ(store.RowCount(0), 0u);

	report.push_back(run_note{});
	store.Retract(retract_owner{2, &report});
	EXPECT_TRUE(report.empty()) << "a report says what this change left; a retract that changed nothing touched no table";
}

TEST(DomainTotality, ALoadOfNoRowsIsABareRetractAndReportsToo)
{
	relation_store store = StoreFrom(kMembers);
	ASSERT_EQ(RefusalOf(LoadLookup(store, {{1, 0, closure{}, 10}, {2, 0, closure{}, 20}}, 1)), "(accepted)");
	std::vector<run_note> report;
	ASSERT_EQ(RefusalOf(LoadLookup(store, {}, 1, &report)), "(accepted)");
	EXPECT_EQ(Notes(report), (std::vector<std::string>{"0 not total (1)", "0 not total (2)"}));
}

TEST(DomainTotality, ARefusedChangeLeavesTheReportEmpty)
{
	relation_store        store = StoreFrom(kMembers);
	std::vector<run_note> report{run_note{}};
	EXPECT_FALSE(LoadLookup(store, {{1, 0, closure{}, 10}, {3, 0, closure{}, 30}}, 1, &report).has_value());
	EXPECT_TRUE(report.empty());
}

TEST(DomainTotality, ALoadRefusedAfterItsOrphansWereNotedLeavesTheReportEmpty)
{
	relation_store        store = StoreFrom(kMembers);
	std::vector<run_note> report;
	// Key 0 is no member and sorts first, so it is noted before member 2 is
	// found to have no run.
	auto loaded = LoadLookup(store, {{0, 0, closure{}, 0}, {1, 0, closure{}, 10}}, 1, &report);
	EXPECT_TRUE(Mentions(RefusalOf(loaded), "key (2) has no row")) << RefusalOf(loaded);
	EXPECT_TRUE(report.empty()) << Notes(report).size();
}

// ---- A foreign domain follows the other table ------------------------------

namespace
{

// kinds(kind) is read-only; clips(kind, clip) takes its keys from it.
constexpr std::string_view kKinds = R"(
[table kinds (column kind int) (pick none)]
[table clips
  (key kind int (domain (from kinds kind)))
  (column clip int)
  (order kind)]
)";

tl::expected<void, ErrorContext> LoadInts(relation_store& store, uint32_t table, uint64_t owner,
	std::vector<int64_t> const& values, std::vector<run_note>* report = nullptr)
{
	std::vector<cell> cells;
	for (int64_t v : values)
	{
		cells.push_back(cell::Integer(v));
	}
	size_t const width = store.definition().tables()[table].columns.size();
	load_rows command{table, owner, cells.size() / width, cells};
	command.report = report;
	return store.Load(command);
}

}

TEST(DomainTotality, AChangeToTheForeignTableRechecksItsDependents)
{
	relation_store        store = StoreFrom(kKinds);
	std::vector<run_note> report;
	// No kinds yet: clips' runs are all orphans, accepted and reported.
	ASSERT_EQ(RefusalOf(LoadInts(store, 1, 1, {1, 10}, &report)), "(accepted)");
	EXPECT_EQ(Notes(report), (std::vector<std::string>{"1 orphan (1)"}));

	// Kind 2 would enter the domain with no run in clips.
	auto refused = LoadInts(store, 0, 7, {1, 2});
	EXPECT_TRUE(Mentions(RefusalOf(refused), "this load changes table 'kinds'")) << RefusalOf(refused);
	EXPECT_TRUE(Mentions(RefusalOf(refused), "table 'clips' takes a key domain from table 'kinds', and is not total"))
		<< RefusalOf(refused);
	EXPECT_TRUE(Mentions(RefusalOf(refused), "table 'clips' key (2) has no row with no script condition"))
		<< RefusalOf(refused);
	EXPECT_EQ(store.RowCount(0), 0u) << "the refused load left kinds as it was";

	ASSERT_EQ(RefusalOf(LoadInts(store, 1, 1, {1, 10, 2, 20})), "(accepted)");
	ASSERT_EQ(RefusalOf(LoadInts(store, 0, 7, {1, 2}, &report)), "(accepted)");
	EXPECT_TRUE(report.empty()) << Notes(report).size();

	// The foreign rows go: clips' runs become orphans, and nothing is refused.
	store.Retract(retract_owner{7, &report});
	EXPECT_EQ(Notes(report), (std::vector<std::string>{"1 orphan (1)", "1 orphan (2)"}));
}

TEST(DomainTotality, ARunEnteringTheDomainIsCheckedWhole)
{
	relation_store store = StoreFrom(kKinds);
	// Kinds 3 and 4 are outside the domain, so neither run is checked: owners 1
	// and 2 both claim kind 3, and owner 1's second kind-4 row is dead.
	ASSERT_EQ(RefusalOf(LoadInts(store, 1, 1, {3, 31, 4, 41, 4, 42})), "(accepted)");
	ASSERT_EQ(RefusalOf(LoadInts(store, 1, 2, {3, 32})), "(accepted)");

	auto overlap = LoadInts(store, 0, 7, {3});
	EXPECT_TRUE(Mentions(RefusalOf(overlap), "table 'clips' key (3): owner 1's row with key (3) and owner 2's row with key (3) overlap"))
		<< RefusalOf(overlap);
	auto dead = LoadInts(store, 0, 7, {4});
	EXPECT_TRUE(Mentions(RefusalOf(dead), "table 'clips' key (4): owner 1's row with key (4) is never picked; owner 1's row with key (4) covers it"))
		<< RefusalOf(dead);
}

// kinds(kind) and clips(kind over kinds.kind, clip): clips is the mutable table when
// kinds is read-only, and the other way round. Mutable tables cannot be declared
// in a file, so the schema is built from commands.
static relation_store ClipsOverKinds(bool mutableClips)
{
	static add_column const kinds[] = {{"kind", column_type::Int}};
	static add_column const clips[] = {{"kind", column_type::Int, true, UINT32_MAX,
	                                    key_domain{domain_kind::Column, {}, "kinds", "kind"}},
	                                   {"clip", column_type::Int}};
	static uint32_t const   byKind[] = {0};
	static order_by const   order{byKind, false};
	create_table const      tables[] = {{"kinds", kinds, nullptr, mutableClips},
	                                    {"clips", clips, &order, !mutableClips, {}, pick_policy::Best}};
	auto s = BuildSchema(tables);
	EXPECT_TRUE(s.has_value()) << RefusalOf(s);
	return relation_store(std::move(*s));
}

TEST(DomainTotality, AMutableForeignTableRechecksOnInsert)
{
	relation_store store = ClipsOverKinds(false);
	cell const two[] = {cell::Integer(2)};
	auto refused = store.Insert(insert_row{0, 7, two});
	EXPECT_TRUE(Mentions(RefusalOf(refused), "table 'clips' key (2)")) << RefusalOf(refused);
	EXPECT_EQ(store.RowCount(0), 0u) << "the refused row was taken out again";
	EXPECT_EQ(store.Generation(0), 0u);

	ASSERT_EQ(RefusalOf(LoadInts(store, 1, 1, {2, 20})), "(accepted)");
	EXPECT_EQ(RefusalOf(store.Insert(insert_row{0, 7, two})), "(accepted)");
	EXPECT_EQ(store.RowCount(0), 1u);
}

TEST(DomainTotality, AnInsertRefusedAfterItsOrphansWereNotedLeavesTheReportEmpty)
{
	relation_store store = ClipsOverKinds(true);
	cell const one[]  = {cell::Integer(1), cell::Integer(10)};
	cell const five[] = {cell::Integer(5), cell::Integer(50)};
	ASSERT_EQ(RefusalOf(store.Insert(insert_row{1, 1, one})), "(accepted)") << "an orphan";
	ASSERT_EQ(RefusalOf(store.Insert(insert_row{1, 1, five})), "(accepted)");
	ASSERT_EQ(RefusalOf(LoadInts(store, 0, 7, {5})), "(accepted)");

	// Orphan 1 sorts before key 5, whose two owners' rows then overlap.
	cell const          clash[] = {cell::Integer(5), cell::Integer(51)};
	std::vector<run_note> report{run_note{}};
	insert_row command{1, 2, clash};
	command.report = &report;
	auto refused = store.Insert(command);
	EXPECT_TRUE(Mentions(RefusalOf(refused), "overlap")) << RefusalOf(refused);
	EXPECT_TRUE(report.empty()) << Notes(report).size();
}

TEST(DomainTotality, AStrandedDependentRefusesEveryChangeToItsSourceThatBringsRows)
{
	relation_store store = StoreFrom(kKinds);
	ASSERT_EQ(RefusalOf(LoadInts(store, 1, 1, {1, 10})), "(accepted)");
	ASSERT_EQ(RefusalOf(LoadInts(store, 1, 2, {2, 20})), "(accepted)");
	ASSERT_EQ(RefusalOf(LoadInts(store, 0, 7, {1, 2})), "(accepted)");
	std::vector<run_note> report;
	store.Retract(retract_owner{2, &report});
	EXPECT_EQ(Notes(report), (std::vector<std::string>{"1 not total (2)"}));

	// Owner 8's kinds name nothing new, and clips' gap is older than the load;
	// the refusal says clips is not total rather than blame the load.
	auto refused = LoadInts(store, 0, 8, {1});
	EXPECT_TRUE(Mentions(RefusalOf(refused), "this load changes table 'kinds'")) << RefusalOf(refused);
	EXPECT_TRUE(Mentions(RefusalOf(refused), "table 'clips' takes a key domain from table 'kinds', and is not total"))
		<< RefusalOf(refused);
	EXPECT_TRUE(Mentions(RefusalOf(refused),
		"table 'clips' key (2) has no row with no script condition; no row has this key, and the key is in the table's domain"))
		<< RefusalOf(refused);
	EXPECT_EQ(store.RowCount(0), 2u);

	ASSERT_EQ(RefusalOf(LoadInts(store, 1, 2, {2, 20})), "(accepted)") << "clips made total again";
	EXPECT_EQ(RefusalOf(LoadInts(store, 0, 8, {1})), "(accepted)");
}

TEST(DomainTotality, AStrandedRunOfADependentNamesItsFirstRowNotTheSourceChange)
{
	relation_store store = StoreFrom(R"(
[table kinds (column kind int) (pick none)]
[table clips
  (input t double 0..1)
  (key kind int (domain (from kinds kind)))
  (column window band (input t))
  (column clip int)
  (order kind)]
)");
	auto loadClip = [&](uint64_t owner, float lo, float hi)
	{
		cell const values[] = {cell::Integer(2), cell::Vector(lo, hi), cell::Integer(static_cast<int64_t>(owner))};
		return store.Load(load_rows{1, owner, 1, values});
	};
	ASSERT_EQ(RefusalOf(loadClip(1, 0.0f, 0.5f)), "(accepted)");
	ASSERT_EQ(RefusalOf(loadClip(2, 0.5f, 1.0f)), "(accepted)");
	ASSERT_EQ(RefusalOf(LoadInts(store, 0, 7, {2})), "(accepted)");
	store.Retract(retract_owner{2});

	auto refused = LoadInts(store, 0, 8, {2});
	EXPECT_TRUE(Mentions(RefusalOf(refused), "table 'clips' takes a key domain from table 'kinds', and is not total"))
		<< RefusalOf(refused);
	EXPECT_TRUE(Mentions(RefusalOf(refused),
		"table 'clips' key (2): t 0.5 has no row with no script condition; its first row is owner 1's row with key (2), window 0..0.5"))
		<< RefusalOf(refused);
	EXPECT_FALSE(Mentions(RefusalOf(refused), "brings the key no row")) << RefusalOf(refused);
}

// ---- Mutable best tables ----------------------------------------------------

namespace
{

// rules(topic hash key over its rows, need ranks by live, when, answer):
// mutable, so contributors insert.
// Mutable tables cannot be declared in a file, so the schema is built from commands.
relation_store RulesStore()
{
	static add_input const  inputs[] = {{"live", column_type::Flags}, {"t", column_type::Double, true, 0.0f, 1.0f}};
	static add_column const columns[] = {{"topic", column_type::Int, true, UINT32_MAX, key_domain::Rows()},
	                                     {"need", column_type::Flags, false, 0},
	                                     {"window", column_type::Band, false, 1},
	                                     {"when", column_type::Condition},
	                                     {"answer", column_type::Int}};
	static uint32_t const   byTopic[] = {0};
	static order_by const   order{byTopic, false};
	create_table const      rules{"rules", columns, &order, false, inputs, pick_policy::Best};
	auto s = BuildSchema(std::span<create_table const>(&rules, 1));
	EXPECT_TRUE(s.has_value()) << RefusalOf(s);
	return relation_store(std::move(*s));
}

tl::expected<void, ErrorContext> InsertRule(relation_store& store, uint64_t owner, int64_t topic, int64_t need,
	float lo, float hi, closure when, int64_t answer, std::vector<run_note>* report = nullptr)
{
	cell const    values[]     = {cell::Integer(topic), cell::Integer(need), cell::Vector(lo, hi), cell::Integer(answer)};
	closure const conditions[] = {when};
	insert_row command{0, owner, values, conditions};
	command.report = report;
	return store.Insert(command);
}

int64_t Answer(relation_store const& store, int64_t topic, int64_t live, double t)
{
	cell const keys[]   = {cell::Integer(topic)};
	cell const inputs[] = {cell::Integer(live), cell::Real(t)};
	int64_t    scratch  = 0;
	auto picked = PickBest(store, 0, pick_query{keys, inputs, &scratch});
	EXPECT_TRUE(picked.has_value()) << RefusalOf(picked);
	if (!picked || !*picked)
	{
		return -1;
	}
	auto answer = store.Value(0, **picked, 4);
	return answer ? answer->i : -1;
}

}

TEST(MutableBest, AMutableBestTableIsPickedThroughAVersion)
{
	relation_store store = RulesStore();
	ASSERT_EQ(RefusalOf(InsertRule(store, 1, 5, 0, 0.0f, 1.0f, closure{}, 50)), "(accepted)");
	ASSERT_EQ(RefusalOf(InsertRule(store, 2, 5, 1, 0.0f, 0.5f, closure{}, 51)), "(accepted)");
	auto held = store.Version(0);
	ASSERT_TRUE(held.has_value()) << RefusalOf(held);
	EXPECT_EQ(Answer(store, 5, 1, 0.25), 51);
	EXPECT_EQ(Answer(store, 5, 1, 0.75), 50);
	EXPECT_EQ(Answer(store, 5, 0, 0.25), 50);
	ASSERT_EQ(RefusalOf(InsertRule(store, 3, 5, 2, 0.0f, 1.0f, closure{}, 52)), "(accepted)");
	EXPECT_EQ((*held)->RowCount(), 2u) << "a held version never changes";
	EXPECT_EQ(store.RowCount(0), 3u);
	EXPECT_FALSE(store.Load(load_rows{0, 1, 0, {}}).has_value()) << "a mutable table refuses a load";
}

TEST(MutableBest, AnInsertThatLeavesARunNotTotalIsRefusedAndReleased)
{
	relation_store store    = RulesStore();
	int            released = 0;
	auto ranked = InsertRule(store, 1, 5, 1, 0.0f, 1.0f, MakeProbe(0, &released), 51);
	EXPECT_TRUE(Mentions(RefusalOf(ranked), "table 'rules' key (5): t 0 has no row with no flags and no script condition"))
		<< RefusalOf(ranked);
	EXPECT_EQ(released, 1);
	auto partial = InsertRule(store, 1, 5, 0, 0.0f, 0.5f, closure{}, 50);
	EXPECT_TRUE(Mentions(RefusalOf(partial), "t 0.5 has no row")) << RefusalOf(partial);
	EXPECT_EQ(store.RowCount(0), 0u);
	EXPECT_EQ(store.Generation(0), 0u);
}

TEST(MutableBest, AnInsertedRowAnEarlierRowCoversIsRefused)
{
	relation_store store = RulesStore();
	ASSERT_EQ(RefusalOf(InsertRule(store, 1, 5, 0, 0.0f, 1.0f, closure{}, 50)), "(accepted)");
	auto dead = InsertRule(store, 1, 5, 0, 0.25f, 0.5f, closure{}, 55);
	EXPECT_TRUE(Mentions(RefusalOf(dead), "inserted row 0 is never picked; owner 1's row with key (5), flags 0, window 0..1 covers it"))
		<< RefusalOf(dead);
	EXPECT_EQ(store.RowCount(0), 1u);
}

TEST(MutableBest, TwoOwnersRowsWithEqualFlagsMayNotOverlap)
{
	relation_store store = RulesStore();
	ASSERT_EQ(RefusalOf(InsertRule(store, 1, 5, 0, 0.0f, 1.0f, closure{}, 50)), "(accepted)");
	ASSERT_EQ(RefusalOf(InsertRule(store, 1, 5, 2, 0.0f, 0.5f, closure{}, 52)), "(accepted)");
	auto clash = InsertRule(store, 2, 5, 2, 0.25f, 1.0f, closure{}, 62);
	EXPECT_TRUE(Mentions(RefusalOf(clash), "owner 1's row with key (5), flags 2, window 0..0.5 and owner 2's inserted row 0 have equal flags and overlap"))
		<< RefusalOf(clash);
	EXPECT_EQ(RefusalOf(InsertRule(store, 2, 5, 2, 0.5f, 1.0f, closure{}, 62)), "(accepted)") << "beside it, not over it";
}

TEST(MutableBest, RetractThenReinsertRestoresTheVersion)
{
	relation_store store = RulesStore();
	// Every insert publishes a total version: owner 2 brings the default, and
	// owner 1 ranked rows.
	auto fill = [&](uint64_t owner)
	{
		if (owner == 2)
		{
			ASSERT_EQ(RefusalOf(InsertRule(store, owner, 5, 0, 0.0f, 1.0f, closure{}, 20)), "(accepted)");
			ASSERT_EQ(RefusalOf(InsertRule(store, owner, 5, 8, 0.0f, 1.0f, closure{}, 21)), "(accepted)");
			return;
		}
		ASSERT_EQ(RefusalOf(InsertRule(store, owner, 5, 4, 0.0f, 0.5f, closure{}, 10)), "(accepted)");
		ASSERT_EQ(RefusalOf(InsertRule(store, owner, 5, 4, 0.5f, 1.0f, closure{}, 11)), "(accepted)");
	};
	auto rows = [&]()
	{
		std::vector<std::pair<uint64_t, int64_t>> out;
		for (size_t r = 0; r < store.RowCount(0); ++r)
		{
			out.push_back({store.OwnerOf(0, r), store.Value(0, r, 4)->i});
		}
		return out;
	};
	fill(2);
	fill(1);
	auto const before = rows();
	EXPECT_EQ(before, (std::vector<std::pair<uint64_t, int64_t>>{{1, 10}, {1, 11}, {2, 20}, {2, 21}}))
		<< "rows sit by owner, then the owner's insertion order";
	store.Retract(retract_owner{1});
	fill(1);
	EXPECT_EQ(rows(), before) << "a retract and the same inserts again give the same version";
}

// The sort that places an insert keeps equal rows where they were up to 16
// rows, so these tables are larger: only the insert's index among the owner's
// rows orders it. The first is sized so that, were that index lost, Y (last
// before the sort) would be the median of three and sorted ahead of X.
TEST(MutableBest, AnOwnersInsertsAreTriedInTheOrderTheyWereInserted)
{
	relation_store store = RulesStore();
	for (int64_t topic : {10, 11, 12, 13, 14, 15, 16, 17, 60, 61, 62, 63, 64, 65, 66, 67})
	{
		ASSERT_EQ(RefusalOf(InsertRule(store, 1, topic, 0, 0.0f, 1.0f, closure{}, topic)), "(accepted)");
	}
	ASSERT_EQ(RefusalOf(InsertRule(store, 1, 50, 0, 0.0f, 1.0f, closure{}, 50)), "(accepted)");
	// X first, then Y over X's inputs and more: X decides where both hold.
	ASSERT_EQ(RefusalOf(InsertRule(store, 1, 50, 2, 0.0f, 0.5f, closure{}, 1)), "(accepted)") << "X";
	ASSERT_EQ(RefusalOf(InsertRule(store, 1, 50, 2, 0.0f, 1.0f, closure{}, 2)), "(accepted)") << "Y is live past 0.5";
	EXPECT_EQ(Answer(store, 50, 2, 0.25), 1) << "X was inserted first";
	EXPECT_EQ(Answer(store, 50, 2, 0.75), 2);
}

TEST(MutableBest, ManyInsertsWithOneKeyKeepTheirInsertionOrder)
{
	relation_store store    = RulesStore();
	int            released = 0;
	ASSERT_EQ(RefusalOf(InsertRule(store, 1, 5, 0, 0.0f, 1.0f, closure{}, 0)), "(accepted)");
	// Scripted, so none closes a list and none is dead.
	for (int64_t i = 1; i <= 20; ++i)
	{
		ASSERT_EQ(RefusalOf(InsertRule(store, 1, 5, 2, 0.0f, 1.0f, MakeProbe(i, &released), i)), "(accepted)");
	}
	ASSERT_EQ(store.RowCount(0), 21u);
	for (size_t r = 0; r < store.RowCount(0); ++r)
	{
		EXPECT_EQ(store.Value(0, r, 4)->i, static_cast<int64_t>(r)) << "row " << r;
	}
}

TEST(MutableBest, RetractIsUncheckedAndReports)
{
	relation_store store = RulesStore();
	ASSERT_EQ(RefusalOf(InsertRule(store, 1, 5, 0, 0.0f, 1.0f, closure{}, 50)), "(accepted)");
	ASSERT_EQ(RefusalOf(InsertRule(store, 2, 5, 4, 0.0f, 1.0f, closure{}, 54)), "(accepted)");
	std::vector<run_note> report;
	store.Retract(retract_owner{1, &report});
	EXPECT_EQ(store.RowCount(0), 1u);
	EXPECT_EQ(Notes(report), (std::vector<std::string>{"0 not total (5)"}));
}
