#include <gtest/gtest.h>
#include "../bench/budgets.h"
#include "Support/allocationtestingsupport.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include "generated/tree_paths.h"

using namespace tabular::bench;

// The budget benchmarks (P17) at small sizes. Their numbers are not held to
// their budgets here: this build is -O0, and it runs beside other suites, so a
// time it measures says nothing about a shipped step. The gate is
// scripts/test-tabular-budgets.sh; this keeps the benchmarks building and
// measuring between its runs, and holds definition.md to the budgets the gate
// holds.

namespace
{

budget_sizes Small()
{
	budget_sizes sizes;
	sizes.clauses     = 4;
	sizes.modes       = 6;
	sizes.instances   = 20;
	sizes.steps       = 3;
	sizes.repeats     = 1;
	sizes.mutableRows = 500;
	sizes.displays    = 5;
	return sizes;
}

std::string Definition()
{
	std::ifstream file(REPO_ROOT "/Spehleon/Tabular/docs/definition.md");
	std::stringstream text;
	text << file.rdbuf();
	return text.str();
}

std::string Number(double value)
{
	char text[64];
	std::snprintf(text, sizeof(text), "%.10g", value);
	return text;
}

std::string Trimmed(std::string_view text)
{
	size_t const first = text.find_first_not_of(' ');
	if (first == std::string_view::npos)
	{
		return {};
	}
	return std::string(text.substr(first, text.find_last_not_of(' ') - first + 1));
}

// definition.md's measured-budgets table, benchmark name to its whole line:
// the rows after the "| Benchmark | Measured | Budget |" header and its rule,
// up to the first line that is not a table row.
std::map<std::string, std::string> BenchmarkRows(std::string const& definition)
{
	std::map<std::string, std::string> rows;
	std::istringstream lines(definition);
	std::string        line;
	bool               inTable = false;
	while (std::getline(lines, line))
	{
		if (!inTable)
		{
			inTable = line.rfind("| Benchmark | Measured | Budget |", 0) == 0;
			if (inTable)
			{
				std::getline(lines, line);   // the |---| rule
			}
			continue;
		}
		if (line.rfind("| ", 0) != 0)
		{
			break;
		}
		std::string name = Trimmed(std::string_view(line).substr(1, line.find('|', 1) - 1));
		EXPECT_TRUE(rows.emplace(std::move(name), line).second) << "definition.md has two rows for " << line;
	}
	return rows;
}

}

// The table's Cost rows are prose ("relation insert, retract, query") and do
// not name benchmarks, so which benchmark answers which cost is a reading of
// definition.md, not something to match here; DefinitionStatesEveryBudget...
// holds the benchmark table to the gate both ways.
TEST(TabularBudget, EveryBenchmarkIsNamedOnceAndMeasuresSomething)
{
	auto measured = RunBudgets(Small());
	ASSERT_TRUE(measured.has_value()) << measured.error().format();
	std::set<std::string> names;
	for (measurement const& m : *measured)
	{
		EXPECT_TRUE(names.insert(m.name).second) << "two measurements named " << m.name;
		EXPECT_FALSE(m.how.empty()) << m.name;
		EXPECT_GT(m.budget, 0) << m.name;
		if (m.unit != budget_unit::Bytes || kCountsAllocations)
		{
			EXPECT_TRUE(std::isfinite(m.measured) && m.measured > 0) << m.name << " measured " << m.measured;
		}
	}
}

// RunBudgets measures every budget in bench/budgets.cpp or refuses, so its
// names are that list; the table must name exactly those, each with the
// budget the gate holds.
TEST(TabularBudget, DefinitionStatesEveryBudgetTheGateHolds)
{
	auto measured = RunBudgets(Small());
	ASSERT_TRUE(measured.has_value()) << measured.error().format();
	std::string const definition = Definition();
	ASSERT_NE(definition.find("## Budgets"), std::string::npos);
	std::map<std::string, std::string> const rows = BenchmarkRows(definition);
	ASSERT_FALSE(rows.empty()) << "definition.md has no | Benchmark | Measured | Budget | table";
	std::set<std::string> gated;
	for (measurement const& m : *measured)
	{
		gated.insert(m.name);
		auto const row = rows.find(m.name);
		if (row == rows.end())
		{
			ADD_FAILURE() << "definition.md has no budget row for " << m.name;
			continue;
		}
		std::string const budget = "| " + Number(m.budget) + " " + std::string(UnitName(m.unit)) + " |";
		EXPECT_NE(row->second.find(budget), std::string::npos)
			<< "definition.md's row does not state " << budget << ": " << row->second;
	}
	for (auto const& [name, line] : rows)
	{
		EXPECT_TRUE(gated.count(name) == 1) << "definition.md states a budget no benchmark measures: " << line;
	}
}

TEST(TabularBudget, TheJudgeFailsAMeasurementOverItsBudgetOrOneThatMeasuredNothing)
{
	std::vector<measurement> const within = {{"a", 9, 10, budget_unit::Nanoseconds, "x"}, {"b", 10, 10, budget_unit::Bytes, "x"}};
	EXPECT_TRUE(Exceeded(within, 1).empty()) << "at the budget is within it";
	std::vector<std::string> const scaled = Exceeded(within, 0.5);
	ASSERT_EQ(scaled.size(), 2u) << "a scale below 1 tightens every budget";
	EXPECT_NE(scaled[0].find("a: 9 ns is over its budget of 5 ns"), std::string::npos) << scaled[0];

	std::vector<measurement> const broken = {{"over", 11, 10, budget_unit::Microseconds, "x"},
		{"zero", 0, 10, budget_unit::Nanoseconds, "x"}, {"nan", std::nan(""), 10, budget_unit::Nanoseconds, "x"},
		{"fine", 1, 10, budget_unit::Nanoseconds, "x"}};
	std::vector<std::string> const over = Exceeded(broken, 1);
	ASSERT_EQ(over.size(), 3u);
	EXPECT_NE(over[0].find("over: 11 us is over its budget of 10 us"), std::string::npos) << over[0];
	EXPECT_NE(over[1].find("zero: measured nothing"), std::string::npos) << over[1];
	EXPECT_NE(over[2].find("nan: measured nothing"), std::string::npos) << over[2];
}
