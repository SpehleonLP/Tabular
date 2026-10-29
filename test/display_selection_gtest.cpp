#include <gtest/gtest.h>
#include "tabular/document.h"
#include "tabular/pick.h"
#include "tabular/relation.h"
#include "cityhash/src/city.h"

#include <string>
#include <string_view>
#include <vector>

using namespace tabular;

// The display-selection example (docs/arcs/input-surfaces.md, "Schema sketch")
// as two mutable best tables. display(display, host) is who contributes each
// display; owner(display, needs, object) picks who gets a display's hits. The
// row's owner is the sketch's `by`, so neither table needs a column for it.
// owner.display takes its domain from display.display, so adding a display
// demands an owner row for it, and retracting one orphans its owner rows.

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

// Mutable tables cannot be declared in a file, so the schema is built from
// commands.
tl::expected<schema, ErrorContext> BuildTables()
{
	static add_column const displayColumns[] = {{"display", column_type::Hash, true, UINT32_MAX, key_domain::Rows()},
	                                            {"host", column_type::Hash}};
	static uint32_t const   byDisplay[]      = {0};
	static order_by const   order{byDisplay, false};
	static add_input const  ownerInputs[]    = {{"live", column_type::Flags}};
	static add_column const ownerColumns[]   = {
		{"display", column_type::Hash, true, UINT32_MAX, key_domain{domain_kind::Column, {}, "display", "display"}},
		{"needs", column_type::Flags, false, 0},
		{"object", column_type::Hash}};
	create_table const tables[] = {
		{"display", displayColumns, &order, false, {}, pick_policy::Best},
		{"owner", ownerColumns, &order, false, ownerInputs, pick_policy::Best},
	};
	return BuildSchema(tables);
}

enum table_id : uint32_t { Display, Owner };

// Contributors. The engine owner supplies every display's default owner row.
enum by : uint64_t { Engine = 1, Lifaundi = 2, Crafting = 3, TabPackage = 4, WindowHost = 100, TabHost = 101, DockHost = 102 };

// Condition keys, highest bit most specific: crafting outranks player mode.
constexpr int64_t kPlayer   = 1;
constexpr int64_t kCrafting = 2;

int64_t H(std::string_view text)
{
	return static_cast<int64_t>(CityHash32(text.data(), text.size()));
}

tl::expected<void, ErrorContext> AddDisplay(relation_store& store, uint64_t by, std::string_view display,
	std::vector<run_note>* report = nullptr)
{
	cell const values[] = {cell::Integer(H(display)), cell::Integer(H("main"))};
	insert_row command{Display, by, values};
	command.report = report;
	return store.Insert(command);
}

tl::expected<void, ErrorContext> AddOwner(relation_store& store, uint64_t by, std::string_view display, int64_t needs,
	std::string_view object, std::vector<run_note>* report = nullptr)
{
	cell const values[] = {cell::Integer(H(display)), cell::Integer(needs), cell::Integer(H(object))};
	insert_row command{Owner, by, values};
	command.report = report;
	return store.Insert(command);
}

// Who gets the display's hits, as the hash of the object's name; 0 for none.
int64_t OwnerOf(relation_store const& store, std::string_view display, int64_t live)
{
	cell const keys[]   = {cell::Integer(H(display))};
	cell const inputs[] = {cell::Integer(live)};
	auto picked = PickBest(store, Owner, pick_query{keys, inputs});
	EXPECT_TRUE(picked.has_value()) << RefusalOf(picked);
	if (!picked || !*picked)
	{
		return 0;
	}
	return store.Value(Owner, **picked, 2)->i;
}

// Lifaundi in player mode with crafting installed, on the window.
relation_store Booted()
{
	auto s = BuildTables();
	EXPECT_TRUE(s.has_value()) << RefusalOf(s);
	relation_store store(std::move(*s));
	EXPECT_EQ(RefusalOf(AddOwner(store, Engine, "window", 0, "hand")), "(accepted)");
	EXPECT_EQ(RefusalOf(AddDisplay(store, WindowHost, "window")), "(accepted)");
	EXPECT_EQ(RefusalOf(AddOwner(store, Lifaundi, "window", kPlayer, "pointing")), "(accepted)");
	EXPECT_EQ(RefusalOf(AddOwner(store, Crafting, "window", kCrafting, "crafting_vm")), "(accepted)");
	return store;
}

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

}

TEST(DisplaySelection, CraftingRetractedFallsBackToPointing)
{
	relation_store store = Booted();
	EXPECT_EQ(OwnerOf(store, "window", kPlayer | kCrafting), H("crafting_vm"));
	std::vector<run_note> report;
	store.Retract(retract_owner{Crafting, &report});
	EXPECT_EQ(OwnerOf(store, "window", kPlayer | kCrafting), H("pointing"));
	EXPECT_TRUE(report.empty()) << Notes(report).size() << " notes: the window's run is still total";
}

TEST(DisplaySelection, LifaundiRetractedLeavesCraftingInertAndTheEngineDefault)
{
	relation_store        store = Booted();
	std::vector<run_note> report;
	store.Retract(retract_owner{Lifaundi, &report});
	EXPECT_TRUE(report.empty()) << Notes(report).size() << " notes: the engine's default keeps the run total";
	EXPECT_EQ(OwnerOf(store, "window", kPlayer), H("hand")) << "crafting's row needs crafting, which is not live";
	EXPECT_EQ(OwnerOf(store, "window", kPlayer | kCrafting), H("crafting_vm"));
}

TEST(DisplaySelection, TheTabsDisplayRetractedOrphansItsOwnerRows)
{
	relation_store store = Booted();
	ASSERT_EQ(RefusalOf(AddOwner(store, Engine, "tab", 0, "hand")), "(accepted)");
	ASSERT_EQ(RefusalOf(AddDisplay(store, TabHost, "tab")), "(accepted)");
	ASSERT_EQ(RefusalOf(AddOwner(store, TabPackage, "tab", kPlayer, "tab_tool")), "(accepted)");
	EXPECT_EQ(OwnerOf(store, "tab", kPlayer), H("tab_tool"));

	std::vector<run_note> report;
	store.Retract(retract_owner{TabHost, &report});
	EXPECT_EQ(Notes(report), (std::vector<std::string>{"1 orphan (" + std::to_string(H("tab")) + ")"}));
	EXPECT_EQ(store.RowCount(Owner), 5u) << "an orphan is reported, never refused or cascaded";
	EXPECT_EQ(OwnerOf(store, "window", kPlayer), H("pointing"));
}

TEST(DisplaySelection, ADisplayBeforeItsDefaultOwnerRowIsRefusedNamingOwner)
{
	relation_store store = Booted();
	auto added = AddDisplay(store, DockHost, "dock");
	EXPECT_TRUE(Mentions(RefusalOf(added), "this insert changes table 'display'")) << RefusalOf(added);
	EXPECT_TRUE(Mentions(RefusalOf(added), "table 'owner' takes a key domain from table 'display', and is not total"))
		<< RefusalOf(added);
	EXPECT_TRUE(Mentions(RefusalOf(added), "table 'owner' key (" + std::to_string(H("dock")) + ") has no row with no flags"))
		<< RefusalOf(added);
	EXPECT_EQ(store.RowCount(Display), 1u);
}

TEST(DisplaySelection, TheDefaultFirstThenTheDisplayIsAccepted)
{
	relation_store        store = Booted();
	std::vector<run_note> report;
	ASSERT_EQ(RefusalOf(AddOwner(store, Engine, "dock", 0, "hand", &report)), "(accepted)");
	EXPECT_EQ(Notes(report), (std::vector<std::string>{"1 orphan (" + std::to_string(H("dock")) + ")"}))
		<< "a default for a display that does not exist yet is an orphan, allowed";
	ASSERT_EQ(RefusalOf(AddDisplay(store, DockHost, "dock", &report)), "(accepted)");
	EXPECT_TRUE(report.empty()) << Notes(report).size();
	EXPECT_EQ(OwnerOf(store, "dock", kPlayer), H("hand"));
}

TEST(DisplaySelection, TwoContributorsCannotClaimOneDisplay)
{
	relation_store store = Booted();
	auto twice = AddDisplay(store, DockHost, "window");
	EXPECT_TRUE(Mentions(RefusalOf(twice), "owner 100's row with key (" + std::to_string(H("window"))
		+ ") and owner 102's inserted row 0 overlap; two owners may not claim the same inputs")) << RefusalOf(twice);
}
