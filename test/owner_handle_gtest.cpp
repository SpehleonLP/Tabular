#include <gtest/gtest.h>
#include "tabular/document.h"
#include "tabular/owner_handle.h"
#include "tabular/relation.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace tabular;

namespace
{

// A mutable table, a read-only one and a grid: one owner can hold rows in all
// three kinds, and a handle's reach is what these tests are about.
// The mutable table cannot be declared in a file, so the schema is built from
// commands.
tl::expected<schema, ErrorContext> BuildTables()
{
	static add_column const id[]  = {{"id", column_type::Int}};
	static grid_shape const grid{{"r", column_type::Int}, {"c", column_type::Int}, column_type::Int};
	create_table const      tables[] = {
		{"live", id},
		{"file", id, nullptr, true},
		{"array", {}, nullptr, true, {}, pick_policy::None, UINT32_MAX, nullptr, &grid},
	};
	return BuildSchema(tables);
}

relation_store Store()
{
	auto s = BuildTables();
	EXPECT_TRUE(s.has_value()) << (s ? std::string() : s.error().format());
	return relation_store(std::move(*s));
}

void Hold(relation_store& store, uint64_t owner)
{
	cell const id[] = {cell::Integer(1)};
	EXPECT_TRUE(store.Insert(insert_row{0, owner, id, {}, {}}).has_value());
	EXPECT_TRUE(store.Load(load_rows{1, owner, 1, id}).has_value());
	cell const keys[]  = {cell::Integer(1)};
	cell const value[] = {cell::Integer(5)};
	EXPECT_TRUE(store.LoadGrid(load_grid{2, owner, keys, keys, value}).has_value());
}

void InsertLive(relation_store& store, uint64_t owner)
{
	cell const id[] = {cell::Integer(1)};
	EXPECT_TRUE(store.Insert(insert_row{0, owner, id, {}, {}}).has_value());
}

}

TEST(OwnerHandle, DroppingItRetractsTheOwnersMutableRows)
{
	relation_store store = Store();
	InsertLive(store, 2);
	{
		owner_handle handle(store, 1);
		InsertLive(store, 1);
		InsertLive(store, 1);
		ASSERT_EQ(store.RowCount(0), 3u);
	}
	ASSERT_EQ(store.RowCount(0), 1u);
	EXPECT_EQ(store.OwnerOf(0, 0), 2u) << "another owner's rows stay";
}

// Decided in the header: the handle is Retract, so a contributor's loaded rows
// and grid do not outlive it.
TEST(OwnerHandle, DroppingItAlsoRetractsTheOwnersLoadedRowsAndGrid)
{
	relation_store store = Store();
	{
		owner_handle handle(store, 1);
		Hold(store, 1);
		ASSERT_EQ(store.RowCount(1), 1u);
		ASSERT_EQ(store.RowCount(2), 1u);
	}
	EXPECT_EQ(store.RowCount(0), 0u);
	EXPECT_EQ(store.RowCount(1), 0u);
	EXPECT_EQ(store.RowCount(2), 0u);
}

TEST(OwnerHandle, ItLeavesAnotherOwnersLoadedRowsAndGrid)
{
	relation_store store = Store();
	Hold(store, 2);
	{
		owner_handle handle(store, 1);
	}
	EXPECT_EQ(store.RowCount(0), 1u);
	EXPECT_EQ(store.RowCount(1), 1u);
	EXPECT_EQ(store.RowCount(2), 1u);
}

TEST(OwnerHandle, AMovedFromHandleRetractsNothing)
{
	relation_store store = Store();
	{
		owner_handle first(store, 1);
		{
			owner_handle second(std::move(first));
			EXPECT_FALSE(first.holds());
			EXPECT_TRUE(second.holds());
			EXPECT_EQ(second.owner(), 1u);
			InsertLive(store, 1);
		}
		ASSERT_EQ(store.RowCount(0), 0u) << "the new holder's drop retracts";
		// Rows the owner has afterwards belong to a later holder; the moved-from
		// handle ending its scope must not take them.
		InsertLive(store, 1);
		first.Release();
		EXPECT_EQ(store.RowCount(0), 1u) << "releasing a moved-from handle is nothing";
	}
	EXPECT_EQ(store.RowCount(0), 1u) << "so is destroying it";
}

TEST(OwnerHandle, MoveAssignmentRetractsTheOwnerItOverwrites)
{
	relation_store store = Store();
	InsertLive(store, 1);
	InsertLive(store, 2);
	owner_handle one(store, 1);
	owner_handle two(store, 2);
	one = std::move(two);
	EXPECT_EQ(store.RowCount(0), 1u);
	EXPECT_EQ(store.OwnerOf(0, 0), 2u) << "owner 1's rows went when its handle was overwritten";
	EXPECT_EQ(one.owner(), 2u);
	EXPECT_FALSE(two.holds());
}

TEST(OwnerHandle, SelfMoveAssignmentKeepsTheHandle)
{
	relation_store store = Store();
	owner_handle   handle(store, 1);
	InsertLive(store, 1);
	owner_handle& alias = handle;
	handle = std::move(alias);
	EXPECT_TRUE(handle.holds());
	EXPECT_EQ(store.RowCount(0), 1u);
}

TEST(OwnerHandle, AnEarlyReleaseRetractsOnceAndTheDestructorThenDoesNothing)
{
	relation_store store = Store();
	{
		owner_handle handle(store, 1);
		InsertLive(store, 1);
		handle.Release();
		EXPECT_EQ(store.RowCount(0), 0u);
		EXPECT_FALSE(handle.holds());
		InsertLive(store, 1);
		handle.Release();
		EXPECT_EQ(store.RowCount(0), 1u) << "a second release is nothing";
	}
	EXPECT_EQ(store.RowCount(0), 1u) << "so is the destructor after a release";
}

TEST(OwnerHandle, DroppingAHandleWhoseOwnerHoldsNothingChangesNoGeneration)
{
	relation_store store = Store();
	Hold(store, 2);
	uint64_t const before[] = {store.Generation(0), store.Generation(1), store.Generation(2)};
	{
		owner_handle handle(store, 1);
	}
	EXPECT_EQ(store.Generation(0), before[0]);
	EXPECT_EQ(store.Generation(1), before[1]);
	EXPECT_EQ(store.Generation(2), before[2]);
}
