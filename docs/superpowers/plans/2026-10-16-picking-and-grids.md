# Picking and Grids Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Read-only tables declare inputs, bands and a flag-ranking column and answer `PickBest` / `PickAll` / `PickOr`; pivoted CSV writes them as grids of flat rows; a grid table is a dense 2D array looked up by two keys.

**Architecture:** Picks are free functions over a `TableVersion` (`pick.h`), reading the store's private row layout through a private header that this plan splits out of `relation.cpp`. Pivoting lives only in the CSV front end and produces ordinary sorted rows plus a per-row pivot column. Grids are a table kind with their own immutable refcounted `GridVersion`, held by the store beside the table versions.

**Tech Stack:** C++20, tl::expected, gtest compiled into `Build/Engine`, Spehleon/lib (`Errors`, `Support`, `Utility/load_csv.h`, `Yaml`).

**Spec:** `Spehleon/Tabular/docs/superpowers/specs/2026-10-16-picking-and-grids-design.md`. The contract is `Spehleon/Tabular/docs/definition.md`.

## Global Constraints

- Worktree: `env -C /mnt/Passport/Kreatures-Engine PWD=/mnt/Passport/Kreatures-Engine scripts/wt-create.sh tabular-picking`; `WT=/mnt/Passport/Worktrees/tabular-picking`. Never edit, build or run git in `/mnt/Passport/Kreatures-Engine`.
- Build: `env -C $WT PWD=$WT scripts/wt-build.sh` (foreground; queueing is normal). Never call cmake/ninja directly.
- Tests, from `$WT`: `cd $WT && GTEST_FILTER="$FILTER" Build/Engine --gtest`, with `FILTER='Relation*:RowsCsv.*:SchemaYaml.*:Pick*:PivotCsv.*:Grid*'`. Never an empty `GTEST_FILTER`.
- Commit leaves first: `Spehleon/Tabular`, then `Spehleon` (gitlink bump plus any `Spehleon/cmake` change), then the superproject (gitlink bump plus `cmake/sources_tests.cmake`). End each message with the session's `Co-Authored-By` / `Claude-Session` lines. Never push, merge or rebase.
- Brace every control-flow body. Errors are returned (`tl::expected<…, ErrorContext>`), never logged; no `LOG_F(ERROR/FATAL)`. Asserts are live in release: the only new asserts are the grid index overloads' bounds.
- PascalCase types are heap-allocated (`GridVersion`, `HeldCondition`), adopted with `UncountedWrap(new T(...))`; snake_case types are never `new`ed. Big parameters by `const&` or `&&`.
- New aggregate fields go at the **end** of their struct: `create_table`, `add_column`, `column_def`, `table_def`, `load_rows`, `csv_rows` are built positionally by existing code and tests.
- Comments say why, not what.
- After restoring any deliberate mutation, `touch` the file so the build sees it.
- `git grep` skips submodules: grep inside `Spehleon/Tabular` directly. Never grep or find rooted at `/`.

### Spec amendments made while planning (owner to see in the final report)

1. **Command structs name inputs, outputs and pivots by number**, like `order_by` names its columns; YAML resolves names to numbers. So `add_column::input` and `create_table::output` are `uint32_t` (`UINT32_MAX` = none), and the pivot is `pivot_by const*` (`{headers, cells}` column numbers). The spec's `std::string_view input`/`output` spelling is replaced.
2. **The grid shape is one struct**, `grid_shape const* grid` on `create_table` (`{rows, columns, cells}`), in place of the spec's three loose fields.
3. **A pivot's cells column may be a string column** (clip names in a lookup table, as animation's lookup CSV holds), besides a value or band column.
4. **Bands compare in float**, as `eval_phase_bits` does: the `double` input is narrowed to `float` and tested `x >= start && x < end` against the `vec2`'s floats, so a table moved from animation picks the same rows at the edges.
5. **The default-row load check covers key runs that contain a row of the load.** A run made only of survivors is not re-checked: removing rows (`Retract`, or a reload of the owner that held a run's default) is never refused, and such a run's `PickBest` returns an empty optional when nothing ranked holds.
6. **"At most one script condition" applies to every table**, as the definition states it, not only pickable ones (no existing table has two).

## Review Focus

1. Retracting the owner that holds a key run's default row: `Retract` is never refused and later picks on that key return an empty optional, not an error or a crash. → Task 2, `PickBest.RetractingTheDefaultsOwnerIsNeverRefused`.
2. A band at its edges and on a NaN input: `t == start` holds, `t == end` does not, NaN holds nowhere. → Task 3, `PickOr.EdgesAreHalfOpenAndNanHoldsNothing`.
3. A flags input outside 32 bits (negative, or 2^32): the pick is refused, not silently masked. → Task 2, `PickBest.QueriesOfTheWrongShapeAreRefused`.
4. Pivot headers written with spaces around a combo's parts (`Injured | Crouch`) and in another case (`none`): accepted, same bits. → Task 4, `PivotCsv.HeadersAreTrimmedAndNoneIgnoresCase`.
5. A grid with negative int keys, and a grid reloaded while an old version is held: lookups by negative key work, and the held version keeps answering with its own cells. → Task 5, `Grid.NegativeIntKeysAreFound` and `Grid.AHeldVersionKeepsItsCells`.

---

### Task 1: Schema: inputs, bands, ranking, pick policy (and YAML)

**Files:**
- Modify: `Spehleon/Tabular/include/tabular/relation.h`
- Modify: `Spehleon/Tabular/src/relation.cpp` (TypeName, IsValueColumn, BuildColumns, BuildTable; new BuildInputs, BuildPick)
- Modify: `Spehleon/Tabular/src/rows_csv.cpp` (ReadCell: `Band`)
- Modify: `Spehleon/Tabular/src/schema_yaml.cpp`
- Create: `Spehleon/Tabular/test/pick_schema_gtest.cpp`
- Modify: `Spehleon/Tabular/test/schema_yaml_gtest.cpp`
- Modify: superproject `cmake/sources_tests.cmake` (register `pick_schema_gtest.cpp` after `rows_csv_gtest.cpp`)

**Interfaces:**
- Produces (in `relation.h`):

```cpp
enum class column_type : uint8_t { Hash, Flags, Int, Double, Condition, Command, Vec2, String, Band };

struct add_column
{
	std::string_view name;
	column_type      type  = column_type::Int;
	bool             key   = false;
	uint32_t         input = UINT32_MAX;   // Band or ranking Flags: index into create_table::inputs
};

struct add_input
{
	std::string_view name;
	column_type      type = column_type::Double;   // Double or Flags
};

enum class pick_policy : uint8_t { None, Best, All, Or };

struct create_table
{
	std::string_view              name;
	std::span<add_column const>   columns;
	order_by const*               ordering  = nullptr;
	bool                          read_only = false;
	std::span<add_input const>    inputs;
	pick_policy                   pick   = pick_policy::None;
	uint32_t                      output = UINT32_MAX;   // Or: the Flags value column OR'd
};

struct column_def { std::string name; column_type type = column_type::Int; bool key = false; uint32_t input = UINT32_MAX; };
struct input_def  { std::string name; column_type type = column_type::Double; };

struct table_def
{
	std::string                  name;
	immutable_array<column_def>  columns;
	immutable_array<uint32_t>    order;
	bool                         unique    = false;
	bool                         read_only = false;
	immutable_array<input_def>   inputs;
	pick_policy                  pick      = pick_policy::None;
	uint32_t                     output    = UINT32_MAX;
	uint32_t                     ranking   = UINT32_MAX;   // the Flags column with an input
	uint32_t                     key_count = 0;            // columns with key: true
};
```

- Produces (YAML): table keys `inputs: [{name, type}]`, `pick: none|best|all|or`, `output: <column name>`; column key `input: <input name>`; type name `band`.

- [ ] **Step 1: Write the failing schema tests**

Create `Spehleon/Tabular/test/pick_schema_gtest.cpp`:

```cpp
#include <gtest/gtest.h>
#include "tabular/relation.h"
#include "tabular/rows_csv.h"

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

add_input const kInputs[] = {{"mode", column_type::Flags}, {"t", column_type::Double}};

// label (key), req (ranks by mode), window (band over t), clip.
std::vector<add_column> Columns()
{
	return {
		{"label",  column_type::Int,   true},
		{"req",    column_type::Flags, false, 0},
		{"window", column_type::Band,  false, 1},
		{"clip",   column_type::Int},
	};
}

uint32_t const kByLabel[] = {0};
order_by const kOrder{kByLabel, false};

tl::expected<schema, ErrorContext> Build(std::vector<add_column> const& columns,
	std::span<add_input const> inputs = kInputs, pick_policy pick = pick_policy::Best,
	uint32_t output = UINT32_MAX, order_by const* order = &kOrder, bool read_only = true)
{
	create_table const t{"lookup", columns, order, read_only, inputs, pick, output};
	return BuildSchema(std::span<create_table const>(&t, 1));
}

std::string Refusal(tl::expected<schema, ErrorContext> const& s)
{
	return s ? std::string() : s.error().message;
}

}

TEST(PickSchema, APickedTableResolvesItsShape)
{
	auto s = Build(Columns());
	ASSERT_TRUE(s.has_value()) << s.error().message;
	table_def const& def = s->tables()[0];
	EXPECT_EQ(def.pick, pick_policy::Best);
	EXPECT_EQ(def.ranking, 1u);
	EXPECT_EQ(def.key_count, 1u);
	EXPECT_EQ(def.columns[2].input, 1u);
	ASSERT_EQ(def.inputs.size(), 2u);
	EXPECT_EQ(def.inputs[0].name, "mode");
	EXPECT_EQ(def.inputs[1].type, column_type::Double);
}

TEST(PickSchema, InputsAreNamedOnceAndAreDoublesOrFlags)
{
	add_input const twice[] = {{"mode", column_type::Flags}, {"mode", column_type::Double}};
	EXPECT_TRUE(Mentions(Refusal(Build(Columns(), twice)), "input 'mode' twice")) << Refusal(Build(Columns(), twice));
	add_input const wrong[] = {{"mode", column_type::Flags}, {"t", column_type::Int}};
	EXPECT_TRUE(Mentions(Refusal(Build(Columns(), wrong)), "a double or a flag set")) << Refusal(Build(Columns(), wrong));
}

TEST(PickSchema, AColumnReadsAnInputOfItsKind)
{
	auto columns = Columns();
	columns[2].input = UINT32_MAX;
	EXPECT_TRUE(Mentions(Refusal(Build(columns)), "names the double input")) << Refusal(Build(columns));

	columns = Columns();
	columns[2].input = 0;   // a band over the flags input
	EXPECT_TRUE(Mentions(Refusal(Build(columns)), "reads input 'mode'")) << Refusal(Build(columns));

	columns = Columns();
	columns[3].input = 1;   // an int column reading an input
	EXPECT_TRUE(Mentions(Refusal(Build(columns)), "only a band or a flags column")) << Refusal(Build(columns));

	columns = Columns();
	columns[1].input = 5;
	EXPECT_TRUE(Mentions(Refusal(Build(columns)), "reads input 5")) << Refusal(Build(columns));
}

TEST(PickSchema, OneRankingColumnAndOneScriptCondition)
{
	auto columns = Columns();
	columns.push_back({"also", column_type::Flags, false, 0});
	EXPECT_TRUE(Mentions(Refusal(Build(columns)), "ranks by two")) << Refusal(Build(columns));

	columns = Columns();
	columns.push_back({"when", column_type::Condition});
	columns.push_back({"unless", column_type::Condition});
	EXPECT_TRUE(Mentions(Refusal(Build(columns)), "one script condition")) << Refusal(Build(columns));
}

TEST(PickSchema, APickedTableIsReadOnlyAndOrderedByItsKeys)
{
	EXPECT_TRUE(Mentions(Refusal(Build(Columns(), kInputs, pick_policy::Best, UINT32_MAX, &kOrder, false)),
		"read-only"));
	uint32_t const byClip[] = {3};
	order_by const clipOrder{byClip, false};
	EXPECT_TRUE(Mentions(Refusal(Build(Columns(), kInputs, pick_policy::Best, UINT32_MAX, &clipOrder)),
		"starts with its keys")) << Refusal(Build(Columns(), kInputs, pick_policy::Best, UINT32_MAX, &clipOrder));
	EXPECT_TRUE(Mentions(Refusal(Build(Columns(), kInputs, pick_policy::Best, UINT32_MAX, nullptr)),
		"starts with its keys"));
}

TEST(PickSchema, AnOrTableNamesAFlagsOutput)
{
	auto columns = Columns();
	columns[1].input = UINT32_MAX;   // req becomes a plain flags value: the output
	EXPECT_TRUE(Build(columns, kInputs, pick_policy::Or, 1).has_value());
	EXPECT_TRUE(Mentions(Refusal(Build(columns, kInputs, pick_policy::Or)), "names its output"));
	EXPECT_TRUE(Mentions(Refusal(Build(columns, kInputs, pick_policy::Or, 3)), "output 'clip' is not a flag set"));
	EXPECT_TRUE(Mentions(Refusal(Build(Columns(), kInputs, pick_policy::Best, 1)), "only an or table"));
}

TEST(PickSchema, ABandIsAConditionNotAValue)
{
	auto columns = Columns();
	columns[2].key = true;
	EXPECT_TRUE(Mentions(Refusal(Build(columns)), "cannot be a key"));

	uint32_t const byBand[] = {0, 2};
	order_by const bandOrder{byBand, false};
	EXPECT_TRUE(Mentions(Refusal(Build(Columns(), kInputs, pick_policy::Best, UINT32_MAX, &bandOrder)),
		"cannot be in an order"));

	auto s = Build(Columns(), kInputs, pick_policy::None);
	ASSERT_TRUE(s.has_value()) << s.error().message;
	relation_store store(std::move(*s));
	cell const row[] = {cell::Integer(1), cell::Integer(0), cell::Vector(0.0f, 1.0f), cell::Integer(7)};
	ASSERT_TRUE(store.Load(load_rows{0, 1, 1, row}).has_value());
	EXPECT_FALSE(store.Value(0, 0, 2).has_value()) << "a band is read by a pick, not as a value";
	EXPECT_EQ(store.Value(0, 0, 1)->i, 0) << "the ranking column is still a value";
}

TEST(PickSchema, ABandCellIsWrittenAsARange)
{
	auto s = Build(Columns(), kInputs, pick_policy::None);
	ASSERT_TRUE(s.has_value()) << s.error().message;
	relation_store store(std::move(*s));
	EXPECT_TRUE(LoadCsv(store, csv_rows{0, 1, "label,req,window,clip\n1,0,0.25..0.5,7\n", "l.csv"}).has_value());
	auto bad = LoadCsv(store, csv_rows{0, 1, "label,req,window,clip\n1,0,0.25,7\n", "l.csv"});
	ASSERT_FALSE(bad.has_value());
	EXPECT_TRUE(Mentions(bad.error().format(), "is a band, written a..b")) << bad.error().format();
}
```

Append to `Spehleon/Tabular/test/schema_yaml_gtest.cpp`:

```cpp
TEST(SchemaYaml, APickedTableIsDeclaredByName)
{
	auto s = SchemaFromYaml(R"(
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
)", "phase.yaml");
	ASSERT_TRUE(s.has_value()) << s.error().format();
	table_def const& def = s->tables()[0];
	EXPECT_EQ(def.pick, pick_policy::Or);
	EXPECT_EQ(def.output, 1u);
	EXPECT_EQ(def.columns[2].type, column_type::Band);
	EXPECT_EQ(def.columns[2].input, 0u);
}

TEST(SchemaYaml, AnUndeclaredInputOrOutputIsRefused)
{
	auto input = SchemaFromYaml(R"(
tables:
  - name: t
    read_only: true
    columns:
      - {name: window, type: band, input: speed}
)", "t.yaml");
	ASSERT_FALSE(input.has_value());
	EXPECT_TRUE(Mentions(input.error().format(), "input 'speed'")) << input.error().format();

	auto output = SchemaFromYaml(R"(
tables:
  - name: t
    read_only: true
    pick: or
    output: bits
    columns:
      - {name: clip, type: int}
)", "t.yaml");
	ASSERT_FALSE(output.has_value());
	EXPECT_TRUE(Mentions(output.error().format(), "output 'bits'")) << output.error().format();
}
```

(If `schema_yaml_gtest.cpp` has no `Mentions` helper, add the same `static bool Mentions` as above at its top.)

Register the new file in superproject `cmake/sources_tests.cmake`, after the `rows_csv_gtest.cpp` line:

```cmake
	${SPEHLEON_ROOT}/Tabular/test/pick_schema_gtest.cpp
```

- [ ] **Step 2: Build and watch it fail**

Run: `env -C $WT PWD=$WT scripts/wt-build.sh`
Expected: compile errors: `add_input`, `pick_policy`, `column_type::Band` undeclared.

- [ ] **Step 3: Implement the schema**

In `relation.h`, apply the Interfaces block above. Extend the comment above `column_type`:

```cpp
// A Band is a condition stored as a vec2 cell: the row counts when its input
// x satisfies start <= x < end, half-open as animation's phase bands are.
```

and above `create_table::inputs`:

```cpp
	// What a pick is given, in this order. A band names a Double input and a
	// ranking flags column a Flags one; Tabular resolves no names at pick time.
```

In `relation.cpp`:

`TypeName` gains `case column_type::Band: return "band";`.

`IsValueColumn` excludes `Band` (it keeps its value slot, but is not read by `Value`):

```cpp
bool IsValueColumn(column_type type)
{
	return type != column_type::Condition && type != column_type::Command && type != column_type::String
		&& type != column_type::Band;
}
```

Add before `BuildColumns`:

```cpp
tl::expected<immutable_array<input_def>, ErrorContext> BuildInputs(create_table const& c)
{
	shared_array<input_def> inputs(c.inputs.size());
	for (uint32_t i = 0; i < c.inputs.size(); ++i)
	{
		add_input const& in = c.inputs[i];
		if (in.name.empty())
		{
			return tl::make_unexpected(ErrorContext("input ", i, " of table '", c.name, "' needs a name"));
		}
		for (uint32_t earlier = 0; earlier < i; ++earlier)
		{
			if (inputs[earlier].name == in.name)
			{
				return tl::make_unexpected(ErrorContext("table '", c.name, "' has input '", in.name, "' twice"));
			}
		}
		if (in.type != column_type::Double && in.type != column_type::Flags)
		{
			return tl::make_unexpected(ErrorContext("input '", in.name, "' of table '", c.name,
				"' is a ", TypeName(in.type), "; an input is a double or a flag set"));
		}
		inputs[i] = input_def{std::string(in.name), in.type};
	}
	return immutable_array<input_def>(std::move(inputs));
}

// A band tests a double; a ranking flags column compares a flag set. Nothing
// else reads an input.
tl::expected<void, ErrorContext> CheckInput(create_table const& c, add_column const& in,
	immutable_array<input_def> const& inputs)
{
	if (in.input == UINT32_MAX)
	{
		if (in.type == column_type::Band)
		{
			return tl::make_unexpected(ErrorContext("band '", in.name, "' of table '", c.name,
				"' names the double input it tests"));
		}
		return {};
	}
	if (in.type != column_type::Band && in.type != column_type::Flags)
	{
		return tl::make_unexpected(ErrorContext("column '", in.name, "' of table '", c.name, "' is a ",
			TypeName(in.type), "; only a band or a flags column reads an input"));
	}
	if (in.input >= inputs.size())
	{
		return tl::make_unexpected(ErrorContext("column '", in.name, "' of table '", c.name, "' reads input ",
			in.input, ", but the table has ", inputs.size()));
	}
	column_type const wanted = in.type == column_type::Band ? column_type::Double : column_type::Flags;
	if (inputs[in.input].type != wanted)
	{
		return tl::make_unexpected(ErrorContext("column '", in.name, "' of table '", c.name, "' reads input '",
			inputs[in.input].name, "', a ", TypeName(inputs[in.input].type), "; a ", TypeName(in.type),
			" reads a ", TypeName(wanted)));
	}
	return {};
}
```

`BuildColumns` takes the inputs and checks each column:

```cpp
tl::expected<immutable_array<column_def>, ErrorContext> BuildColumns(create_table const& c,
	immutable_array<input_def> const& inputs)
```

Inside its loop, after the key check:

```cpp
		auto reads = CheckInput(c, in, inputs);
		if (!reads)
		{
			return tl::make_unexpected(std::move(reads.error()));
		}
		columns[i] = column_def{std::string(in.name), in.type, in.key, in.input};
```

(replacing the old `columns[i] = …` line).

Add after `BuildOrder`:

```cpp
// The pick shape: one ranking column, one script condition, the key prefix,
// and the output of an or table. Every table gets ranking and key_count, so a
// later front end reads them without re-deriving.
tl::expected<void, ErrorContext> BuildPick(create_table const& c, table_def& out)
{
	uint32_t conditions = 0;
	for (uint32_t i = 0; i < out.columns.size(); ++i)
	{
		column_def const& col = out.columns[i];
		if (col.key)
		{
			++out.key_count;
		}
		if (col.type == column_type::Condition && ++conditions > 1)
		{
			return tl::make_unexpected(ErrorContext("table '", c.name,
				"' has two condition columns; a row carries at most one script condition"));
		}
		if (col.type == column_type::Flags && col.input != UINT32_MAX)
		{
			if (out.ranking != UINT32_MAX)
			{
				return tl::make_unexpected(ErrorContext("table '", c.name, "' ranks by two flags columns, '",
					out.columns[out.ranking].name, "' and '", col.name, "'"));
			}
			out.ranking = i;
		}
	}
	out.pick = c.pick;
	if (c.pick != pick_policy::Or && c.output != UINT32_MAX)
	{
		return tl::make_unexpected(ErrorContext("table '", c.name, "' names an output; only an or table has one"));
	}
	if (c.pick == pick_policy::None)
	{
		return {};
	}
	if (!out.read_only)
	{
		return tl::make_unexpected(ErrorContext("table '", c.name, "' is picked, and a picked table is read-only"));
	}
	// A pick's keys are the order's leading columns, found by binary search.
	uint32_t k = 0;
	for (uint32_t i = 0; i < out.columns.size(); ++i)
	{
		if (!out.columns[i].key)
		{
			continue;
		}
		if (k >= out.order.size() || out.order[k] != i)
		{
			return tl::make_unexpected(ErrorContext("table '", c.name,
				"' is picked, so its order starts with its keys, in column order"));
		}
		++k;
	}
	if (c.pick == pick_policy::Or)
	{
		if (c.output == UINT32_MAX)
		{
			return tl::make_unexpected(ErrorContext("or table '", c.name, "' names its output, a flags column"));
		}
		if (c.output >= out.columns.size() || out.columns[c.output].type != column_type::Flags)
		{
			return tl::make_unexpected(ErrorContext("output '",
				c.output < out.columns.size() ? out.columns[c.output].name : std::string("?"),
				"' of table '", c.name, "' is not a flag set"));
		}
		out.output = c.output;
	}
	return {};
}
```

In `BuildTable`, build inputs first, pass them to `BuildColumns`, store them, and call `BuildPick` after the order:

```cpp
	auto inputs = BuildInputs(c);
	if (!inputs)
	{
		return tl::make_unexpected(std::move(inputs.error()));
	}
	auto columns = BuildColumns(c, *inputs);
	if (!columns)
	{
		return tl::make_unexpected(std::move(columns.error()));
	}

	table_def out{std::string(c.name), std::move(*columns), {}, false, c.read_only};
	out.inputs = std::move(*inputs);
	if (c.ordering)
	{
		... unchanged ...
	}
	auto picked = BuildPick(c, out);
	if (!picked)
	{
		return tl::make_unexpected(std::move(picked.error()));
	}
	return out;
```

In `rows_csv.cpp` `ReadCell`, the `Vec2` case serves `Band` too:

```cpp
	case column_type::Vec2:
	case column_type::Band:
	{
		... unchanged parse ...
		{
			return tl::make_unexpected(ErrorContext("column '", column.name, "' is a ",
				column.type == column_type::Band ? "band" : "vec2", ", written a..b; got '", text, "'"));
		}
```

In `schema_yaml.cpp`:

```cpp
struct column_yaml
{
	std::string name;
	int32_t     type = -1;
	bool        key  = false;
	std::string input;
};

struct input_yaml
{
	std::string name;
	int32_t     type = -1;
};

struct table_yaml
{
	std::string              name;
	std::vector<column_yaml> columns;
	order_yaml               order_by;
	bool                     read_only = false;
	std::vector<input_yaml>  inputs;
	int32_t                  pick = 0;
	std::string              output;
};

char const* const kTypeNames[] = {"hash", "flags", "int", "double", "condition", "command", "vec2", "string", "band"};
YamlEnum const    kTypes{kTypeNames, 9};

// In pick_policy's order.
char const* const kPickNames[] = {"none", "best", "all", "or"};
YamlEnum const    kPicks{kPickNames, 4};
```

`kColumnFields` gains `{"input", FieldType::String, offsetof(column_yaml, input)},`. Add:

```cpp
YamlField const kInputFields[] = {
	{"name", FieldType::String, offsetof(input_yaml, name), true},
	{"type", FieldType::Enum,   offsetof(input_yaml, type), true, 0, nullptr, nullptr, &kTypes},
};
YamlSet const kInputSet{kInputFields, nullptr, "input"};
```

`kTableFields` gains:

```cpp
	{"inputs",    FieldType::Sequence, offsetof(table_yaml, inputs), false,
	 0, &kInputSet, nullptr, nullptr, MakeContainer<std::vector<input_yaml>>()},
	{"pick",      FieldType::Enum,     offsetof(table_yaml, pick), false, 0, nullptr, nullptr, &kPicks},
	{"output",    FieldType::String,   offsetof(table_yaml, output)},
```

In `SchemaFromYaml`, add `std::vector<std::vector<add_input>> inputs(document.tables.size());` beside `columns`, and per table, before building columns:

```cpp
		for (input_yaml const& i : in.inputs)
		{
			inputs[t].push_back(add_input{i.name, static_cast<column_type>(i.type)});
		}
		auto inputOf = [&](std::string const& name) -> uint32_t
		{
			for (uint32_t i = 0; i < in.inputs.size(); ++i)
			{
				if (in.inputs[i].name == name)
				{
					return i;
				}
			}
			return UINT32_MAX;
		};
		for (column_yaml const& c : in.columns)
		{
			uint32_t input = UINT32_MAX;
			if (!c.input.empty())
			{
				input = inputOf(c.input);
				if (input == UINT32_MAX)
				{
					return tl::make_unexpected(ErrorContext("column '", c.name, "' of table '", in.name,
						"' reads input '", c.input, "', which the table does not declare").with(std::string(label)));
				}
			}
			columns[t].push_back(add_column{c.name, static_cast<column_type>(c.type), c.key, input});
		}
```

(replacing the old column loop). Add a column-name lookup used by `output` (and by Tasks 4 and 6):

```cpp
		auto columnOf = [&](std::string const& name) -> uint32_t
		{
			for (uint32_t c = 0; c < in.columns.size(); ++c)
			{
				if (in.columns[c].name == name)
				{
					return c;
				}
			}
			return UINT32_MAX;
		};
		uint32_t output = UINT32_MAX;
		if (!in.output.empty())
		{
			output = columnOf(in.output);
			if (output == UINT32_MAX)
			{
				return tl::make_unexpected(ErrorContext("table '", in.name, "' names output '", in.output,
					"', which the table does not have").with(std::string(label)));
			}
		}
```

and the command becomes:

```cpp
		commands.push_back(create_table{in.name, columns[t], ordered ? &orders[t] : nullptr, in.read_only,
			inputs[t], static_cast<pick_policy>(in.pick), output});
```

(The existing order-column lookup loop may use `columnOf` too.)

- [ ] **Step 4: Build and run**

Run: `env -C $WT PWD=$WT scripts/wt-build.sh && cd $WT && GTEST_FILTER="$FILTER" Build/Engine --gtest > $WT/../tabular-picking-t1.log 2>&1; tail -5 $WT/../tabular-picking-t1.log`
Expected: all pass, including the 8 `PickSchema` and 2 new `SchemaYaml` tests, and every existing test (none declares two condition columns).

- [ ] **Step 5: Mutation check**

In `BuildPick`, change `++conditions > 1` to `++conditions > 2`. Build, run `GTEST_FILTER='PickSchema.*'`. Expected: `PickSchema.OneRankingColumnAndOneScriptCondition` FAILS. Restore, `touch src/relation.cpp`, rebuild, rerun: passes.

- [ ] **Step 6: Commit**

```bash
cd $WT/Spehleon/Tabular && git add include/tabular/relation.h src/relation.cpp src/rows_csv.cpp src/schema_yaml.cpp test/pick_schema_gtest.cpp test/schema_yaml_gtest.cpp && git commit -m "Schema: inputs, bands, a ranking column and a pick policy; YAML names them"
cd $WT/Spehleon && git add Tabular && git commit -m "Bump Tabular: pick schema"
cd $WT && git add Spehleon cmake/sources_tests.cmake && git commit -m "Bump Spehleon: pick schema; register its tests"
```

---

### Task 2: PickBest, the pivot column and the default-row check

**Files:**
- Create: `Spehleon/Tabular/src/relation_internal.h` (private: the row layout, split out of `relation.cpp`)
- Modify: `Spehleon/Tabular/src/relation.cpp`
- Modify: `Spehleon/Tabular/include/tabular/relation.h` (`load_rows::pivot`; `friend struct version_access`)
- Create: `Spehleon/Tabular/include/tabular/pick.h`, `Spehleon/Tabular/src/pick.cpp`
- Modify: `Spehleon/cmake/sources_tabular.cmake` (add `pick.cpp`)
- Create: `Spehleon/Tabular/test/pick_gtest.cpp`; register in superproject `cmake/sources_tests.cmake`

**Interfaces:**
- Consumes: Task 1's `table_def::{pick, ranking, key_count, inputs}`, `column_def::input`, `column_type::Band`.
- Produces:

```cpp
// relation.h
struct load_rows { ...existing fields...; std::span<uint16_t const> pivot; };   // empty, or one per row

// pick.h
namespace tabular
{
struct pick_query
{
	std::span<cell const> keys;              // the key columns' values, in column order
	std::span<cell const> inputs;            // the table's declared inputs, in order
	void const*           this_in = nullptr; // handed to the script condition
};

tl::expected<std::optional<size_t>, ErrorContext> PickBest(TableVersion const& version, pick_query const& query);
tl::expected<std::optional<size_t>, ErrorContext> PickBest(relation_store const& store, uint32_t table, pick_query const& query);
}

// relation_internal.h (private to src/)
namespace tabular
{
struct table_rows { ...; std::vector<uint16_t> pivots; /* read-only tables: one per row */ };
struct version_access { static table_def const& Def(TableVersion const&); static table_rows const& Rows(TableVersion const&); };
}
```

- [ ] **Step 1: Write the failing pick tests**

Create `Spehleon/Tabular/test/pick_gtest.cpp`:

```cpp
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
	return store.Value(0, **picked, 2)->i;
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
	return picked && *picked ? store.Value(0, **picked, 3)->i : -1;
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
	EXPECT_TRUE(Mentions(PickBest(store, 0, pick_query{twoKeys, oneInput}).error().format(), "2 keys"));
	EXPECT_TRUE(Mentions(PickBest(store, 0, pick_query{oneKey, {}}).error().format(), "0 inputs"));
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
	EXPECT_EQ((*held)->Value(**old, 2)->i, 10);
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
```

Register in superproject `cmake/sources_tests.cmake` after `pick_schema_gtest.cpp`:

```cmake
	${SPEHLEON_ROOT}/Tabular/test/pick_gtest.cpp
```

- [ ] **Step 2: Build and watch it fail**

Run: `env -C $WT PWD=$WT scripts/wt-build.sh`
Expected: `tabular/pick.h` not found.

- [ ] **Step 3: Split the private row layout into `src/relation_internal.h`**

Move, unchanged except as noted, from `relation.cpp` into a new `src/relation_internal.h`: `ReleaseClosure`, `HeldCondition`, `held_condition`, `Hold` (into `namespace tabular::detail`, as `inline` functions: they were in an anonymous namespace, and a type from an anonymous namespace inside `table_rows`, which every TU including the header sees, would be a different type per TU); `ShiftRows` stays in `relation.cpp`. Also move `struct table_rows`, `struct TableVersion::Data`, and `struct relation_store::Storage`. Add the pivot array and the accessor:

```cpp
#ifndef TABULAR_RELATION_INTERNAL_H
#define TABULAR_RELATION_INTERNAL_H

// The store's row layout, shared by relation.cpp, pick.cpp and grid.cpp. Not a
// public header: nothing outside src/ includes it.

#include "tabular/relation.h"

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tabular
{

namespace detail
{

inline void ReleaseClosure(closure const& c) { ... as before ... }

class HeldCondition { ... as before ... };

using held_condition = counted_ptr<HeldCondition const>;

inline held_condition Hold(closure const& c) { ... as before ... }

}

struct table_rows
{
	... as before, with detail::held_condition ...
	// Per row, the header position a pivoted load took it from (0 otherwise),
	// so flag ties go to column order. Kept by read-only tables only; picks
	// read nothing else.
	std::vector<uint16_t>    pivots;
	...
};

struct TableVersion::Data { table_def def; table_rows rows; };

struct relation_store::Storage { ... as before ... };

// Picks and grids read a version's rows; TableVersion keeps them private.
struct version_access
{
	static table_def const&  Def(TableVersion const& v)  { return v._data->def; }
	static table_rows const& Rows(TableVersion const& v) { return v._data->rows; }
};

}

#endif // TABULAR_RELATION_INTERNAL_H
```

In `relation.cpp`, `#include "relation_internal.h"`, delete the moved definitions, and add `using detail::Hold; using detail::held_condition; using detail::ReleaseClosure;` inside its anonymous namespace (`ReleaseAll` stays there and calls `ReleaseClosure`).

In `relation.h`, inside `class TableVersion`, beside `friend class relation_store;`:

```cpp
	friend struct version_access;
```

and add `struct version_access;` to the forward declarations near the top of the namespace (before `class TableVersion`).

Rebuild now (`env -C $WT PWD=$WT scripts/wt-build.sh`) with `pick_gtest.cpp` temporarily removed from `cmake/sources_tests.cmake`, run `GTEST_FILTER="$FILTER"`: every pre-existing test passes. Put the line back. This proves the split moved code without changing it.

- [ ] **Step 4: The pivot column and the default-row check in `Load`**

`relation.h`: `load_rows` gains, at the end:

```cpp
	std::span<uint16_t const>         pivot;      // empty, or count: each row's pivot column
```

`relation.cpp` `Load`: after the strings span check:

```cpp
	if (!command.pivot.empty() && command.pivot.size() != command.count)
	{
		return tl::make_unexpected(ErrorContext("load into '", def.name, "' of ", command.count, " rows has ",
			command.pivot.size(), " pivot columns; give none or one per row"));
	}
```

After the unique-order check (still before `table_rows next(def)`):

```cpp
	// A ranked best table answers "nothing matched" with its R == 0 row, so a
	// key run that gains rows must have one. Runs this load does not touch are
	// not re-checked: removing rows is never refused (Retract relies on it).
	if (def.pick == pick_policy::Best && def.ranking != UINT32_MAX)
	{
		uint32_t const rankSlot = current.slot[def.ranking];
		auto sameKey = [&](source a, source b)
		{
			for (uint32_t k = 0; k < def.key_count; ++k)
			{
				uint32_t const s = current.orderSlots[k];
				if (cellsOf(a)[s].i != cellsOf(b)[s].i)
				{
					return false;
				}
			}
			return true;
		};
		size_t start = 0;
		while (start < order.size())
		{
			size_t end = start + 1;
			while (end < order.size() && sameKey(order[start], order[end]))
			{
				++end;
			}
			bool   hasDefault  = false;
			size_t firstLoaded = SIZE_MAX;
			for (size_t i = start; i < end; ++i)
			{
				hasDefault = hasDefault || cellsOf(order[i])[rankSlot].i == 0;
				if (order[i].loaded && firstLoaded == SIZE_MAX)
				{
					firstLoaded = order[i].row;
				}
			}
			if (!hasDefault && firstLoaded != SIZE_MAX)
			{
				if (command.refusal)
				{
					command.refusal->row = firstLoaded;
				}
				std::string key;
				for (uint32_t k = 0; k < def.key_count; ++k)
				{
					key = cc(key, key.empty() ? "" : ", ", cellsOf(order[start])[current.orderSlots[k]].i);
				}
				return tl::make_unexpected(ErrorContext("table '", def.name, "' ranks by '",
					def.columns[def.ranking].name, "', and key (", key,
					") has no row with no flags, the default for an input that matches nothing"));
			}
			start = end;
		}
	}
```

In the copy loop, after `next.owners.push_back(ownerOf(s));`:

```cpp
		next.pivots.push_back(s.loaded ? (command.pivot.empty() ? uint16_t(0) : command.pivot[s.row])
		                               : current.pivots[s.row]);
```

and reserve it with the others: `next.pivots.reserve(order.size());`.

- [ ] **Step 5: `pick.h` and `PickBest`**

Create `include/tabular/pick.h`:

```cpp
#ifndef TABULAR_PICK_H
#define TABULAR_PICK_H

#include "tabular/relation.h"

#include <optional>
#include <span>
#include <vector>

namespace tabular
{

// What a pick is given. Keys select the key run (the table's leading order
// columns); inputs are the table's declared inputs in order, which the caller
// resolved, since Tabular resolves no names; this_in goes to the row's script
// condition.
struct pick_query
{
	std::span<cell const> keys;
	std::span<cell const> inputs;
	void const*           this_in = nullptr;
};

// A row holds when every band holds for its input and its script condition,
// if any, holds; bands are tested first. A script condition that cannot answer
// fails the pick with its error: it is never read as "does not hold".
//
// The best row of the key run: with a ranking column, ranked as animation's
// LookupBest ranks a lookup row (a row whose flags are all in the input first,
// then the larger R & F, then the larger R; ties to the lower pivot column,
// then row order), rows matching nothing skipped, and the R == 0 row tried
// last; without one, in row order. Empty when no row holds.
tl::expected<std::optional<size_t>, ErrorContext> PickBest(TableVersion const& version, pick_query const& query);
tl::expected<std::optional<size_t>, ErrorContext> PickBest(relation_store const& store, uint32_t table, pick_query const& query);

}

#endif // TABULAR_PICK_H
```

Create `src/pick.cpp`:

```cpp
#include "tabular/pick.h"

#include "relation_internal.h"

#include <algorithm>

namespace tabular
{

namespace
{

char const* PolicyName(pick_policy p)
{
	switch (p)
	{
	case pick_policy::None: return "none";
	case pick_policy::Best: return "best";
	case pick_policy::All:  return "all";
	case pick_policy::Or:   return "or";
	}
	return "unknown";
}

// The key run the query selects, once the query matches the table's shape.
tl::expected<row_range, ErrorContext> RunOf(TableVersion const& version, pick_query const& query, pick_policy wanted)
{
	table_def const& def = version_access::Def(version);
	if (def.pick != wanted)
	{
		return tl::make_unexpected(ErrorContext("table '", def.name, "' is a ", PolicyName(def.pick),
			" table, not picked with ", PolicyName(wanted)));
	}
	if (query.keys.size() != def.key_count)
	{
		return tl::make_unexpected(ErrorContext("a pick of '", def.name, "' with ", query.keys.size(),
			" keys; the table has ", def.key_count));
	}
	if (query.inputs.size() != def.inputs.size())
	{
		return tl::make_unexpected(ErrorContext("a pick of '", def.name, "' with ", query.inputs.size(),
			" inputs; the table declares ", def.inputs.size()));
	}
	for (uint32_t i = 0; i < def.inputs.size(); ++i)
	{
		int64_t const v = query.inputs[i].i;
		if (def.inputs[i].type == column_type::Flags && (v < 0 || v > INT64_C(0xFFFFFFFF)))
		{
			return tl::make_unexpected(ErrorContext("input '", def.inputs[i].name, "' of '", def.name,
				"' is a flag set, a 32-bit value widened without sign; got ", v));
		}
	}
	if (def.key_count == 0)
	{
		return row_range{0, version.RowCount()};
	}
	return version.FindOrdered(query.keys);
}

// Bands first, so a script runs only for a row whose native conditions hold.
// Floats, as animation's phase bands compare.
tl::expected<bool, ErrorContext> RowHolds(TableVersion const& version, size_t row, pick_query const& query)
{
	table_def const&  def  = version_access::Def(version);
	table_rows const& rows = version_access::Rows(version);
	for (uint32_t c = 0; c < def.columns.size(); ++c)
	{
		if (def.columns[c].type != column_type::Band)
		{
			continue;
		}
		cell const  band = rows.cells[row * rows.valueCount + rows.slot[c]];
		float const x    = static_cast<float>(query.inputs[def.columns[c].input].d);
		if (!(x >= band.v[0] && x < band.v[1]))
		{
			return false;
		}
	}
	for (uint32_t c = 0; c < def.columns.size(); ++c)
	{
		if (def.columns[c].type != column_type::Condition)
		{
			continue;
		}
		auto holds = version.Holds(row, c, query.this_in);
		if (!holds)
		{
			return tl::make_unexpected(std::move(holds.error()).with("table '", def.name, "' row ", row));
		}
		if (!*holds)
		{
			return false;
		}
	}
	return true;
}

}

tl::expected<std::optional<size_t>, ErrorContext> PickBest(TableVersion const& version, pick_query const& query)
{
	auto run = RunOf(version, query, pick_policy::Best);
	if (!run)
	{
		return tl::make_unexpected(std::move(run.error()));
	}
	table_def const&  def  = version_access::Def(version);
	table_rows const& rows = version_access::Rows(version);

	auto firstHolding = [&](auto const& candidates) -> tl::expected<std::optional<size_t>, ErrorContext>
	{
		for (size_t r : candidates)
		{
			auto holds = RowHolds(version, r, query);
			if (!holds)
			{
				return tl::make_unexpected(std::move(holds.error()));
			}
			if (*holds)
			{
				return std::optional<size_t>(r);
			}
		}
		return std::optional<size_t>();
	};

	std::vector<size_t> inOrder;
	if (def.ranking == UINT32_MAX)
	{
		for (size_t r = run->first; r < run->last; ++r)
		{
			inOrder.push_back(r);
		}
		return firstHolding(inOrder);
	}

	uint32_t const F        = static_cast<uint32_t>(query.inputs[def.columns[def.ranking].input].i);
	uint32_t const rankSlot = rows.slot[def.ranking];
	struct ranked
	{
		size_t   row;
		bool     full;
		uint32_t hit;
		uint32_t required;
		uint16_t pivot;
	};
	std::vector<ranked> candidates;
	std::vector<size_t> defaults;
	for (size_t r = run->first; r < run->last; ++r)
	{
		uint32_t const R = static_cast<uint32_t>(rows.cells[r * rows.valueCount + rankSlot].i);
		if (R == 0)
		{
			defaults.push_back(r);
			continue;
		}
		uint32_t const hit = R & F;
		if (hit == 0)
		{
			continue;   // matches nothing in the input
		}
		candidates.push_back({r, hit == R, hit, R, rows.pivots[r]});
	}
	// Stable, so rows equal in every rank keep row order.
	std::stable_sort(candidates.begin(), candidates.end(), [](ranked const& a, ranked const& b)
	{
		if (a.full != b.full)         { return a.full; }
		if (a.hit != b.hit)           { return a.hit > b.hit; }
		if (a.required != b.required) { return a.required > b.required; }
		return a.pivot < b.pivot;
	});
	for (ranked const& c : candidates)
	{
		inOrder.push_back(c.row);
	}
	inOrder.insert(inOrder.end(), defaults.begin(), defaults.end());
	return firstHolding(inOrder);
}

tl::expected<std::optional<size_t>, ErrorContext> PickBest(relation_store const& store, uint32_t table, pick_query const& query)
{
	auto version = store.Version(table);
	if (!version)
	{
		return tl::make_unexpected(std::move(version.error()));
	}
	return PickBest(**version, query);
}

}
```

(The one-line lambda bodies in the comparator are braced, as the brace rule requires.)

Add to `Spehleon/cmake/sources_tabular.cmake`, after `relation.cpp`:

```cmake
	${SPEHLEON_ROOT}/Tabular/src/pick.cpp
```

- [ ] **Step 6: Build and run**

Run: `env -C $WT PWD=$WT scripts/wt-build.sh && cd $WT && GTEST_FILTER="$FILTER" Build/Engine --gtest > $WT/../tabular-picking-t2.log 2>&1; tail -5 $WT/../tabular-picking-t2.log`
Expected: all pass, the 11 `PickBest` tests included.

- [ ] **Step 7: Mutation checks**

1. In the comparator, swap `return a.full;` for `return !a.full;`. Build, `GTEST_FILTER='PickBest.*'`. Expected: `RanksLikeAnimationsLookupBest` FAILS (case 4 or 5). Restore, `touch src/pick.cpp`.
2. In `Load`'s default check, change `firstLoaded != SIZE_MAX` to `true`. Expected: `RetractingTheDefaultsOwnerIsNeverRefused` FAILS (row count stays 2: the retract's load was refused). Restore, `touch src/relation.cpp`.
3. In `RowHolds`, move the band loop after the condition loop. Expected: `BandsAreTestedBeforeTheScript` FAILS. Restore, `touch src/pick.cpp`.
Rebuild and rerun `$FILTER`: all pass.

- [ ] **Step 8: Commit**

```bash
cd $WT/Spehleon/Tabular && git add include/tabular/relation.h include/tabular/pick.h src/relation_internal.h src/relation.cpp src/pick.cpp test/pick_gtest.cpp && git commit -m "PickBest: ranked like animation's LookupBest, bands before the script; a ranked key run needs a default"
cd $WT/Spehleon && git add Tabular cmake/sources_tabular.cmake && git commit -m "Bump Tabular: PickBest"
cd $WT && git add Spehleon cmake/sources_tests.cmake && git commit -m "Bump Spehleon: PickBest; register its tests"
```

---

### Task 3: PickAll and PickOr

**Files:**
- Modify: `Spehleon/Tabular/include/tabular/pick.h`, `Spehleon/Tabular/src/pick.cpp`
- Modify: `Spehleon/Tabular/test/pick_gtest.cpp`

**Interfaces:**
- Consumes: Task 2's `pick_query`, `RunOf`, `RowHolds` (file-local in `pick.cpp`).
- Produces:

```cpp
tl::expected<std::vector<size_t>, ErrorContext> PickAll(TableVersion const& version, pick_query const& query);
tl::expected<std::vector<size_t>, ErrorContext> PickAll(relation_store const& store, uint32_t table, pick_query const& query);
tl::expected<uint32_t, ErrorContext>            PickOr (TableVersion const& version, pick_query const& query);
tl::expected<uint32_t, ErrorContext>            PickOr (relation_store const& store, uint32_t table, pick_query const& query);
```

- [ ] **Step 1: Write the failing tests**

Append to `pick_gtest.cpp`:

```cpp
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
	EXPECT_TRUE(Mentions(PickBest(phase, 0, pick_query{keys, inputs}).error().format(), "not picked with best"));
	EXPECT_TRUE(Mentions(PickAll(phase, 0, pick_query{keys, inputs}).error().format(), "not picked with all"));
	relation_store ranked = StoreFrom(kRanked);
	cell const mode[] = {cell::Integer(0)};
	EXPECT_TRUE(Mentions(PickOr(ranked, 0, pick_query{keys, mode}).error().format(), "not picked with or"));
}
```

- [ ] **Step 2: Build and watch it fail**

Expected: `PickOr` and `PickAll` not declared.

- [ ] **Step 3: Implement**

Add to `pick.h`, after `PickBest`:

```cpp
// Every row of the key run that holds, in row order; ranking is not used.
tl::expected<std::vector<size_t>, ErrorContext> PickAll(TableVersion const& version, pick_query const& query);
tl::expected<std::vector<size_t>, ErrorContext> PickAll(relation_store const& store, uint32_t table, pick_query const& query);

// The OR of the output column over every row of the key run that holds; 0 when
// none does, so an or table needs no default. This is animation's
// PipelineClosure::eval_phase_bits.
tl::expected<uint32_t, ErrorContext> PickOr(TableVersion const& version, pick_query const& query);
tl::expected<uint32_t, ErrorContext> PickOr(relation_store const& store, uint32_t table, pick_query const& query);
```

Add to `pick.cpp`, before the store overload of `PickBest`:

```cpp
tl::expected<std::vector<size_t>, ErrorContext> PickAll(TableVersion const& version, pick_query const& query)
{
	auto run = RunOf(version, query, pick_policy::All);
	if (!run)
	{
		return tl::make_unexpected(std::move(run.error()));
	}
	std::vector<size_t> out;
	for (size_t r = run->first; r < run->last; ++r)
	{
		auto holds = RowHolds(version, r, query);
		if (!holds)
		{
			return tl::make_unexpected(std::move(holds.error()));
		}
		if (*holds)
		{
			out.push_back(r);
		}
	}
	return out;
}

tl::expected<uint32_t, ErrorContext> PickOr(TableVersion const& version, pick_query const& query)
{
	auto run = RunOf(version, query, pick_policy::Or);
	if (!run)
	{
		return tl::make_unexpected(std::move(run.error()));
	}
	table_def const&  def  = version_access::Def(version);
	table_rows const& rows = version_access::Rows(version);
	uint32_t const    slot = rows.slot[def.output];
	uint32_t          bits = 0;
	for (size_t r = run->first; r < run->last; ++r)
	{
		auto holds = RowHolds(version, r, query);
		if (!holds)
		{
			return tl::make_unexpected(std::move(holds.error()));
		}
		if (*holds)
		{
			bits |= static_cast<uint32_t>(rows.cells[r * rows.valueCount + slot].i);
		}
	}
	return bits;
}
```

and after the store overload of `PickBest`, the two store overloads in the same shape:

```cpp
tl::expected<std::vector<size_t>, ErrorContext> PickAll(relation_store const& store, uint32_t table, pick_query const& query)
{
	auto version = store.Version(table);
	if (!version)
	{
		return tl::make_unexpected(std::move(version.error()));
	}
	return PickAll(**version, query);
}

tl::expected<uint32_t, ErrorContext> PickOr(relation_store const& store, uint32_t table, pick_query const& query)
{
	auto version = store.Version(table);
	if (!version)
	{
		return tl::make_unexpected(std::move(version.error()));
	}
	return PickOr(**version, query);
}
```

- [ ] **Step 4: Build and run** `$FILTER`. Expected: all pass, the 5 new tests included.

- [ ] **Step 5: Mutation check**

In `RowHolds`, change `x < band.v[1]` to `x <= band.v[1]`. Expected: `PickOr.EdgesAreHalfOpenAndNanHoldsNothing` FAILS (`t == end`). Restore, `touch src/pick.cpp`, rebuild, rerun: passes.

- [ ] **Step 6: Commit**

```bash
cd $WT/Spehleon/Tabular && git add include/tabular/pick.h src/pick.cpp test/pick_gtest.cpp && git commit -m "PickAll and PickOr: every holding row, and the OR of their flags as animation's phase bits"
cd $WT/Spehleon && git add Tabular && git commit -m "Bump Tabular: PickAll and PickOr"
cd $WT && git add Spehleon && git commit -m "Bump Spehleon: PickAll and PickOr"
```

---

### Task 4: Pivoted CSV

**Files:**
- Modify: `Spehleon/Tabular/include/tabular/relation.h` (`pivot_by`; `create_table::pivot`; `table_def::pivot_headers/pivot_cells`)
- Modify: `Spehleon/Tabular/src/relation.cpp` (`BuildPivot`)
- Modify: `Spehleon/Tabular/src/schema_yaml.cpp` (`pivot:`)
- Create: `Spehleon/Tabular/src/csv_cells.h` (private: `ParseWhole`, `ReadCell`, `LoadPivotedCsv` declarations)
- Modify: `Spehleon/Tabular/src/rows_csv.cpp` (`ReadCell` out of the anonymous namespace into `detail`; dispatch)
- Create: `Spehleon/Tabular/src/pivot_csv.cpp`
- Modify: `Spehleon/Tabular/include/tabular/rows_csv.h` (`csv_rows::pivoted`, `flag_names`)
- Modify: `Spehleon/cmake/sources_tabular.cmake` (add `pivot_csv.cpp`)
- Create: `Spehleon/Tabular/test/pivot_csv_gtest.cpp`; register it

**Interfaces:**
- Consumes: Tasks 1–3 (`pick_policy`, `ranking`, `output`, `key_count`, `load_rows::pivot`, `PickBest`, `PickOr`).
- Produces:

```cpp
// relation.h
struct pivot_by
{
	uint32_t headers = UINT32_MAX;   // the flags column the headers are members of
	uint32_t cells   = UINT32_MAX;   // the column each non-empty cell fills
};
struct create_table { ...; pivot_by const* pivot = nullptr; };
struct table_def    { ...; uint32_t pivot_headers = UINT32_MAX; uint32_t pivot_cells = UINT32_MAX; };

// rows_csv.h
struct csv_rows
{
	...existing fields...;
	bool                      pivoted    = false;     // the table's pivot: keys, then flag headers
	std::vector<std::string>* flag_names = nullptr;   // filled by a pivoted load: index = bit
};
```

- YAML: `pivot: {headers: <column>, cells: <column>}`.

- [ ] **Step 1: Write the failing tests**

Create `Spehleon/Tabular/test/pivot_csv_gtest.cpp`:

```cpp
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
      - {name: label, type: int, key: true}
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
	return std::string(std::string_view(*store.StringAt(0, **picked, 2)));
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
	EXPECT_TRUE(Mentions(Error(LoadPivot(store, "label,None,Injured|Crouch,Crouch\n1,n,ic,c\n")), "'Crouch'"));
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
		return *PickOr(store, 0, pick_query{keys, inputs});
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
      - {name: label, type: int, key: true}
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
      - {name: label, type: int, key: true}
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

TEST(PivotCsv, EqualRanksFollowHeaderOrder)
{
	relation_store store = StoreFrom(kLookup);
	// Two names for the same flags (duplicates are by name); the left header wins.
	ASSERT_EQ(Error(LoadPivot(store, "label,None,Injured,Crouch,Crouch|Injured,Injured|Crouch\n1,n,,,left,right\n")), "");
	EXPECT_EQ(Clip(store, 1, 3), "left");
}
```

Register in superproject `cmake/sources_tests.cmake` after `pick_gtest.cpp`:

```cmake
	${SPEHLEON_ROOT}/Tabular/test/pivot_csv_gtest.cpp
```

- [ ] **Step 2: Build and watch it fail**

Expected: `csv_rows` has no member `pivoted`; YAML `pivot` unknown.

- [ ] **Step 3: The pivot in the schema**

`relation.h`: add `pivot_by` (Interfaces), `create_table::pivot` at its end, and `table_def::pivot_headers`, `pivot_cells` at its end. Comment above `pivot_by`:

```cpp
// A table written as a grid in CSV: keys down the side, the members of a flags
// column across the top, and each non-empty cell one row. Stored as ordinary
// rows; the pivot is the front end's.
```

`relation.cpp`, after `BuildPick`:

```cpp
tl::expected<void, ErrorContext> BuildPivot(create_table const& c, table_def& out)
{
	if (!c.pivot)
	{
		return {};
	}
	pivot_by const& p = *c.pivot;
	if (out.pick != pick_policy::Best && out.pick != pick_policy::Or)
	{
		return tl::make_unexpected(ErrorContext("table '", c.name, "' is pivoted; only a best or an or table is"));
	}
	uint32_t const headersWanted = out.pick == pick_policy::Best ? out.ranking : out.output;
	if (p.headers == UINT32_MAX || p.headers != headersWanted)
	{
		return tl::make_unexpected(ErrorContext("the pivot headers of table '", c.name, "' are its ",
			out.pick == pick_policy::Best ? "ranking column" : "output column"));
	}
	if (p.cells >= out.columns.size() || p.cells == p.headers || out.columns[p.cells].key)
	{
		return tl::make_unexpected(ErrorContext("the pivot cells of table '", c.name,
			"' are a column that is neither a key nor the headers"));
	}
	column_type const cellType = out.columns[p.cells].type;
	if (cellType == column_type::Condition || cellType == column_type::Command)
	{
		return tl::make_unexpected(ErrorContext("the pivot cells of table '", c.name, "' are a ",
			TypeName(cellType), "; they are a value, a band or a string"));
	}
	for (uint32_t i = 0; i < out.columns.size(); ++i)
	{
		if (!out.columns[i].key && i != p.headers && i != p.cells)
		{
			return tl::make_unexpected(ErrorContext("table '", c.name, "' is pivoted, so it holds only its keys, '",
				out.columns[p.headers].name, "' and '", out.columns[p.cells].name, "'; it also has '",
				out.columns[i].name, "'"));
		}
	}
	out.pivot_headers = p.headers;
	out.pivot_cells   = p.cells;
	return {};
}
```

and in `BuildTable`, after `BuildPick`:

```cpp
	auto pivoted = BuildPivot(c, out);
	if (!pivoted)
	{
		return tl::make_unexpected(std::move(pivoted.error()));
	}
```

`schema_yaml.cpp`:

```cpp
struct pivot_yaml
{
	std::string headers;
	std::string cells;
};
```

`table_yaml` gains `pivot_yaml pivot;`; fields:

```cpp
YamlField const kPivotFields[] = {
	{"headers", FieldType::String, offsetof(pivot_yaml, headers), true},
	{"cells",   FieldType::String, offsetof(pivot_yaml, cells), true},
};
YamlSet const kPivotSet{kPivotFields, nullptr, "pivot"};
```

`kTableFields` gains `{"pivot", FieldType::Nested, offsetof(table_yaml, pivot), false, 0, &kPivotSet},`. In `SchemaFromYaml`, add `std::vector<pivot_by> pivots(document.tables.size());` beside `orders`; per table:

```cpp
		bool const pivoted = tableNodes->as_sequence().items[t]->get("pivot") != nullptr;
		if (pivoted)
		{
			pivots[t] = pivot_by{columnOf(in.pivot.headers), columnOf(in.pivot.cells)};
			if (pivots[t].headers == UINT32_MAX || pivots[t].cells == UINT32_MAX)
			{
				return tl::make_unexpected(ErrorContext("the pivot of table '", in.name, "' names a column the table does not have")
					.with(std::string(label)));
			}
		}
```

and the command gains `pivoted ? &pivots[t] : nullptr` as its last argument.

- [ ] **Step 4: Share the cell readers, and load pivoted CSV**

Create `src/csv_cells.h`:

```cpp
#ifndef TABULAR_CSV_CELLS_H
#define TABULAR_CSV_CELLS_H

// Cell readers shared by the CSV front ends. Not a public header.

#include "tabular/rows_csv.h"

#include <charconv>
#include <string_view>
#include <type_traits>

namespace tabular::detail
{

// The whole of text as T, or nothing: "12abc" is not 12.
template<typename T>
bool ParseWhole(std::string_view text, T& out, int base = 10)
{
	... the body moved unchanged from rows_csv.cpp ...
}

// A value cell of column's type; an empty cell is refused.
tl::expected<cell, ErrorContext> ReadCell(column_def const& column, std::string_view text);

tl::expected<void, ErrorContext> LoadPivotedCsv(relation_store& store, csv_rows const& command);

}

#endif // TABULAR_CSV_CELLS_H
```

In `rows_csv.cpp`: include `"csv_cells.h"`, delete `ParseWhole` from the anonymous namespace, and move `ReadCell` out of it into `namespace detail` (definition unchanged), adding `using detail::ParseWhole; using detail::ReadCell;` in the anonymous namespace. At the top of `LoadCsv`, after the table-index check:

```cpp
	if (command.pivoted)
	{
		return detail::LoadPivotedCsv(store, command);
	}
```

`rows_csv.h`: the two `csv_rows` fields (Interfaces), and in the struct's comment:

```cpp
// A pivoted load reads the table's pivot: the header row names the key
// columns, in key order, then the flag headers, whose order gives the flag
// domain (a best table's first is None, the 0 value; each simple header after
// takes the next bit; `A|B` ORs headers to its left). Each non-empty cell is
// one row, its header's flags in the headers column and the cell in the cells
// column; blank cells give no row.
```

Create `src/pivot_csv.cpp`:

```cpp
#include "csv_cells.h"
#include "Utility/load_csv.h"

#include <cctype>
#include <string>
#include <vector>

namespace tabular::detail
{

namespace
{

// The CSV reader trims whole cells; a combo's parts are trimmed here.
std::string_view Trim(std::string_view s)
{
	while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
	{
		s.remove_prefix(1);
	}
	while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
	{
		s.remove_suffix(1);
	}
	return s;
}

bool IsNone(std::string_view name)
{
	if (name.size() != 4)
	{
		return false;
	}
	char const* none = "none";
	for (size_t i = 0; i < 4; ++i)
	{
		if ((name[i] | 0x20) != none[i])
		{
			return false;
		}
	}
	return true;
}

struct flag_header
{
	std::string name;
	uint32_t    flags  = 0;
	bool        simple = true;
};

// The flag domain the headers spell, read once, left to right.
tl::expected<std::vector<flag_header>, ErrorContext> ReadHeaders(std::vector<std::string_view> const& headers,
	bool firstIsNone)
{
	std::vector<flag_header> out;
	uint32_t nextBit = 0;
	for (size_t h = 0; h < headers.size(); ++h)
	{
		std::string_view const name = headers[h];
		if (name.empty())
		{
			return tl::make_unexpected(ErrorContext("flag header ", h + 1, " is empty"));
		}
		if (h == 0 && firstIsNone)
		{
			if (!IsNone(name))
			{
				return tl::make_unexpected(ErrorContext("the first flag header is None, the 0 value; got '", name, "'"));
			}
			out.push_back({std::string(name), 0, false});
			continue;
		}
		for (flag_header const& earlier : out)
		{
			if (earlier.name == name)
			{
				return tl::make_unexpected(ErrorContext("flag header '", name, "' twice"));
			}
		}
		if (name.find('|') == std::string_view::npos)
		{
			if (nextBit == 32)
			{
				return tl::make_unexpected(ErrorContext("flag header '", name, "' would be a 33rd bit; a flag set has 32 bits"));
			}
			out.push_back({std::string(name), uint32_t(1) << nextBit++, true});
			continue;
		}
		uint32_t         flags = 0;
		std::string_view rest  = name;
		while (true)
		{
			size_t const     bar  = rest.find('|');
			std::string_view part = Trim(rest.substr(0, bar));
			uint32_t         found = 0;
			for (flag_header const& earlier : out)
			{
				if (earlier.simple && earlier.name == part)
				{
					found = earlier.flags;
				}
			}
			if (found == 0)
			{
				return tl::make_unexpected(ErrorContext("combo header '", name, "' names '", part,
					"', which is not a simple header to its left"));
			}
			flags |= found;
			if (bar == std::string_view::npos)
			{
				break;
			}
			rest = rest.substr(bar + 1);
		}
		out.push_back({std::string(name), flags, false});
	}
	return out;
}

}

tl::expected<void, ErrorContext> LoadPivotedCsv(relation_store& store, csv_rows const& command)
{
	auto             label = [&](uint32_t line) { return cc(std::string(), command.label, ":", line); };
	table_def const& def   = store.definition().tables()[command.table];
	if (def.pivot_headers == UINT32_MAX)
	{
		return tl::make_unexpected(ErrorContext("table '", def.name, "' has no pivot").with(std::string(command.label)));
	}

	// Slots as the store numbers them: keys and headers are values; the cells
	// column is a value, a band or a string.
	std::vector<uint32_t> slot(def.columns.size());
	uint32_t valueCount  = 0;
	uint32_t stringCount = 0;
	for (uint32_t c = 0; c < def.columns.size(); ++c)
	{
		slot[c] = def.columns[c].type == column_type::String ? stringCount++ : valueCount++;
	}
	column_def const& cells = def.columns[def.pivot_cells];

	std::string_view text = command.text;
	if (text.starts_with("\xEF\xBB\xBF"))
	{
		text.remove_prefix(3);
	}
	CSV::Options options;
	options.normalizeUnicode = false;
	CSV::File file(text, options, command.first_line);
	if (file.rows() == 0)
	{
		return tl::make_unexpected(ErrorContext("no header row for pivoted table '", def.name, "'")
			.with(std::string(command.label)));
	}
	size_t const width      = file[0].size();
	uint32_t const headerLine = file.line_of_row(0);

	// The keys, in key order (a picked table's keys are its leading order columns).
	if (width < def.key_count)
	{
		return tl::make_unexpected(ErrorContext("the header names ", width, " columns; table '", def.name,
			"' has ", def.key_count, " keys").with(label(headerLine)));
	}
	for (uint32_t k = 0; k < def.key_count; ++k)
	{
		column_def const& key = def.columns[def.order[k]];
		if (file.cell(k, 0) != key.name)
		{
			return tl::make_unexpected(ErrorContext("header cell ", k + 1, " is key column '", key.name, "'; got '",
				file.cell(k, 0), "'").with(label(headerLine)));
		}
	}
	std::vector<std::string_view> names;
	for (size_t h = def.key_count; h < width; ++h)
	{
		names.push_back(file.cell(h, 0));
	}
	auto headers = ReadHeaders(names, def.pick == pick_policy::Best);
	if (!headers)
	{
		return tl::make_unexpected(std::move(headers.error()).with(label(headerLine)));
	}

	std::vector<cell>      values;
	std::vector<string_in> strings;
	std::vector<uint16_t>  pivot;
	std::vector<uint32_t>  lineOf;   // per loaded row
	for (size_t r = 1; r < file.rows(); ++r)
	{
		uint32_t const line = file.line_of_row(r);
		if (file[r].size() != width)
		{
			return tl::make_unexpected(ErrorContext("row has ", file[r].size(), " cells, but the header has ", width)
				.with(label(line)));
		}
		std::vector<cell> keys;
		for (uint32_t k = 0; k < def.key_count; ++k)
		{
			auto key = ReadCell(def.columns[def.order[k]], file.cell(k, r));
			if (!key)
			{
				return tl::make_unexpected(std::move(key.error()).with(label(line)));
			}
			keys.push_back(*key);
		}
		for (size_t h = 0; h < headers->size(); ++h)
		{
			std::string_view const cellText = file.cell(def.key_count + h, r);
			if (cellText.empty())
			{
				continue;   // a blank cell is no row
			}
			std::vector<cell> row(valueCount);
			for (uint32_t k = 0; k < def.key_count; ++k)
			{
				row[slot[def.order[k]]] = keys[k];
			}
			row[slot[def.pivot_headers]] = cell::Integer((*headers)[h].flags);
			if (cells.type == column_type::String)
			{
				strings.emplace_back(cellText);
			}
			else
			{
				auto value = ReadCell(column_def{(*headers)[h].name, cells.type}, cellText);
				if (!value)
				{
					return tl::make_unexpected(std::move(value.error()).with(label(line)));
				}
				row[slot[def.pivot_cells]] = *value;
			}
			values.insert(values.end(), row.begin(), row.end());
			pivot.push_back(static_cast<uint16_t>(h));
			lineOf.push_back(line);
		}
	}

	if (command.flag_names)
	{
		command.flag_names->clear();
		for (flag_header const& h : *headers)
		{
			if (h.simple)
			{
				command.flag_names->push_back(h.name);
			}
		}
	}

	load_refusal refusal;
	auto loaded = store.Load(load_rows{command.table, command.owner, pivot.size(), values, {}, &refusal, {},
		strings, pivot});
	if (!loaded)
	{
		ErrorContext error = std::move(loaded.error());
		if (refusal.earlier < lineOf.size())
		{
			error = std::move(error).with("same key as line ", lineOf[refusal.earlier]);
		}
		if (refusal.row < lineOf.size())
		{
			return tl::make_unexpected(std::move(error).with(label(lineOf[refusal.row])));
		}
		return tl::make_unexpected(std::move(error).with(std::string(command.label)));
	}
	return {};
}

}
```

(`column_def{name, type}` builds a throwaway column so `ReadCell`'s error names the header.)

Note on `flag_names`: it is filled before `Load` so a refused load still reports the domain; that is harmless and keeps the code linear. If the reviewer prefers it filled only on success, move the block after the `if (!loaded)` return.

Add to `Spehleon/cmake/sources_tabular.cmake` after `rows_csv.cpp`:

```cmake
	${SPEHLEON_ROOT}/Tabular/src/pivot_csv.cpp
```

- [ ] **Step 5: Build and run** `$FILTER`. Expected: all pass, the 8 `PivotCsv` tests included.

- [ ] **Step 6: Mutation check**

In `LoadPivotedCsv`, change `pivot.push_back(static_cast<uint16_t>(h));` to `pivot.push_back(static_cast<uint16_t>(headers->size() - 1 - h));` (reversed header order; a constant 0 would not show, since row order also puts `left` first). Expected: `PivotCsv.EqualRanksFollowHeaderOrder` FAILS (`right`). Restore, `touch src/pivot_csv.cpp`, rebuild, rerun: passes.

- [ ] **Step 7: Commit**

```bash
cd $WT/Spehleon/Tabular && git add include/tabular/relation.h include/tabular/rows_csv.h src/relation.cpp src/schema_yaml.cpp src/csv_cells.h src/rows_csv.cpp src/pivot_csv.cpp test/pivot_csv_gtest.cpp && git commit -m "Pivoted CSV: flag headers are the domain, each cell a row, ties to header order"
cd $WT/Spehleon && git add Tabular cmake/sources_tabular.cmake && git commit -m "Bump Tabular: pivoted CSV"
cd $WT && git add Spehleon cmake/sources_tests.cmake && git commit -m "Bump Spehleon: pivoted CSV; register its tests"
```

---

### Task 5: Grids: schema, storage, lookup

**Files:**
- Modify: `Spehleon/Tabular/include/tabular/relation.h` (`grid_axis`, `grid_shape`, `create_table::grid`, `table_def` grid fields, `load_grid`, `grid_refusal`, `relation_store::LoadGrid`, `relation_store::Grid`, forward `class GridVersion;`)
- Create: `Spehleon/Tabular/include/tabular/grid.h`, `Spehleon/Tabular/src/grid.cpp`
- Modify: `Spehleon/Tabular/src/relation_internal.h` (`GridVersion::Data`; `Storage::grids`)
- Modify: `Spehleon/Tabular/src/relation.cpp` (`BuildTable` grid branch; constructor; `Insert`/`Load`/`Version` refuse a grid; `Retract` empties an owner's grid)
- Modify: `Spehleon/cmake/sources_tabular.cmake` (add `grid.cpp`)
- Create: `Spehleon/Tabular/test/grid_gtest.cpp`; register it

**Interfaces:**
- Produces:

```cpp
// relation.h
class GridVersion;

struct grid_axis
{
	std::string_view name;
	column_type      type = column_type::Hash;   // Hash or Int
};

// A dense 2D array: every (row key, column key) pair has one cell of one type.
struct grid_shape
{
	grid_axis   rows;
	grid_axis   columns;
	column_type cells = column_type::Int;   // a value type, String or Command
};

struct create_table { ...; grid_shape const* grid = nullptr; };   // set: this table is a grid

struct table_def
{
	...;
	bool        is_grid      = false;
	column_def  grid_rows;               // name and type of the row axis
	column_def  grid_columns;
	column_type grid_cells   = column_type::Int;
};

// Which keys of a refused grid load clash, as indices into the load's key spans.
struct grid_refusal
{
	size_t row            = SIZE_MAX;
	size_t earlier_row    = SIZE_MAX;
	size_t column         = SIZE_MAX;
	size_t earlier_column = SIZE_MAX;
};

// A whole grid from one contributor: a load replaces it. Cells are row-major,
// row_keys.size() * column_keys.size() of them, in values for a value grid or
// strings for a String or Command grid; the other span stays empty.
struct load_grid
{
	uint32_t                   table = 0;
	uint64_t                   owner = 0;
	std::span<cell const>      row_keys;
	std::span<cell const>      column_keys;
	std::span<cell const>      values;
	std::span<string_in const> strings;
	grid_refusal*              refusal = nullptr;
};

// relation_store
	tl::expected<void, ErrorContext>                           LoadGrid(load_grid const& command);
	tl::expected<counted_ptr<GridVersion const>, ErrorContext> Grid(uint32_t table) const;

// grid.h
class GridVersion
{
public:
	~GridVersion();
	GridVersion(GridVersion const&)            = delete;
	GridVersion& operator=(GridVersion const&) = delete;

	table_def const& definition() const;
	uint32_t Rows() const;
	uint32_t Columns() const;
	uint64_t Owner() const;
	uint64_t Generation() const;

	tl::expected<uint32_t, ErrorContext>       RowOf(cell key) const;
	tl::expected<uint32_t, ErrorContext>       ColumnOf(cell key) const;
	tl::expected<cell, ErrorContext>           At(cell row_key, cell column_key) const;
	tl::expected<counted_string, ErrorContext> StringAt(cell row_key, cell column_key) const;
	cell                                       At(uint32_t row, uint32_t column) const;
	counted_string const&                      StringAt(uint32_t row, uint32_t column) const;

	void AddRef() const { _refs.fetch_add(1, std::memory_order_relaxed); }
	void Release() const;

private:
	friend class relation_store;
	friend struct version_access;
	struct Data;
	explicit GridVersion(std::unique_ptr<Data>&& data);

	std::unique_ptr<Data const> _data;
	mutable std::atomic<int>    _refs{1};
};
```

- [ ] **Step 1: Write the failing tests**

Create `Spehleon/Tabular/test/grid_gtest.cpp`:

```cpp
#include <gtest/gtest.h>
#include "tabular/grid.h"
#include "tabular/pick.h"
#include "tabular/relation.h"

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

relation_store GridStore(column_type cells, column_type axes = column_type::Int)
{
	grid_shape const   shape{{"creature", axes}, {"kind", axes}, cells};
	create_table       t{"g"};
	t.grid = &shape;
	auto s = BuildSchema(std::span<create_table const>(&t, 1));
	EXPECT_TRUE(s.has_value()) << (s ? std::string() : s.error().message);
	return relation_store(std::move(*s));
}

std::vector<cell> Ints(std::vector<int64_t> const& v)
{
	std::vector<cell> out;
	for (int64_t x : v)
	{
		out.push_back(cell::Integer(x));
	}
	return out;
}

}

TEST(Grid, LooksUpByKeysAndByIndices)
{
	relation_store store = GridStore(column_type::Int);
	auto rows    = Ints({10, 20});
	auto columns = Ints({1, 2, 3});
	auto cells   = Ints({11, 12, 13, 21, 22, 23});
	ASSERT_TRUE(store.LoadGrid(load_grid{0, 1, rows, columns, cells}).has_value());
	auto grid = store.Grid(0);
	ASSERT_TRUE(grid.has_value()) << grid.error().message;
	EXPECT_EQ((*grid)->Rows(), 2u);
	EXPECT_EQ((*grid)->Columns(), 3u);
	EXPECT_EQ((*grid)->At(cell::Integer(20), cell::Integer(3))->i, 23);
	uint32_t r = *(*grid)->RowOf(cell::Integer(10));
	uint32_t c = *(*grid)->ColumnOf(cell::Integer(2));
	EXPECT_EQ((*grid)->At(r, c).i, 12);
	EXPECT_FALSE((*grid)->At(cell::Integer(30), cell::Integer(1)).has_value()) << "an unknown key is an error";
	EXPECT_FALSE((*grid)->StringAt(cell::Integer(10), cell::Integer(1)).has_value()) << "a value grid has no strings";
}

TEST(Grid, NegativeIntKeysAreFound)
{
	relation_store store = GridStore(column_type::Int);
	auto rows    = Ints({5, -3, 0});
	auto columns = Ints({-1});
	auto cells   = Ints({50, -30, 0});
	ASSERT_TRUE(store.LoadGrid(load_grid{0, 1, rows, columns, cells}).has_value());
	auto grid = *store.Grid(0);
	EXPECT_EQ(grid->At(cell::Integer(-3), cell::Integer(-1))->i, -30);
	EXPECT_EQ(grid->At(cell::Integer(5), cell::Integer(-1))->i, 50);
	EXPECT_EQ(grid->At(cell::Integer(0), cell::Integer(-1))->i, 0);
}

TEST(Grid, StringAndCommandGridsHoldText)
{
	for (column_type type : {column_type::String, column_type::Command})
	{
		relation_store store = GridStore(type);
		auto rows    = Ints({1});
		auto columns = Ints({1, 2});
		string_in const cells[] = {std::string_view("anim/walk.glb"), std::string_view("(play run)")};
		ASSERT_TRUE(store.LoadGrid(load_grid{0, 1, rows, columns, {}, cells}).has_value());
		auto grid = *store.Grid(0);
		EXPECT_EQ(std::string_view(*grid->StringAt(cell::Integer(1), cell::Integer(1))), "anim/walk.glb");
		EXPECT_EQ(std::string_view(grid->StringAt(0, 1)), "(play run)");
		EXPECT_FALSE(grid->At(cell::Integer(1), cell::Integer(1)).has_value()) << "a text grid has no values";
	}
}

TEST(Grid, AHeldVersionKeepsItsCells)
{
	relation_store store = GridStore(column_type::Int);
	auto rows    = Ints({1});
	auto columns = Ints({1});
	auto first   = Ints({100});
	auto second  = Ints({200});
	ASSERT_TRUE(store.LoadGrid(load_grid{0, 1, rows, columns, first}).has_value());
	auto held = *store.Grid(0);
	ASSERT_TRUE(store.LoadGrid(load_grid{0, 1, rows, columns, second}).has_value());
	EXPECT_EQ(held->At(0, 0).i, 100);
	EXPECT_EQ((*store.Grid(0))->At(0, 0).i, 200);
	EXPECT_GT((*store.Grid(0))->Generation(), held->Generation());
}

TEST(Grid, DuplicateKeysAndWrongSizesAreRefused)
{
	relation_store store = GridStore(column_type::Int);
	auto rows    = Ints({1, 2, 1});
	auto columns = Ints({1});
	auto cells   = Ints({1, 2, 3});
	grid_refusal refusal;
	auto dupRow = store.LoadGrid(load_grid{0, 1, rows, columns, cells, {}, &refusal});
	ASSERT_FALSE(dupRow.has_value());
	EXPECT_EQ(refusal.row, 2u);
	EXPECT_EQ(refusal.earlier_row, 0u);

	auto oneRow  = Ints({1});
	auto dupCols = Ints({4, 4});
	auto two     = Ints({1, 2});
	grid_refusal colRefusal;
	ASSERT_FALSE(store.LoadGrid(load_grid{0, 1, oneRow, dupCols, two, {}, &colRefusal}).has_value());
	EXPECT_EQ(colRefusal.column, 1u);
	EXPECT_EQ(colRefusal.earlier_column, 0u);

	auto three = Ints({1, 2, 3});
	auto wrong = store.LoadGrid(load_grid{0, 1, oneRow, two, three});
	ASSERT_FALSE(wrong.has_value());
	EXPECT_TRUE(Mentions(wrong.error().message, "3 cells")) << wrong.error().message;
	EXPECT_EQ((*store.Grid(0))->Rows(), 0u) << "refusals change nothing";
}

TEST(Grid, RetractEmptiesTheOwnersGrid)
{
	relation_store store = GridStore(column_type::Int);
	auto rows = Ints({1});
	auto cols = Ints({1});
	auto cell1 = Ints({9});
	ASSERT_TRUE(store.LoadGrid(load_grid{0, 7, rows, cols, cell1}).has_value());
	store.Retract(retract_owner{8});
	EXPECT_EQ((*store.Grid(0))->Rows(), 1u) << "another owner's retract leaves it";
	store.Retract(retract_owner{7});
	EXPECT_EQ((*store.Grid(0))->Rows(), 0u);
	EXPECT_FALSE((*store.Grid(0))->At(cell::Integer(1), cell::Integer(1)).has_value());
}

TEST(Grid, TableCallsRefuseAGridAndGridCallsRefuseATable)
{
	grid_shape const shape{{"r", column_type::Int}, {"c", column_type::Int}, column_type::Int};
	add_column const columns[] = {{"x", column_type::Int}};
	create_table tables[2] = {{"g"}, {"t", columns, nullptr, true}};
	tables[0].grid = &shape;
	auto s = BuildSchema(tables);
	ASSERT_TRUE(s.has_value()) << s.error().message;
	relation_store store(std::move(*s));
	cell const one[] = {cell::Integer(1)};
	EXPECT_TRUE(Mentions(store.Insert(insert_row{0, 1, one}).error().message, "is a grid"));
	EXPECT_TRUE(Mentions(store.Load(load_rows{0, 1, 1, one}).error().message, "is a grid"));
	EXPECT_TRUE(Mentions(store.Version(0).error().message, "is a grid"));
	EXPECT_FALSE(PickBest(store, 0, pick_query{}).has_value());
	EXPECT_TRUE(Mentions(store.Grid(1).error().message, "not a grid"));
	EXPECT_TRUE(Mentions(store.LoadGrid(load_grid{1, 1, one, one, one}).error().message, "not a grid"));
}

TEST(Grid, AGridIsDeclaredWithAxesAndOneCellType)
{
	auto build = [](grid_shape const& shape, std::span<add_column const> columns = {})
	{
		create_table t{"g", columns};
		t.grid = &shape;
		return BuildSchema(std::span<create_table const>(&t, 1));
	};
	EXPECT_FALSE(build({{"r", column_type::Double}, {"c", column_type::Int}, column_type::Int}).has_value());
	EXPECT_FALSE(build({{"r", column_type::Int}, {"c", column_type::Int}, column_type::Condition}).has_value());
	EXPECT_FALSE(build({{"r", column_type::Int}, {"c", column_type::Int}, column_type::Band}).has_value());
	EXPECT_FALSE(build({{"", column_type::Int}, {"c", column_type::Int}, column_type::Int}).has_value());
	add_column const extra[] = {{"x", column_type::Int}};
	EXPECT_FALSE(build({{"r", column_type::Int}, {"c", column_type::Int}, column_type::Int}, extra).has_value());
	auto ok = build({{"r", column_type::Hash}, {"c", column_type::Int}, column_type::String});
	ASSERT_TRUE(ok.has_value()) << ok.error().message;
	EXPECT_TRUE(ok->tables()[0].is_grid);
	EXPECT_TRUE(ok->tables()[0].read_only);
}
```

Register after `pivot_csv_gtest.cpp`:

```cmake
	${SPEHLEON_ROOT}/Tabular/test/grid_gtest.cpp
```

- [ ] **Step 2: Build and watch it fail**

Expected: `tabular/grid.h` not found.

- [ ] **Step 3: Grid schema**

`relation.h`: the structs from Interfaces (`create_table::grid` and the `table_def` grid fields at the ends; `grid_refusal` and `load_grid` after `load_rows`), and on `relation_store`, after `Load`:

```cpp
	// A grid's whole next version, checked and then put in place; on refusal
	// the current one is untouched. Grid tables only.
	tl::expected<void, ErrorContext>                           LoadGrid(load_grid const& command);
	// A grid's current version; refused for a table that is not a grid.
	tl::expected<counted_ptr<GridVersion const>, ErrorContext> Grid(uint32_t table) const;
```

`relation.cpp`, a grid branch at the start of `BuildTable` (after the name checks):

```cpp
	if (c.grid)
	{
		return BuildGrid(c);
	}
```

with, before `BuildTable`:

```cpp
// A grid is read-only, has two integer axes and one cell type, and none of a
// row table's shape.
tl::expected<table_def, ErrorContext> BuildGrid(create_table const& c)
{
	grid_shape const& g = *c.grid;
	if (!c.columns.empty() || c.ordering || !c.inputs.empty() || c.pick != pick_policy::None
		|| c.output != UINT32_MAX || c.pivot)
	{
		return tl::make_unexpected(ErrorContext("grid '", c.name,
			"' has rows, columns and cells; it has no columns, order, inputs, pick or pivot"));
	}
	for (grid_axis const* axis : {&g.rows, &g.columns})
	{
		if (axis->name.empty())
		{
			return tl::make_unexpected(ErrorContext("an axis of grid '", c.name, "' needs a name"));
		}
		if (axis->type != column_type::Hash && axis->type != column_type::Int)
		{
			return tl::make_unexpected(ErrorContext("axis '", axis->name, "' of grid '", c.name, "' is a ",
				TypeName(axis->type), "; an axis is a hash or an int"));
		}
	}
	if (g.cells == column_type::Condition || g.cells == column_type::Band)
	{
		return tl::make_unexpected(ErrorContext("the cells of grid '", c.name, "' are ", TypeName(g.cells),
			"s; a grid holds values, strings or commands"));
	}
	table_def out{std::string(c.name), {}, {}, false, true};
	out.is_grid      = true;
	out.grid_rows    = column_def{std::string(g.rows.name), g.rows.type};
	out.grid_columns = column_def{std::string(g.columns.name), g.columns.type};
	out.grid_cells   = g.cells;
	return out;
}
```

(`BuildTable`'s grid branch comes before `BuildColumns`, whose "has no columns" refusal would otherwise fire.)

- [ ] **Step 4: `GridVersion` and the store's grid slots**

Create `include/tabular/grid.h` with the class from Interfaces, headed:

```cpp
#ifndef TABULAR_GRID_H
#define TABULAR_GRID_H

#include "tabular/relation.h"

#include <atomic>
#include <memory>

namespace tabular
{

// One version of a grid: a dense 2D array of one cell type, built whole by
// LoadGrid and never modified, like a TableVersion. Keys are looked up by
// binary search; a hot caller resolves RowOf / ColumnOf once and reads by
// index. The index overloads assert their bounds: indices come from RowOf and
// ColumnOf, and asserts are live in release, so a bad one aborts rather than
// reading past the array.
class GridVersion { ... };

}

#endif // TABULAR_GRID_H
```

`relation_internal.h`: `#include "tabular/grid.h"`, and:

```cpp
struct GridVersion::Data
{
	table_def                                 def;
	uint64_t                                  owner      = 0;
	uint64_t                                  generation = 0;
	uint32_t                                  rows       = 0;
	uint32_t                                  columns    = 0;
	std::vector<std::pair<int64_t, uint32_t>> rowKeys;      // (key, position), sorted by key
	std::vector<std::pair<int64_t, uint32_t>> columnKeys;
	std::vector<cell>                         values;       // value grid: rows * columns, row-major
	std::vector<counted_string>               strings;      // String or Command grid
};
```

`Storage` gains:

```cpp
	std::vector<counted_ptr<GridVersion const>> grids;      // null for a table that is not a grid

	static counted_ptr<GridVersion const> NewGrid(GridVersion::Data&& data)
	{
		auto owned = std::make_unique<GridVersion::Data>(std::move(data));
		return UncountedWrap(static_cast<GridVersion const*>(new GridVersion(std::move(owned))));
	}
```

`relation.h`: add `class GridVersion;` to the forward declarations.

`relation.cpp`:

- Constructor: `_storage->grids.resize(defs.size());` and in the read-only loop:

```cpp
		if (defs[t].is_grid)
		{
			_storage->grids[t] = Storage::NewGrid(GridVersion::Data{defs[t]});
		}
		else if (defs[t].read_only)
		{
			_storage->versions[t] = Storage::NewVersion(defs[t], table_rows(defs[t]));
		}
```

- `Insert`, before the read-only check, and `Load`, right after taking `def` (before `current` is read: a grid has no table version):

```cpp
	if (def.is_grid)
	{
		return refuse(ErrorContext("table '", def.name, "' is a grid; it is loaded with LoadGrid"));
	}
```

(in `Load`, `return tl::make_unexpected(ErrorContext(...))` — its held closures release on return).

- `Version`, before the writable check:

```cpp
	if (defs[table].is_grid)
	{
		return tl::make_unexpected(ErrorContext("table '", defs[table].name, "' is a grid; read it with Grid"));
	}
```

- `Retract`, first in the loop:

```cpp
		if (defs[t].is_grid)
		{
			GridVersion::Data const& data = *_storage->grids[t]->_data;
			if (data.owner == command.owner && data.rows != 0)
			{
				GridVersion::Data empty{defs[t]};
				empty.generation = data.generation + 1;
				_storage->grids[t] = Storage::NewGrid(std::move(empty));
			}
			continue;
		}
```

(`relation_store` is a friend of `GridVersion`, so `_data` is reachable here.)

Create `src/grid.cpp`:

```cpp
#include "tabular/grid.h"

#include "relation_internal.h"

#include <algorithm>
#include <cassert>

namespace tabular
{

namespace
{

bool IsTextGrid(column_type cells)
{
	return cells == column_type::String || cells == column_type::Command;
}

bool Fits32(int64_t v)
{
	return v >= 0 && v <= INT64_C(0xFFFFFFFF);
}

// Sorted (key, position) pairs; on a clash, the two positions.
struct sorted_keys
{
	std::vector<std::pair<int64_t, uint32_t>> keys;
	size_t                                    clash   = SIZE_MAX;
	size_t                                    earlier = SIZE_MAX;
};

sorted_keys SortKeys(std::span<cell const> in)
{
	sorted_keys out;
	out.keys.reserve(in.size());
	for (uint32_t i = 0; i < in.size(); ++i)
	{
		out.keys.push_back({in[i].i, i});
	}
	std::sort(out.keys.begin(), out.keys.end());
	for (size_t i = 1; i < out.keys.size(); ++i)
	{
		if (out.keys[i].first == out.keys[i - 1].first)
		{
			out.clash   = std::max(out.keys[i].second, out.keys[i - 1].second);
			out.earlier = std::min(out.keys[i].second, out.keys[i - 1].second);
			break;
		}
	}
	return out;
}

tl::expected<uint32_t, ErrorContext> Find(std::vector<std::pair<int64_t, uint32_t>> const& keys, cell key,
	column_def const& axis, table_def const& def)
{
	auto found = std::lower_bound(keys.begin(), keys.end(), std::pair<int64_t, uint32_t>{key.i, 0});
	if (found == keys.end() || found->first != key.i)
	{
		return tl::make_unexpected(ErrorContext("grid '", def.name, "' has no ", axis.name, " ", key.i));
	}
	return found->second;
}

}

GridVersion::GridVersion(std::unique_ptr<Data>&& data) : _data(std::move(data)) {}
GridVersion::~GridVersion() = default;

void GridVersion::Release() const
{
	if (_refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
	{
		delete this;
	}
}

table_def const& GridVersion::definition() const { return _data->def; }
uint32_t GridVersion::Rows() const               { return _data->rows; }
uint32_t GridVersion::Columns() const            { return _data->columns; }
uint64_t GridVersion::Owner() const              { return _data->owner; }
uint64_t GridVersion::Generation() const         { return _data->generation; }

tl::expected<uint32_t, ErrorContext> GridVersion::RowOf(cell key) const
{
	return Find(_data->rowKeys, key, _data->def.grid_rows, _data->def);
}

tl::expected<uint32_t, ErrorContext> GridVersion::ColumnOf(cell key) const
{
	return Find(_data->columnKeys, key, _data->def.grid_columns, _data->def);
}

tl::expected<cell, ErrorContext> GridVersion::At(cell row_key, cell column_key) const
{
	if (IsTextGrid(_data->def.grid_cells))
	{
		return tl::make_unexpected(ErrorContext("grid '", _data->def.name, "' holds text; read it with StringAt"));
	}
	auto r = RowOf(row_key);
	if (!r)
	{
		return tl::make_unexpected(std::move(r.error()));
	}
	auto c = ColumnOf(column_key);
	if (!c)
	{
		return tl::make_unexpected(std::move(c.error()));
	}
	return At(*r, *c);
}

tl::expected<counted_string, ErrorContext> GridVersion::StringAt(cell row_key, cell column_key) const
{
	if (!IsTextGrid(_data->def.grid_cells))
	{
		return tl::make_unexpected(ErrorContext("grid '", _data->def.name, "' holds values; read it with At"));
	}
	auto r = RowOf(row_key);
	if (!r)
	{
		return tl::make_unexpected(std::move(r.error()));
	}
	auto c = ColumnOf(column_key);
	if (!c)
	{
		return tl::make_unexpected(std::move(c.error()));
	}
	return StringAt(*r, *c);
}

cell GridVersion::At(uint32_t row, uint32_t column) const
{
	assert(row < _data->rows && column < _data->columns && !IsTextGrid(_data->def.grid_cells));
	return _data->values[size_t(row) * _data->columns + column];
}

counted_string const& GridVersion::StringAt(uint32_t row, uint32_t column) const
{
	assert(row < _data->rows && column < _data->columns && IsTextGrid(_data->def.grid_cells));
	return _data->strings[size_t(row) * _data->columns + column];
}

tl::expected<void, ErrorContext> relation_store::LoadGrid(load_grid const& command)
{
	auto defs = _storage->definition.tables();
	if (command.table >= defs.size())
	{
		return tl::make_unexpected(ErrorContext("load into table ", command.table, ", but the schema has ", defs.size(), " tables"));
	}
	table_def const& def = defs[command.table];
	if (!def.is_grid)
	{
		return tl::make_unexpected(ErrorContext("table '", def.name, "' is not a grid; its rows come from Load or Insert"));
	}
	bool const   text  = IsTextGrid(def.grid_cells);
	size_t const given = text ? command.strings.size() : command.values.size();
	// Divided, not multiplied, as Load checks its spans: the product of two
	// span sizes could wrap.
	size_t const rows    = command.row_keys.size();
	size_t const columns = command.column_keys.size();
	bool const   fits    = rows == 0 || columns == 0 ? given == 0 : given % rows == 0 && given / rows == columns;
	if (!fits || (text ? !command.values.empty() : !command.strings.empty()))
	{
		return tl::make_unexpected(ErrorContext("grid '", def.name, "' of ", rows, " rows and ", columns,
			" columns was given ", given, " cells"));
	}
	if (rows > UINT32_MAX || columns > UINT32_MAX)
	{
		return tl::make_unexpected(ErrorContext("grid '", def.name, "' is too large"));
	}
	for (auto const& [keys, axis] : {std::pair{command.row_keys, &def.grid_rows}, std::pair{command.column_keys, &def.grid_columns}})
	{
		for (cell k : keys)
		{
			if (axis->type == column_type::Hash && !Fits32(k.i))
			{
				return tl::make_unexpected(ErrorContext(axis->name, " of grid '", def.name,
					"' is a hash, a 32-bit value widened without sign; got ", k.i));
			}
		}
	}
	if (def.grid_cells == column_type::Hash || def.grid_cells == column_type::Flags)
	{
		for (cell v : command.values)
		{
			if (!Fits32(v.i))
			{
				return tl::make_unexpected(ErrorContext("the cells of grid '", def.name,
					"' are 32-bit values widened without sign; got ", v.i));
			}
		}
	}
	sorted_keys rowKeys = SortKeys(command.row_keys);
	if (rowKeys.clash != SIZE_MAX)
	{
		if (command.refusal)
		{
			command.refusal->row         = rowKeys.clash;
			command.refusal->earlier_row = rowKeys.earlier;
		}
		return tl::make_unexpected(ErrorContext("grid '", def.name, "' has ", def.grid_rows.name, " ",
			command.row_keys[rowKeys.clash].i, " twice"));
	}
	sorted_keys columnKeys = SortKeys(command.column_keys);
	if (columnKeys.clash != SIZE_MAX)
	{
		if (command.refusal)
		{
			command.refusal->column         = columnKeys.clash;
			command.refusal->earlier_column = columnKeys.earlier;
		}
		return tl::make_unexpected(ErrorContext("grid '", def.name, "' has ", def.grid_columns.name, " ",
			command.column_keys[columnKeys.clash].i, " twice"));
	}

	GridVersion::Data next{def};
	next.owner      = command.owner;
	next.generation = _storage->grids[command.table]->_data->generation + 1;
	next.rows       = static_cast<uint32_t>(rows);
	next.columns    = static_cast<uint32_t>(columns);
	next.rowKeys    = std::move(rowKeys.keys);
	next.columnKeys = std::move(columnKeys.keys);
	if (text)
	{
		next.strings.reserve(given);
		for (string_in s : command.strings)
		{
			next.strings.push_back(counted_string::MakeShared(s.text));
		}
	}
	else
	{
		next.values.assign(command.values.begin(), command.values.end());
	}
	_storage->grids[command.table] = Storage::NewGrid(std::move(next));
	return {};
}

tl::expected<counted_ptr<GridVersion const>, ErrorContext> relation_store::Grid(uint32_t table) const
{
	auto defs = _storage->definition.tables();
	if (table >= defs.size())
	{
		return tl::make_unexpected(ErrorContext("no table ", table));
	}
	if (!defs[table].is_grid)
	{
		return tl::make_unexpected(ErrorContext("table '", defs[table].name, "' is not a grid"));
	}
	return _storage->grids[table];
}

}
```

(The `for (auto const& [keys, axis] : {...})` over an initializer list of pairs needs `#include <utility>`; if the compiler balks at deducing the list, write the two loops out.)

Add to `Spehleon/cmake/sources_tabular.cmake` after `pick.cpp`:

```cmake
	${SPEHLEON_ROOT}/Tabular/src/grid.cpp
```

- [ ] **Step 5: Build and run** `$FILTER`. Expected: all pass, the 8 `Grid` tests included.

- [ ] **Step 6: Mutation check**

In `SortKeys`, change `std::sort` to nothing (comment the line out). Expected: `Grid.NegativeIntKeysAreFound` FAILS (binary search over unsorted keys). Restore, `touch src/grid.cpp`, rebuild, rerun: passes.

- [ ] **Step 7: Commit**

```bash
cd $WT/Spehleon/Tabular && git add include/tabular/relation.h include/tabular/grid.h src/relation_internal.h src/relation.cpp src/grid.cpp test/grid_gtest.cpp && git commit -m "Grids: a dense 2D array of one cell type, versioned like a read-only table"
cd $WT/Spehleon && git add Tabular cmake/sources_tabular.cmake && git commit -m "Bump Tabular: grids"
cd $WT && git add Spehleon cmake/sources_tests.cmake && git commit -m "Bump Spehleon: grids; register their tests"
```

---

### Task 6: Grid CSV and YAML; docs; full suite

**Files:**
- Modify: `Spehleon/Tabular/include/tabular/rows_csv.h` (`csv_grid`, `LoadGridCsv`)
- Create: `Spehleon/Tabular/src/grid_csv.cpp`
- Modify: `Spehleon/Tabular/src/schema_yaml.cpp` (`grid:`; `columns` no longer required)
- Modify: `Spehleon/cmake/sources_tabular.cmake` (add `grid_csv.cpp`)
- Modify: `Spehleon/Tabular/test/grid_gtest.cpp`, `Spehleon/Tabular/test/schema_yaml_gtest.cpp`
- Modify: `Spehleon/Tabular/docs/definition.md`, `Spehleon/Tabular/docs/feature-checklist.md`, the spec

**Interfaces:**
- Consumes: Task 5's `load_grid`, `grid_refusal`, `relation_store::LoadGrid`/`Grid`, `GridVersion`; Task 4's `detail::ReadCell`, `detail::ParseWhole` (`src/csv_cells.h`).
- Produces:

```cpp
// rows_csv.h
// A grid in CSV: the header row's cells after the first are the column keys,
// each line's first cell its row key, and every other cell one grid cell.
// Keys read as key cells (a hash is CityHash32 of the text, an int decimal).
// Every cell is filled: a blank one is refused, naming its line and column
// header. Trimming, '#' lines, blank lines and the BOM are as in LoadCsv.
struct csv_grid
{
	uint32_t         table      = 0;
	uint64_t         owner      = 0;
	std::string_view text;
	std::string_view label;
	uint32_t         first_line = 1;
};

tl::expected<void, ErrorContext> LoadGridCsv(relation_store& store, csv_grid const& command);
```

- YAML: `grid: {rows: {name, type}, columns: {name, type}, cells: <type>}`.

- [ ] **Step 1: Write the failing tests**

Append to `grid_gtest.cpp` (add `#include "tabular/rows_csv.h"` and `#include "tabular/schema_yaml.h"` at its top):

```cpp
namespace
{

constexpr std::string_view kClipByKind = R"(
tables:
  - name: clip_by_kind
    grid: {rows: {name: creature, type: hash}, columns: {name: kind, type: int}, cells: string}
)";

relation_store YamlGridStore(std::string_view yaml)
{
	auto s = SchemaFromYaml(yaml, "grid.yaml");
	EXPECT_TRUE(s.has_value()) << (s ? std::string() : s.error().format());
	return relation_store(std::move(*s));
}

std::string GridError(relation_store& store, std::string_view text)
{
	auto r = LoadGridCsv(store, csv_grid{0, 1, text, "grid.csv"});
	return r ? std::string() : r.error().format();
}

}

TEST(GridCsv, LoadsADenseStringGridFromYamlAndCsv)
{
	relation_store store = YamlGridStore(kClipByKind);
	ASSERT_TRUE(store.definition().tables()[0].is_grid);
	ASSERT_EQ(GridError(store, "creature,1,2\ngrendel, anim/g_walk.glb ,anim/g_run.glb\nnorn,anim/n_walk.glb,anim/n_run.glb\n"), "");
	auto grid = *store.Grid(0);
	EXPECT_EQ(grid->Rows(), 2u);
	// Row indices are line order; row keys are CityHash32 of the names, so read by index.
	uint32_t const run = *grid->ColumnOf(cell::Integer(2));
	EXPECT_EQ(std::string_view(grid->StringAt(0, run)), "anim/g_run.glb");
	EXPECT_EQ(std::string_view(grid->StringAt(1, run)), "anim/n_run.glb");
	EXPECT_EQ(std::string_view(grid->StringAt(0, 0)), "anim/g_walk.glb") << "cells are trimmed";
}

TEST(GridCsv, BlankCellsAreErrorsNamingLineAndHeader)
{
	relation_store store = YamlGridStore(kClipByKind);
	std::string error = GridError(store, "creature,1,2\ngrendel,a,b\nnorn,c,\n");
	EXPECT_TRUE(Mentions(error, "'2'")) << error;
	EXPECT_TRUE(Mentions(error, "grid.csv:3")) << error;
}

TEST(GridCsv, RaggedLinesDuplicatesAndBadCellsAreRefused)
{
	relation_store store = YamlGridStore(kClipByKind);
	EXPECT_TRUE(Mentions(GridError(store, "creature,1,2\ngrendel,a\n"), "grid.csv:2"));
	std::string dupRow = GridError(store, "creature,1\ngrendel,a\nnorn,b\ngrendel,c\n");
	EXPECT_TRUE(Mentions(dupRow, "grid.csv:4")) << dupRow;
	EXPECT_TRUE(Mentions(dupRow, "line 2")) << dupRow;
	std::string dupColumn = GridError(store, "creature,1,1\ngrendel,a,b\n");
	EXPECT_TRUE(Mentions(dupColumn, "'1'")) << dupColumn;
	EXPECT_TRUE(Mentions(GridError(store, "creature,x\ngrendel,a\n"), "'kind'")) << "an int axis key that is not an int";

	relation_store ints = YamlGridStore(R"(
tables:
  - name: g
    grid: {rows: {name: r, type: int}, columns: {name: c, type: int}, cells: double}
)");
	EXPECT_TRUE(Mentions(GridError(ints, "r,1\n1,fast\n"), "grid.csv:2"));
	EXPECT_EQ(GridError(ints, "r,-1,2\n-5,0.5,1e3\n"), "");
	EXPECT_EQ((*ints.Grid(0))->At(cell::Integer(-5), cell::Integer(2))->d, 1000.0);
}

TEST(GridCsv, AGridIsNotLoadedAsRowsNorRowsAsAGrid)
{
	relation_store store = YamlGridStore(kClipByKind);
	EXPECT_FALSE(LoadCsv(store, csv_rows{0, 1, "creature\nx\n", "g.csv"}).has_value());
	relation_store rows = YamlGridStore(R"(
tables:
  - name: t
    read_only: true
    columns:
      - {name: x, type: int}
)");
	EXPECT_TRUE(Mentions(GridError(rows, "x,1\n1,2\n"), "not a grid"));
}
```

Append to `schema_yaml_gtest.cpp`:

```cpp
TEST(SchemaYaml, AGridHasAxesAndCellsAndNoColumns)
{
	auto ok = SchemaFromYaml(R"(
tables:
  - name: g
    grid: {rows: {name: creature, type: hash}, columns: {name: kind, type: hash}, cells: command}
)", "g.yaml");
	ASSERT_TRUE(ok.has_value()) << ok.error().format();
	table_def const& def = ok->tables()[0];
	EXPECT_TRUE(def.is_grid);
	EXPECT_EQ(def.grid_rows.name, "creature");
	EXPECT_EQ(def.grid_cells, column_type::Command);

	auto both = SchemaFromYaml(R"(
tables:
  - name: g
    grid: {rows: {name: r, type: int}, columns: {name: c, type: int}, cells: int}
    columns:
      - {name: x, type: int}
)", "g.yaml");
	EXPECT_FALSE(both.has_value());

	auto neither = SchemaFromYaml(R"(
tables:
  - name: t
)", "t.yaml");
	ASSERT_FALSE(neither.has_value());
	EXPECT_TRUE(Mentions(neither.error().format(), "no columns")) << neither.error().format();
}
```

- [ ] **Step 2: Build and watch it fail**

Expected: `csv_grid` / `LoadGridCsv` undeclared; YAML `grid` unknown.

- [ ] **Step 3: YAML**

`schema_yaml.cpp`:

```cpp
struct axis_yaml
{
	std::string name;
	int32_t     type = -1;
};

struct grid_yaml
{
	axis_yaml rows;
	axis_yaml columns;
	int32_t   cells = -1;
};
```

`table_yaml` gains `grid_yaml grid;`. Fields:

```cpp
YamlField const kAxisFields[] = {
	{"name", FieldType::String, offsetof(axis_yaml, name), true},
	{"type", FieldType::Enum,   offsetof(axis_yaml, type), true, 0, nullptr, nullptr, &kTypes},
};
YamlSet const kAxisSet{kAxisFields, nullptr, "axis"};

YamlField const kGridFields[] = {
	{"rows",    FieldType::Nested, offsetof(grid_yaml, rows), true, 0, &kAxisSet},
	{"columns", FieldType::Nested, offsetof(grid_yaml, columns), true, 0, &kAxisSet},
	{"cells",   FieldType::Enum,   offsetof(grid_yaml, cells), true, 0, nullptr, nullptr, &kTypes},
};
YamlSet const kGridSet{kGridFields, nullptr, "grid"};
```

In `kTableFields`, `columns` becomes optional (its `true` becomes `false`: a grid has none, and `BuildSchema` refuses a row table without columns, "has no columns"), and add `{"grid", FieldType::Nested, offsetof(table_yaml, grid), false, 0, &kGridSet},`. In `SchemaFromYaml`, `std::vector<grid_shape> grids(document.tables.size());` and per table:

```cpp
		bool const isGrid = tableNodes->as_sequence().items[t]->get("grid") != nullptr;
		if (isGrid)
		{
			grids[t] = grid_shape{{in.grid.rows.name, static_cast<column_type>(in.grid.rows.type)},
			                      {in.grid.columns.name, static_cast<column_type>(in.grid.columns.type)},
			                      static_cast<column_type>(in.grid.cells)};
		}
```

and the command gains `isGrid ? &grids[t] : nullptr` as its last argument. (`grid_axis::name` is a view into `document`, which outlives `BuildSchema`.)

- [ ] **Step 4: `LoadGridCsv`**

`rows_csv.h`: add `csv_grid` and `LoadGridCsv` (Interfaces).

Create `src/grid_csv.cpp`:

```cpp
#include "csv_cells.h"
#include "Utility/load_csv.h"

#include <string>
#include <vector>

namespace tabular
{

tl::expected<void, ErrorContext> LoadGridCsv(relation_store& store, csv_grid const& command)
{
	auto label = [&](uint32_t line) { return cc(std::string(), command.label, ":", line); };
	auto defs  = store.definition().tables();
	if (command.table >= defs.size())
	{
		return tl::make_unexpected(ErrorContext("no table ", command.table).with(std::string(command.label)));
	}
	table_def const& def = defs[command.table];
	if (!def.is_grid)
	{
		return tl::make_unexpected(ErrorContext("table '", def.name, "' is not a grid").with(std::string(command.label)));
	}

	std::string_view text = command.text;
	if (text.starts_with("\xEF\xBB\xBF"))
	{
		text.remove_prefix(3);
	}
	CSV::Options options;
	options.normalizeUnicode = false;
	CSV::File file(text, options, command.first_line);
	if (file.rows() == 0 || file[0].size() < 2)
	{
		return tl::make_unexpected(ErrorContext("grid '", def.name, "' needs a header row naming its ",
			def.grid_columns.name, " keys").with(std::string(command.label)));
	}
	size_t const   width      = file[0].size();
	uint32_t const headerLine = file.line_of_row(0);

	std::vector<cell> columnKeys;
	for (size_t h = 1; h < width; ++h)
	{
		auto key = detail::ReadCell(def.grid_columns, file.cell(h, 0));
		if (!key)
		{
			return tl::make_unexpected(std::move(key.error()).with(label(headerLine)));
		}
		columnKeys.push_back(*key);
	}

	bool const text_cells = def.grid_cells == column_type::String || def.grid_cells == column_type::Command;
	std::vector<cell>      rowKeys;
	std::vector<cell>      values;
	std::vector<string_in> strings;
	std::vector<uint32_t>  lineOf;
	for (size_t r = 1; r < file.rows(); ++r)
	{
		uint32_t const line = file.line_of_row(r);
		if (file[r].size() != width)
		{
			return tl::make_unexpected(ErrorContext("row has ", file[r].size(), " cells, but the header has ", width)
				.with(label(line)));
		}
		auto key = detail::ReadCell(def.grid_rows, file.cell(0, r));
		if (!key)
		{
			return tl::make_unexpected(std::move(key.error()).with(label(line)));
		}
		rowKeys.push_back(*key);
		lineOf.push_back(line);
		for (size_t h = 1; h < width; ++h)
		{
			std::string_view const cellText = file.cell(h, r);
			// The header names the column, so an error points at the cell.
			column_def const column{std::string(file.cell(h, 0)), def.grid_cells};
			if (text_cells)
			{
				if (cellText.empty())
				{
					return tl::make_unexpected(ErrorContext("column '", column.name, "' is empty").with(label(line)));
				}
				strings.emplace_back(cellText);
				continue;
			}
			auto value = detail::ReadCell(column, cellText);
			if (!value)
			{
				return tl::make_unexpected(std::move(value.error()).with(label(line)));
			}
			values.push_back(*value);
		}
	}

	grid_refusal refusal;
	auto loaded = store.LoadGrid(load_grid{command.table, command.owner, rowKeys, columnKeys, values, strings, &refusal});
	if (!loaded)
	{
		ErrorContext error = std::move(loaded.error());
		if (refusal.row < lineOf.size())
		{
			return tl::make_unexpected(std::move(error).with("same key as line ", lineOf[refusal.earlier_row])
				.with(label(lineOf[refusal.row])));
		}
		if (refusal.column < columnKeys.size())
		{
			return tl::make_unexpected(std::move(error).with("header '", file.cell(refusal.column + 1, 0),
				"' repeats header ", refusal.earlier_column + 2).with(label(headerLine)));
		}
		return tl::make_unexpected(std::move(error).with(std::string(command.label)));
	}
	return {};
}

}
```

Add to `Spehleon/cmake/sources_tabular.cmake` after `pivot_csv.cpp`:

```cmake
	${SPEHLEON_ROOT}/Tabular/src/grid_csv.cpp
```

- [ ] **Step 5: Build and run** `$FILTER`. Expected: all pass, 4 `GridCsv` and 1 new `SchemaYaml` test included. (`GridCsv.*` is matched by `Grid*`.)

- [ ] **Step 6: Mutation check**

In `LoadGridCsv`, delete the `if (cellText.empty())` block for text cells. Expected: `GridCsv.BlankCellsAreErrorsNamingLineAndHeader` FAILS. Restore, `touch src/grid_csv.cpp`, rebuild, rerun: passes.

- [ ] **Step 7: Docs**

`docs/definition.md`:
- **Flag matching:** replace rule 2 and the ties paragraph with the order as built: a row whose flags are all in the input first, then the larger `R & F` (higher bits outrank all lower bits together, so bit order is priority), then the larger `R`; ties to the lower pivot column (header order), then row order; a row with `R != 0` and `R & F == 0` never wins; the `R == 0` row is tried last and must exist in every key run a load adds to. Cite animation's `LookupBest` as the same order.
- **A row:** "Conditions" bullet: the kinds are a band (`a..b`, half-open, compared in float as animation's phase bands), a flags ranking column (ranks, never fails a row) and at most one script condition; **all of a row's conditions must hold** (owner, 2026-10-16). Edges stay to be built.
- **Pivot:** add that a key × key pivot is a **grid**, stored densely, every cell filled (a blank is an error); a flags-of-key pivot and an output pivot are ordinary rows sorted by key, which the CSV front end expands.
- **Order of work** item 3: add "**Picking and grids** (built: `tabular/pick.h`, `tabular/grid.h`): inputs, bands and a ranking column; `PickBest`, `PickAll`, `PickOr`; pivoted CSV; grid tables and grid CSV."
- **Undecided:** delete the first bullet (condition kinds and whether all must hold): it is ruled.

`docs/feature-checklist.md`: tick `[x]` Flags match; Ranked fall-through; At most one script condition per row (drop its `(?)`); Pivoted flag outputs OR together; Pick "all"; Pivoted tables; Flag domain from pivot headers. Native condition kinds becomes `[~]` with "edge not built". In Consequences, tick "No tie is ever ambiguous" and make "A missing default row is refused" `[~]` with "checked for key runs a load adds rows to; a run left without one by removing rows picks nothing". Add an item under section 2: `- [x] Grid tables: key × key, dense, every cell filled; grid CSV [owner 2026-10-16]`.

The spec: set **Status** to "landed 2026-10-16 (approved by the owner the same day)"; add an **As built** section listing the six spec amendments from this plan's Global Constraints. Move the spec to `docs/superpowers/landed/specs/` and this plan to `docs/superpowers/landed/plans/` (`git mv`), and fix the plan's **Spec:** line to the new path.

- [ ] **Step 8: Full suite**

Run: `cd $WT && env -u GTEST_FILTER Build/Engine --gtest > $WT/../tabular-picking-suite.log 2>&1; grep -E "^\[  (FAILED|PASSED|SKIPPED) |^\[==========\]" $WT/../tabular-picking-suite.log | tail`
Expected: 0 failed. A red outside Tabular: check `known-master-reds-index.md` in the memory vault, rerun it alone (`GTEST_FILTER='<Suite.Test>'`) three times; report it with its attribution, do not fix it here.

- [ ] **Step 9: Commit**

```bash
cd $WT/Spehleon/Tabular && git add -A include src test docs && git commit -m "Grid CSV and YAML; definition and checklist: picking and grids are built"
cd $WT/Spehleon && git add Tabular cmake/sources_tabular.cmake && git commit -m "Bump Tabular: grid CSV; picking and grids landed"
cd $WT && git add Spehleon && git commit -m "Bump Spehleon: Tabular picking and grids"
```
