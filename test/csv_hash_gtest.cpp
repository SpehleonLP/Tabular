#include <gtest/gtest.h>
#include "../src/csv_cells.h"

#include <string>

using tabular::detail::hash_names;

TEST(HashNames, ZeroIsRefusedAndRepeatsAreNot)
{
	hash_names names;
	auto zero = names.Note("anything", 0, 3);
	ASSERT_FALSE(zero.has_value());
	EXPECT_TRUE(zero.error().format().find("hashes to 0") != std::string::npos) << zero.error().format();
	EXPECT_TRUE(names.Note("a", 7, 4).has_value());
	EXPECT_TRUE(names.Note("a", 7, 9).has_value()) << "the same text again is a key, not a collision";
	auto clash = names.Note("b", 7, 10);
	ASSERT_FALSE(clash.has_value());
	EXPECT_TRUE(clash.error().format().find("'a' on line 4") != std::string::npos) << clash.error().format();
}
