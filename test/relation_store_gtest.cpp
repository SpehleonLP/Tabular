#include <gtest/gtest.h>
#include "tabular/relation.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace tabular;

// The message mentions the text: errors must name what they refuse.
static bool Mentions(std::string const& message, std::string_view text)
{
	return message.find(text) != std::string::npos;
}

namespace
{

// spawn_points(package: hash key, section: hash key, x: double, y: double)
// flags_table(mask: flags)
relation_store MakeStore()
{
	static add_column const spawn[] = {
		{"package", column_type::Hash,   true},
		{"section", column_type::Hash,   true},
		{"x",       column_type::Double, false},
		{"y",       column_type::Double, false},
	};
	static add_column const flags[] = {{"mask", column_type::Flags, false}};
	create_table const tables[] = {{"spawn_points", spawn}, {"flags_table", flags}};
	auto s = BuildSchema(tables);
	EXPECT_TRUE(s.has_value());
	return relation_store(std::move(*s));
}

// Owns the cells an insert_row views, so a test can write one line per row.
struct spawn_row
{
	cell       values[4];
	insert_row row;

	spawn_row(uint64_t owner, int64_t package, int64_t section, double x, double y)
		: values{cell::Integer(package), cell::Integer(section), cell::Real(x), cell::Real(y)}
		, row{0, owner, values, {}, {}}
	{
	}
};

tl::expected<void, ErrorContext> InsertFlags(relation_store& store, uint64_t owner, int64_t mask)
{
	cell const values[] = {cell::Integer(mask)};
	return store.Insert(insert_row{1, owner, values, {}, {}});
}

int64_t IntAt(relation_store const& store, uint32_t table, size_t row, uint32_t column)
{
	auto v = store.Value(table, row, column);
	EXPECT_TRUE(v.has_value());
	return v ? v->i : INT64_MIN;
}

double RealAt(relation_store const& store, uint32_t table, size_t row, uint32_t column)
{
	auto v = store.Value(table, row, column);
	EXPECT_TRUE(v.has_value());
	return v ? v->d : -1.0;
}

}

TEST(RelationStore, StartsEmptyAtGenerationZero)
{
	relation_store store = MakeStore();
	EXPECT_EQ(store.RowCount(0), 0u);
	EXPECT_EQ(store.RowCount(1), 0u);
	EXPECT_EQ(store.Generation(0), 0u);
	EXPECT_EQ(store.definition().tables().size(), 2u);
}

TEST(RelationStore, InsertKeepsValuesOwnerAndOrder)
{
	relation_store store = MakeStore();
	ASSERT_TRUE(store.Insert(spawn_row(7, 100, 200, 1.5, 2.5).row).has_value());
	ASSERT_TRUE(store.Insert(spawn_row(8, 101, 201, 3.5, 4.5).row).has_value());

	ASSERT_EQ(store.RowCount(0), 2u);
	EXPECT_EQ(IntAt(store, 0, 0, 0), 100);
	EXPECT_EQ(IntAt(store, 0, 0, 1), 200);
	EXPECT_EQ(RealAt(store, 0, 0, 2), 1.5);
	EXPECT_EQ(RealAt(store, 0, 0, 3), 2.5);
	EXPECT_EQ(store.OwnerOf(0, 0), 7u);
	EXPECT_EQ(IntAt(store, 0, 1, 0), 101);
	EXPECT_EQ(store.OwnerOf(0, 1), 8u);
}

TEST(RelationStore, InsertedValuesAreCopied)
{
	relation_store store = MakeStore();
	{
		spawn_row r(1, 100, 200, 1.5, 2.5);
		ASSERT_TRUE(store.Insert(r.row).has_value());
		r.values[0] = cell::Integer(-1);
	}
	EXPECT_EQ(IntAt(store, 0, 0, 0), 100);
}

TEST(RelationStore, InsertChangesOnlyItsTablesGeneration)
{
	relation_store store = MakeStore();
	ASSERT_TRUE(store.Insert(spawn_row(7, 1, 2, 0, 0).row).has_value());
	EXPECT_NE(store.Generation(0), 0u);
	EXPECT_EQ(store.Generation(1), 0u);
}

TEST(RelationStore, RetractRemovesOnlyThatOwnerAndKeepsSurvivorOrder)
{
	relation_store store = MakeStore();
	ASSERT_TRUE(store.Insert(spawn_row(1, 10, 0, 0, 0).row).has_value());
	ASSERT_TRUE(store.Insert(spawn_row(2, 20, 0, 0, 0).row).has_value());
	ASSERT_TRUE(store.Insert(spawn_row(1, 30, 0, 0, 0).row).has_value());
	ASSERT_TRUE(store.Insert(spawn_row(3, 40, 0, 0, 0).row).has_value());

	store.Retract(retract_owner{1});

	ASSERT_EQ(store.RowCount(0), 2u);
	EXPECT_EQ(IntAt(store, 0, 0, 0), 20);
	EXPECT_EQ(IntAt(store, 0, 1, 0), 40);
	EXPECT_EQ(store.OwnerOf(0, 0), 2u);
	EXPECT_EQ(store.OwnerOf(0, 1), 3u);
}

TEST(RelationStore, RetractReachesEveryTable)
{
	relation_store store = MakeStore();
	ASSERT_TRUE(store.Insert(spawn_row(5, 1, 2, 0, 0).row).has_value());
	ASSERT_TRUE(InsertFlags(store, 5, 3).has_value());
	uint64_t before0 = store.Generation(0);
	uint64_t before1 = store.Generation(1);

	store.Retract(retract_owner{5});

	EXPECT_EQ(store.RowCount(0), 0u);
	EXPECT_EQ(store.RowCount(1), 0u);
	EXPECT_NE(store.Generation(0), before0);
	EXPECT_NE(store.Generation(1), before1);
}

TEST(RelationStore, RetractOfAbsentOwnerChangesNothing)
{
	relation_store store = MakeStore();
	ASSERT_TRUE(store.Insert(spawn_row(1, 10, 0, 0, 0).row).has_value());
	uint64_t before = store.Generation(0);

	store.Retract(retract_owner{99});
	store.Retract(retract_owner{99});

	EXPECT_EQ(store.RowCount(0), 1u);
	EXPECT_EQ(store.Generation(0), before);
}

TEST(RelationStore, ReloadIsRetractThenInsert)
{
	relation_store store = MakeStore();
	ASSERT_TRUE(store.Insert(spawn_row(1, 10, 0, 0, 0).row).has_value());
	ASSERT_TRUE(store.Insert(spawn_row(2, 20, 0, 0, 0).row).has_value());

	store.Retract(retract_owner{1});
	ASSERT_TRUE(store.Insert(spawn_row(1, 11, 0, 0, 0).row).has_value());

	ASSERT_EQ(store.RowCount(0), 2u);
	EXPECT_EQ(IntAt(store, 0, 0, 0), 20);
	EXPECT_EQ(IntAt(store, 0, 1, 0), 11);
}

TEST(RelationStore, WrongArityIsRefused)
{
	relation_store store = MakeStore();
	cell const one[] = {cell::Integer(1)};
	auto r = store.Insert(insert_row{0, 1, one, {}, {}});
	ASSERT_FALSE(r.has_value());
	EXPECT_TRUE(Mentions(r.error().message, "spawn_points")) << r.error().message;
	EXPECT_EQ(store.RowCount(0), 0u);
	EXPECT_EQ(store.Generation(0), 0u);

	std::string_view const stray[] = {"(say hi)"};
	spawn_row withCommand(1, 1, 1, 0, 0);
	withCommand.row.commands = stray;
	auto extra = store.Insert(withCommand.row);
	ASSERT_FALSE(extra.has_value());
	EXPECT_TRUE(Mentions(extra.error().message, "command")) << extra.error().message;
}

TEST(RelationStore, UnknownTableIsRefused)
{
	relation_store store = MakeStore();
	cell const one[] = {cell::Integer(1)};
	auto r = store.Insert(insert_row{9, 1, one, {}, {}});
	ASSERT_FALSE(r.has_value());
	EXPECT_TRUE(Mentions(r.error().message, "9")) << r.error().message;
}

TEST(RelationStore, FlagsOutOfRangeIsRefused)
{
	relation_store store = MakeStore();
	EXPECT_TRUE(InsertFlags(store, 1, 0).has_value());
	EXPECT_TRUE(InsertFlags(store, 1, INT64_C(0xFFFFFFFF)).has_value());

	auto negative = InsertFlags(store, 1, -1);
	ASSERT_FALSE(negative.has_value());
	EXPECT_TRUE(Mentions(negative.error().message, "'mask'")) << negative.error().message;

	EXPECT_FALSE(InsertFlags(store, 1, INT64_C(0x100000000)).has_value());
	EXPECT_EQ(store.RowCount(1), 2u);
}

TEST(RelationStore, ValueOfABadAddressIsRefused)
{
	relation_store store = MakeStore();
	ASSERT_TRUE(store.Insert(spawn_row(1, 10, 0, 0, 0).row).has_value());
	EXPECT_FALSE(store.Value(5, 0, 0).has_value());
	EXPECT_FALSE(store.Value(0, 1, 0).has_value());
	EXPECT_FALSE(store.Value(0, 0, 4).has_value());
}

TEST(RelationStore, MovedStoreKeepsItsRows)
{
	relation_store store = MakeStore();
	ASSERT_TRUE(store.Insert(spawn_row(1, 10, 0, 0, 0).row).has_value());
	relation_store moved(std::move(store));
	EXPECT_EQ(moved.RowCount(0), 1u);
	EXPECT_EQ(IntAt(moved, 0, 0, 0), 10);
}

// ---- Ordered tables ---------------------------------------------------------

namespace
{

// sorted(section: int, package: int key, mask: flags), ordered by (section, package).
relation_store MakeOrderedStore(bool unique)
{
	static add_column const columns[] = {
		{"section", column_type::Int,   false},
		{"package", column_type::Int,   true},
		{"mask",    column_type::Flags, false},
	};
	static uint32_t const order[] = {0, 1};
	order_by const ordering{order, unique};
	create_table const table{"sorted", columns, &ordering};
	auto s = BuildSchema(std::span<create_table const>(&table, 1));
	EXPECT_TRUE(s.has_value());
	return relation_store(std::move(*s));
}

tl::expected<void, ErrorContext> InsertSorted(relation_store& store, uint64_t owner, int64_t section,
	int64_t package, int64_t mask)
{
	cell const values[] = {cell::Integer(section), cell::Integer(package), cell::Integer(mask)};
	return store.Insert(insert_row{0, owner, values, {}, {}});
}

}

TEST(RelationStore, OrderedTableSortsRowsAndTiesKeepInsertionOrder)
{
	relation_store store = MakeOrderedStore(false);
	ASSERT_TRUE(InsertSorted(store, 1, 5, 2, 0).has_value());
	ASSERT_TRUE(InsertSorted(store, 1, 3, 9, 0).has_value());
	ASSERT_TRUE(InsertSorted(store, 1, 5, 1, 0).has_value());
	ASSERT_TRUE(InsertSorted(store, 1, 5, 2, 1).has_value());   // ties with the first row
	ASSERT_TRUE(InsertSorted(store, 1, -4, 0, 0).has_value());

	ASSERT_EQ(store.RowCount(0), 5u);
	std::vector<std::vector<int64_t>> got;
	for (size_t r = 0; r < store.RowCount(0); ++r)
	{
		got.push_back({IntAt(store, 0, r, 0), IntAt(store, 0, r, 1), IntAt(store, 0, r, 2)});
	}
	std::vector<std::vector<int64_t>> want = {
		{-4, 0, 0}, {3, 9, 0}, {5, 1, 0}, {5, 2, 0}, {5, 2, 1},
	};
	EXPECT_EQ(got, want);
}

TEST(RelationStore, UniqueOrderRefusesASecondRowWithTheKey)
{
	relation_store store = MakeOrderedStore(true);
	ASSERT_TRUE(InsertSorted(store, 1, 5, 2, 0).has_value());
	uint64_t before = store.Generation(0);

	auto r = InsertSorted(store, 2, 5, 2, 7);
	ASSERT_FALSE(r.has_value());
	EXPECT_TRUE(Mentions(r.error().message, "sorted")) << r.error().message;
	EXPECT_TRUE(Mentions(r.error().message, "owner 1")) << r.error().message;
	EXPECT_EQ(store.RowCount(0), 1u);
	EXPECT_EQ(store.Generation(0), before);

	EXPECT_TRUE(InsertSorted(store, 2, 5, 3, 7).has_value());   // a different key is fine
}

TEST(RelationStore, UniqueKeyCanBeReusedAfterItsOwnerReloads)
{
	relation_store store = MakeOrderedStore(true);
	ASSERT_TRUE(InsertSorted(store, 1, 5, 2, 0).has_value());
	store.Retract(retract_owner{1});
	auto r = InsertSorted(store, 1, 5, 2, 9);
	ASSERT_TRUE(r.has_value()) << r.error().message;
	EXPECT_EQ(IntAt(store, 0, 0, 2), 9);
}

TEST(RelationStore, RetractKeepsAnOrderedTableSorted)
{
	relation_store store = MakeOrderedStore(false);
	ASSERT_TRUE(InsertSorted(store, 1, 4, 0, 0).has_value());
	ASSERT_TRUE(InsertSorted(store, 2, 2, 0, 0).has_value());
	ASSERT_TRUE(InsertSorted(store, 1, 3, 0, 0).has_value());
	ASSERT_TRUE(InsertSorted(store, 2, 1, 0, 0).has_value());

	store.Retract(retract_owner{2});

	ASSERT_EQ(store.RowCount(0), 2u);
	EXPECT_EQ(IntAt(store, 0, 0, 0), 3);
	EXPECT_EQ(IntAt(store, 0, 1, 0), 4);
}

// ---- Conditions and commands ------------------------------------------------

namespace
{

// A condition's captured state: the threshold it compares with, and a count
// of releases, so a test can see each closure released exactly once.
struct probe
{
	int64_t threshold = 0;
	int*    released  = nullptr;
};

// this_in is an int64_t; the row counts when it reaches the threshold. A
// negative this_in is an error the closure reports.
bool AtLeast(void* data, void const* this_in, ErrorContext* error_out)
{
	auto const* p     = static_cast<probe const*>(data);
	int64_t     value = *static_cast<int64_t const*>(this_in);
	if (value < 0)
	{
		*error_out = ErrorContext("negative input ", value);
		return false;
	}
	return value >= p->threshold;
}

void ReleaseProbe(void* data)
{
	auto* p = static_cast<probe*>(data);
	++*p->released;
	delete p;
}

closure MakeProbe(int64_t threshold, int* released)
{
	return closure{&AtLeast, new probe{threshold, released}, &ReleaseProbe};
}

// rules(priority: int, when: condition, then: command), ordered by priority, unique.
relation_store MakeRules()
{
	static add_column const columns[] = {
		{"priority", column_type::Int,       false},
		{"when",     column_type::Condition, false},
		{"then",     column_type::Command,   false},
	};
	static uint32_t const order[] = {0};
	order_by const ordering{order, true};
	create_table const table{"rules", columns, &ordering};
	auto s = BuildSchema(std::span<create_table const>(&table, 1));
	EXPECT_TRUE(s.has_value());
	return relation_store(std::move(*s));
}

tl::expected<void, ErrorContext> InsertRule(relation_store& store, uint64_t owner, int64_t priority,
	closure when, std::string_view then)
{
	cell const             values[]     = {cell::Integer(priority)};
	closure const          conditions[] = {when};
	std::string_view const commands[]   = {then};
	return store.Insert(insert_row{0, owner, values, conditions, commands});
}

}

TEST(RelationStore, ConditionIsCalledWithThisIn)
{
	int released = 0;
	relation_store store = MakeRules();
	ASSERT_TRUE(InsertRule(store, 1, 10, MakeProbe(5, &released), "(flee)").has_value());

	int64_t low = 3, high = 8;
	auto no  = store.Holds(0, 0, 1, &low);
	auto yes = store.Holds(0, 0, 1, &high);
	ASSERT_TRUE(no.has_value()) << no.error().message;
	ASSERT_TRUE(yes.has_value()) << yes.error().message;
	EXPECT_FALSE(*no);
	EXPECT_TRUE(*yes);
}

TEST(RelationStore, ConditionErrorIsReturned)
{
	int released = 0;
	relation_store store = MakeRules();
	ASSERT_TRUE(InsertRule(store, 1, 10, MakeProbe(5, &released), "(flee)").has_value());

	int64_t negative = -2;
	auto r = store.Holds(0, 0, 1, &negative);
	ASSERT_FALSE(r.has_value());
	EXPECT_TRUE(Mentions(r.error().message, "negative input -2")) << r.error().message;
}

TEST(RelationStore, CommandTextIsCopied)
{
	int released = 0;
	relation_store store = MakeRules();
	{
		std::string text = "(message :what \"panic\")";
		ASSERT_TRUE(InsertRule(store, 1, 10, MakeProbe(0, &released), text).has_value());
		text.assign("overwritten");
	}
	auto command = store.CommandAt(0, 0, 2);
	ASSERT_TRUE(command.has_value()) << command.error().message;
	EXPECT_EQ(*command, "(message :what \"panic\")");
}

TEST(RelationStore, ConditionsAndCommandsFollowTheirRowsWhenSorted)
{
	int released = 0;
	relation_store store = MakeRules();
	ASSERT_TRUE(InsertRule(store, 1, 30, MakeProbe(30, &released), "(c)").has_value());
	ASSERT_TRUE(InsertRule(store, 1, 10, MakeProbe(10, &released), "(a)").has_value());
	ASSERT_TRUE(InsertRule(store, 2, 20, MakeProbe(20, &released), "(b)").has_value());

	store.Retract(retract_owner{2});

	ASSERT_EQ(store.RowCount(0), 2u);
	int64_t fifteen = 15;
	EXPECT_EQ(*store.CommandAt(0, 0, 2), "(a)");
	EXPECT_TRUE(*store.Holds(0, 0, 1, &fifteen));
	EXPECT_EQ(*store.CommandAt(0, 1, 2), "(c)");
	EXPECT_FALSE(*store.Holds(0, 1, 1, &fifteen));
}

TEST(RelationStore, ClosuresAreReleasedOnceOnRetractRefusalAndDestruction)
{
	int retracted = 0, refused = 0, destroyed = 0;
	{
		relation_store store = MakeRules();
		ASSERT_TRUE(InsertRule(store, 1, 10, MakeProbe(0, &retracted), "(a)").has_value());
		ASSERT_TRUE(InsertRule(store, 2, 20, MakeProbe(0, &destroyed), "(b)").has_value());

		store.Retract(retract_owner{1});
		EXPECT_EQ(retracted, 1);

		// Same priority as the surviving rule: refused by the unique order.
		EXPECT_FALSE(InsertRule(store, 3, 20, MakeProbe(0, &refused), "(c)").has_value());
		EXPECT_EQ(refused, 1);

		// Wrong arity: refused before the row is even placed.
		cell const values[] = {cell::Integer(99)};
		closure const two[] = {MakeProbe(0, &refused), MakeProbe(0, &refused)};
		std::string_view const commands[] = {"(d)"};
		EXPECT_FALSE(store.Insert(insert_row{0, 3, values, two, commands}).has_value());
		EXPECT_EQ(refused, 3);

		EXPECT_EQ(destroyed, 0);
	}
	EXPECT_EQ(retracted, 1);
	EXPECT_EQ(destroyed, 1);
}

TEST(RelationStore, WrongColumnKindIsRefused)
{
	int released = 0;
	relation_store store = MakeRules();
	ASSERT_TRUE(InsertRule(store, 1, 10, MakeProbe(0, &released), "(a)").has_value());
	int64_t zero = 0;

	auto conditionAsValue = store.Value(0, 0, 1);
	ASSERT_FALSE(conditionAsValue.has_value());
	EXPECT_TRUE(Mentions(conditionAsValue.error().message, "'when'")) << conditionAsValue.error().message;

	EXPECT_FALSE(store.Holds(0, 0, 0, &zero).has_value());
	EXPECT_FALSE(store.Holds(0, 0, 2, &zero).has_value());
	EXPECT_FALSE(store.CommandAt(0, 0, 1).has_value());
	EXPECT_FALSE(store.Value(0, 0, 2).has_value());
}
