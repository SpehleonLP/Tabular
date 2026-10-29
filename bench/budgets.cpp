#include "budgets.h"

#include "tabular/instance.h"
#include "tabular/machine.h"
#include "tabular/pick.h"
#include "tabular/relation.h"
#include "tabular/rows_csv.h"
#include "tabular/document.h"
#include "Support/strings.h"
#include "cityhash/src/city.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>
#include <utility>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

namespace tabular::bench
{

namespace
{

// The budgets, in the unit each measurement is taken in. Each is the number
// measured with scripts/test-tabular-budgets.sh (definition.md, Budgets, says
// on what machine and at what sizes) times the margin written there; a change
// here is a change to that table.
struct budget_row
{
	std::string_view name;
	double           budget;
	budget_unit      unit;
};

constexpr budget_row kBudgets[] = {
	{"build a definition: grazer",         1200, budget_unit::Microseconds},
	{"build a definition: per row",        1.8, budget_unit::Microseconds},
	{"build a definition: brain machine",  260000, budget_unit::Bytes},
	{"bind: grazer",                       1.8, budget_unit::Microseconds},
	{"bind: brain",                        150, budget_unit::Microseconds},
	{"bind: grazer binding",               1300, budget_unit::Bytes},
	{"bind: brain binding",                47000, budget_unit::Bytes},
	{"create an instance: grazer",         0.9, budget_unit::Microseconds},
	{"create an instance: brain",          0.9, budget_unit::Microseconds},
	{"create an instance: grazer instance", 820, budget_unit::Bytes},
	{"create an instance: brain instance", 1400, budget_unit::Bytes},
	{"step: per row evaluated",            250, budget_unit::Nanoseconds},
	{"step: grazer",                       550, budget_unit::Nanoseconds},
	{"relation insert: mutable",           25, budget_unit::Microseconds},
	{"relation retract: mutable",          420, budget_unit::Microseconds},
	{"relation query: mutable",            280, budget_unit::Nanoseconds},
	{"relation insert: best",              110, budget_unit::Microseconds},
	{"relation retract: best",             45, budget_unit::Microseconds},
	{"relation query: best",               380, budget_unit::Nanoseconds},
};

using steady = std::chrono::steady_clock;

double Elapsed(steady::time_point from)
{
	return std::chrono::duration<double, std::nano>(steady::now() - from).count();
}

// The fastest of repeats runs of work, in nanoseconds: the machine's noise
// only ever adds time.
template<typename Work>
double Fastest(size_t repeats, Work const& work)
{
	double best = std::numeric_limits<double>::infinity();
	for (size_t r = 0; r < repeats; ++r)
	{
		steady::time_point const start = steady::now();
		work();
		best = std::min(best, Elapsed(start));
	}
	return best;
}

// Bytes the heap holds for live allocations, malloc's own overhead included,
// so a difference is what a structure really costs here (Windows' heap
// differs in its overhead, not in what is asked for).
std::optional<size_t> HeapInUse()
{
#if defined(__GLIBC__)
	return mallinfo2().uordblks;
#else
	return std::nullopt;
#endif
}

int64_t Hash(std::string_view text)
{
	return static_cast<int64_t>(CityHash32(text.data(), text.size()));
}

// ---- The host -----------------------------------------------------------------------

struct bench_host
{
	double  energy   = 0.5;
	int64_t near     = 0;
	bool    cornered = false;
	int64_t event    = 0;   // filled by the host for one step: the event it dispatches
};

bench_host const& Host(void const* self)
{
	return *static_cast<bench_host const*>(self);
}

cell ReadEnergy(void*, void const* self, ErrorContext*)
{
	return cell::Real(Host(self).energy);
}

cell ReadNear(void*, void const* self, ErrorContext*)
{
	return cell::Integer(Host(self).near);
}

cell ReadEvent(void*, void const* self, ErrorContext*)
{
	return cell::Integer(Host(self).event);
}

bool Cornered(void*, void const* self, ErrorContext*)
{
	return Host(self).cornered;
}

bool Panicky(void*, void const* self, ErrorContext*)
{
	return !Host(self).cornered;
}

tl::expected<closure, ErrorContext> Conditions(std::string_view text)
{
	if (text == "cornered")
	{
		return closure{&Cornered, nullptr, nullptr};
	}
	if (text == "panicky")
	{
		return closure{&Panicky, nullptr, nullptr};
	}
	return tl::make_unexpected(ErrorContext("no condition '", text, "'"));
}

host_binding BenchHost()
{
	host_binding host;
	host.resolve = [](std::string_view name) -> tl::expected<input_getter, ErrorContext>
	{
		if (name == "energy")
		{
			return input_getter{&ReadEnergy, nullptr, nullptr};
		}
		if (name == "near" || name == "food_near")
		{
			return input_getter{&ReadNear, nullptr, nullptr};
		}
		if (name == "message")
		{
			return input_getter{&ReadEvent, nullptr, nullptr};
		}
		return tl::make_unexpected(ErrorContext("no wire named '", name, "'"));
	};
	return host;
}

// ---- The workloads ------------------------------------------------------------------

// The spec's grazer (§4) as the step tests run it: a fall-through key, a ranked
// flags input, a band and two script conditions, and an entry clause that
// emits.
constexpr std::string_view kGrazer = R"(
[table grazer_brain
  (input energy double 0..1)
  (input food_near flags)
  (key mode hash (domain rows))
  (key message hash fallthrough)
  (column energy band (input energy))
  (column near flags (input food_near))
  (column when condition)
  (column next hash)
  (order mode message)]
[table grazer_enter
  (key mode hash (domain rows))
  (order mode)
  (suffix command)]
[machine grazer
  (state mode hash (members idle walk eat panic) (initial idle))
  (input energy double 0..1)
  (input food_near flags)
  (input message hash)]
list,from,where,given,set,emit,overrides
entry,grazer_enter,mode=state.mode,,,command,
step,grazer_brain,mode=state.mode message=input.message,energy=input.energy food_near=input.food_near,mode=next,,
)";

constexpr std::string_view kGrazerBrainRows = R"(mode,  message, energy,   near, when,     next
idle,      ,    0..0.3,   1,    ,         eat
idle,      ,    0..1,     2,    cornered, panic
idle,      ,    0..1,     0,    ,         walk
walk,      ,    0.5..1,   2,    ,         eat
walk,      ,    0..0.5,   3,    ,         idle
walk,      ,    0.2..0.8, 1,    panicky,  panic
walk,      ,    0..1,     0,    ,         idle
eat,       ,    0..0.5,   0,    cornered, panic
eat,       ,    0..1,     0,    ,         idle
eat,       ,    0..1,     6,    ,         walk
eat,       ,    0..1,     9,    ,         panic
idle,  scared,  0..1,     0,    ,         panic
walk,  scared,  0..1,     0,    ,         panic
eat,   scared,  0..1,     0,    ,         panic
panic,     ,    0..1,     0,    ,         panic
panic, calm,    0..1,     0,    ,         idle
panic, scared,  0..1,     0,    ,         panic
idle,  calm,    0..1,     0,    ,         idle
walk,  calm,    0..0.5,   0,    panicky,  panic
walk,  calm,    0..1,     0,    ,         walk
eat,   calm,    0..1,     0,    ,         eat
)";

constexpr std::string_view kGrazerEnterRows = R"csv(mode,  command
eat,   (anim chew), "(message food ""eat"")"
panic, (anim run), "(message this ""calm"" :delay random(0.4, 1.0))"
idle,  (anim stand)
walk,  (anim walk)
)csv";

std::string Decimal(double value)
{
	char text[32];
	std::snprintf(text, sizeof(text), "%g", value);
	return text;
}

// Band k of n equal bands over 0..1, as a CSV cell.
std::string Band(size_t k, size_t n)
{
	return Decimal(double(k) / double(n)) + ".." + Decimal(double(k + 1) / double(n));
}

std::string Member(size_t m)
{
	return "m" + std::to_string(m);
}

// The budget brain: a hash state of modes members and clauses step clauses,
// each over its own best table keyed on the state and writing it, so the
// clauses chain through next. Every table has 10 rows per mode with no
// script: an even table ten energy bands, an odd one a ranked flags column
// with two groups (0 and 1) of five bands. Every point lies in one row of its
// cell, so each clause tries exactly one row: a row evaluated is a clause run.
std::string BrainSource(budget_sizes const& sizes)
{
	std::string source;
	for (size_t t = 0; t < sizes.clauses; ++t)
	{
		bool const ranked = t % 2 == 1;
		source += "[table t" + std::to_string(t) + "\n  (input energy double 0..1)\n";
		if (ranked)
		{
			source += "  (input near flags)\n";
		}
		source += "  (key mode hash (domain rows))\n  (column energy band (input energy))\n";
		if (ranked)
		{
			source += "  (column near flags (input near))\n";
		}
		source += "  (column next hash)\n  (order mode)]\n";
	}
	source += "[machine brain\n  (state mode hash (members";
	for (size_t m = 0; m < sizes.modes; ++m)
	{
		source += " " + Member(m);
	}
	source += ") (initial m0))\n  (input energy double 0..1)\n  (input near flags)]\n"
		"list,from,where,given,set,emit,overrides\n";
	for (size_t t = 0; t < sizes.clauses; ++t)
	{
		bool const ranked = t % 2 == 1;
		source += "step,t" + std::to_string(t) + ",mode=state.mode,energy=input.energy"
			+ std::string(ranked ? " near=input.near" : "") + ",mode=next,,\n";
	}
	return source;
}

std::string BrainRows(budget_sizes const& sizes, size_t t)
{
	bool const  ranked = t % 2 == 1;
	std::string rows   = ranked ? "mode,energy,near,next\n" : "mode,energy,next\n";
	for (size_t m = 0; m < sizes.modes; ++m)
	{
		for (size_t b = 0; b < 10; ++b)
		{
			std::string const next = Member((m * 7 + b + t) % sizes.modes);
			if (ranked)
			{
				rows += Member(m) + "," + Band(b % 5, 5) + "," + std::to_string(b / 5) + "," + next + "\n";
			}
			else
			{
				rows += Member(m) + "," + Band(b, 10) + "," + next + "\n";
			}
		}
	}
	return rows;
}

// A document's tables in a store, its rows loaded, its first machine
// prepared: what "build a definition from text" pays.
struct Workload
{
	schema_document            document;
	relation_store             store;
	counted_ptr<Machine const> machine;
	size_t                     rows = 0;

	explicit Workload(schema_document&& parsed) : document(std::move(parsed)), store(std::move(document.tables)) {}
};

tl::expected<std::unique_ptr<Workload>, ErrorContext> Build(std::string_view source, std::span<std::string const> rows)
{
	auto parsed = ReadDocument(source, "budget.tabular");
	if (!parsed)
	{
		return tl::make_unexpected(std::move(parsed.error()));
	}
	auto built = std::make_unique<Workload>(std::move(*parsed));
	for (uint32_t t = 0; t < rows.size(); ++t)
	{
		csv_rows load;
		load.table     = t;
		load.owner     = 1;
		load.text      = rows[t];
		load.label     = "budget.csv";
		load.condition = &Conditions;
		if (auto loaded = LoadCsv(built->store, load); !loaded)
		{
			return tl::make_unexpected(std::move(loaded.error()).with("table ", t));
		}
		built->rows += size_t(std::count(rows[t].begin(), rows[t].end(), '\n')) - 1;
	}
	if (built->document.machines().empty())
	{
		return tl::make_unexpected(ErrorContext("the budget document has no machine"));
	}
	auto prepared = Prepare(built->document.machines()[0], built->store);
	if (!prepared)
	{
		return tl::make_unexpected(std::move(prepared.error()));
	}
	built->machine = std::move(*prepared);
	return built;
}

struct results
{
	std::vector<measurement> out;

	tl::expected<void, ErrorContext> Add(std::string_view name, double measured, std::string&& how)
	{
		for (budget_row const& row : kBudgets)
		{
			if (row.name == name)
			{
				out.push_back(measurement{std::string(name), measured, row.budget, row.unit, std::move(how)});
				return {};
			}
		}
		return tl::make_unexpected(ErrorContext("no budget named '", name, "'"));
	}
};

#define TABULAR_TRY(expr)                                                 \
	do                                                                    \
	{                                                                     \
		if (auto tried = (expr); !tried)                                  \
		{                                                                 \
			return tl::make_unexpected(std::move(tried.error()));         \
		}                                                                 \
	} while (false)

// Heap bytes make(count) leaves allocated, per object; it keeps its count
// objects alive until measured. mallinfo counts a chunk parked in glibc's
// per-thread cache (up to 7 per size class) as in use, so the first few
// objects can reuse memory freed just before and look free: measuring many
// makes that error a few chunks over count objects.
template<typename Make>
tl::expected<double, ErrorContext> BytesEach(size_t count, Make const& make)
{
	std::optional<size_t> const before = HeapInUse();
	auto made = make(count);
	std::optional<size_t> const after = HeapInUse();
	if (!before || !after)
	{
		return tl::make_unexpected(ErrorContext("this platform has no heap counter, so memory is not measured"));
	}
	return double(*after > *before ? *after - *before : 0) / double(count);
}

tl::expected<void, ErrorContext> MeasureMachine(results& r, budget_sizes const& sizes, std::string_view label,
	Workload const& w, size_t clausesPerStep, bool grazer)
{
	std::string const bindName   = cc("bind: ", label);
	std::string const createName = cc("create an instance: ", label);

	// Bind: per definition x object type.
	size_t const binds = 200;
	double const bindNs = Fastest(sizes.repeats, [&]()
	{
		for (size_t b = 0; b < binds; ++b)
		{
			auto bound = Bind(w.machine, BenchHost());
			if (!bound)
			{
				return;
			}
		}
	});
	auto probe = Bind(w.machine, BenchHost());
	if (!probe)
	{
		return tl::make_unexpected(std::move(probe.error()));
	}
	TABULAR_TRY(r.Add(bindName, bindNs / binds / 1000.0, cc("one Bind of the ", label, ", the fastest of ",
		sizes.repeats, " runs of ", binds)));
	std::vector<counted_ptr<Binding const>> keptBindings;
	keptBindings.reserve(100);
	auto bindingBytes = BytesEach(keptBindings.capacity(), [&](size_t count)
	{
		for (size_t b = 0; b < count; ++b)
		{
			auto bound = Bind(w.machine, BenchHost());
			if (bound)
			{
				keptBindings.push_back(std::move(*bound));
			}
		}
		return 0;
	});
	keptBindings.clear();
	if (!bindingBytes)
	{
		return tl::make_unexpected(std::move(bindingBytes.error()));
	}
	TABULAR_TRY(r.Add(cc("bind: ", label, " binding"), *bindingBytes,
		cc("heap held by one Binding of the ", label, " (its getters, sites, report and rank arrays), malloc overhead included, over 100")));

	Binding const& binding = **probe;
	std::vector<bench_host> hosts(sizes.instances);
	for (size_t i = 0; i < hosts.size(); ++i)
	{
		hosts[i].energy = double(i % 97) / 97.0;
		hosts[i].near   = int64_t(i % 3);
	}

	// Create: per object.
	std::vector<instance> instances;
	instances.reserve(sizes.instances);
	double const createNs = Fastest(sizes.repeats, [&]()
	{
		instances.clear();
		for (size_t i = 0; i < sizes.instances; ++i)
		{
			instances.push_back(CreateInstance(binding, i, i, &hosts[i]));
		}
	});
	TABULAR_TRY(r.Add(createName, createNs / double(sizes.instances) / 1000.0,
		cc("one CreateInstance of the ", label, " (create and entry lists run), the fastest of ", sizes.repeats,
			" runs of ", sizes.instances)));
	instances.clear();
	auto instanceBytes = BytesEach(sizes.instances, [&](size_t count)
	{
		for (size_t i = 0; i < count; ++i)
		{
			instances.push_back(CreateInstance(binding, i, i, &hosts[i]));
		}
		return 0;
	});
	if (!instanceBytes)
	{
		return tl::make_unexpected(std::move(instanceBytes.error()));
	}
	TABULAR_TRY(r.Add(cc("create an instance: ", label, " instance"), *instanceBytes + double(sizeof(instance)),
		cc("sizeof(instance) (", sizeof(instance), ") plus the heap one instance of the ", label,
			" holds, malloc overhead included, over ", sizes.instances)));

	// Step: per instance per tick. The inputs vary per instance and tick from
	// a table, so the timing is the step's and not a generator's.
	std::array<double, 1024> energies{};
	for (size_t k = 0; k < energies.size(); ++k)
	{
		energies[k] = double((k * 389) % 1024) / 1024.0;
	}
	int64_t const flags[]    = {0, 1, 2, 3, 6, 9};
	int64_t const messages[] = {0, Hash("scared"), Hash("calm"), Hash("sneeze"), 0, 0};
	size_t        ticks      = 0;
	double const  stepNs = Fastest(sizes.repeats, [&]()
	{
		for (size_t s = 0; s < sizes.steps; ++s, ++ticks)
		{
			for (size_t i = 0; i < instances.size(); ++i)
			{
				bench_host& host = hosts[i];
				host.energy   = energies[(i + ticks * 7) & 1023];
				host.near     = flags[(i + ticks) % std::size(flags)];
				host.cornered = ((i + ticks) & 4) != 0;
				host.event    = grazer ? messages[(i * 3 + ticks) % std::size(messages)] : 0;
				Step(instances[i], &host, ticks);
			}
		}
	});
	// A step whose clauses did not all answer measured holds, not picks.
	for (instance const& subject : instances)
	{
		for (size_t c = 0; c < clausesPerStep; ++c)
		{
			if (subject.Answer(trigger::Step, c).outcome != clause_outcome::Answered)
			{
				return tl::make_unexpected(ErrorContext("a ", label, " step clause did not answer, so the step timing is not a pick's"));
			}
		}
	}
	double const steps = double(sizes.steps) * double(instances.size());
	if (grazer)
	{
		TABULAR_TRY(r.Add("step: grazer", stepNs / steps,
			cc("one Step of the spec grazer (one step clause, its entry clause on a change; events, flags, bands and scripts varied), "
				"over ", instances.size(), " instances x ", sizes.steps, " ticks, the fastest of ", sizes.repeats)));
	}
	else
	{
		TABULAR_TRY(r.Add("step: per row evaluated", stepNs / (steps * double(clausesPerStep)),
			cc("one clause of the budget brain (", sizes.clauses, " chained best clauses over ", sizes.modes * 10,
				"-row tables; each tries exactly one row), over ", instances.size(), " instances x ", sizes.steps,
				" ticks, the fastest of ", sizes.repeats)));
	}
	return {};
}

tl::expected<void, ErrorContext> MeasureBuild(results& r, budget_sizes const& sizes, std::unique_ptr<Workload>& grazer,
	std::unique_ptr<Workload>& brain)
{
	std::string const grazerRows[] = {std::string(kGrazerBrainRows), std::string(kGrazerEnterRows)};
	std::string const brainSource  = BrainSource(sizes);
	std::vector<std::string> brainRows;
	for (size_t t = 0; t < sizes.clauses; ++t)
	{
		brainRows.push_back(BrainRows(sizes, t));
	}

	// The fastest build of repeats; the previous repeat's workload is freed
	// before the clock starts, so a timing is a build and not a teardown too.
	auto timeBuild = [&](std::unique_ptr<Workload>& kept, std::string_view source,
						 std::span<std::string const> rows) -> tl::expected<double, ErrorContext>
	{
		double best = std::numeric_limits<double>::infinity();
		for (size_t rep = 0; rep < sizes.repeats; ++rep)
		{
			kept.reset();
			steady::time_point const start = steady::now();
			auto built = Build(source, rows);
			best = std::min(best, Elapsed(start));
			if (!built)
			{
				return tl::make_unexpected(std::move(built.error()));
			}
			kept = std::move(*built);
		}
		return best;
	};
	auto grazerTimed = timeBuild(grazer, kGrazer, grazerRows);
	if (!grazerTimed)
	{
		return tl::make_unexpected(std::move(grazerTimed.error()));
	}
	auto brainTimed = timeBuild(brain, brainSource, brainRows);
	if (!brainTimed)
	{
		return tl::make_unexpected(std::move(brainTimed.error()));
	}
	double const grazerNs = *grazerTimed;
	double const brainNs  = *brainTimed;
	TABULAR_TRY(r.Add("build a definition: grazer", grazerNs / 1000.0,
		cc("the spec grazer from text: file, ", grazer->rows, " CSV rows, Prepare; the fastest of ", sizes.repeats)));
	TABULAR_TRY(r.Add("build a definition: per row", brainNs / double(brain->rows) / 1000.0,
		cc("the budget brain from text (file, ", brain->rows, " CSV rows in ", sizes.clauses,
			" tables, Prepare) per row; the fastest of ", sizes.repeats)));

	std::vector<counted_ptr<Machine const>> keptMachines;
	keptMachines.reserve(10);
	auto machineBytes = BytesEach(keptMachines.capacity(), [&](size_t count)
	{
		for (size_t m = 0; m < count; ++m)
		{
			auto prepared = Prepare(brain->document.machines()[0], brain->store);
			if (prepared)
			{
				keptMachines.push_back(std::move(*prepared));
			}
		}
		return 0;
	});
	keptMachines.clear();
	if (!machineBytes)
	{
		return tl::make_unexpected(std::move(machineBytes.error()));
	}
	TABULAR_TRY(r.Add("build a definition: brain machine", *machineBytes,
		cc("heap held by the budget brain's prepared Machine (its layout over ", brain->rows,
			" rows; the versions it holds are the store's), malloc overhead included, over 10")));
	return {};
}

// ---- Relations ----------------------------------------------------------------------

// Built here, not read: both tables take rows at run time, and every table a
// file declares is read-only.
tl::expected<relation_store, ErrorContext> SpawnPointsStore()
{
	static add_column const columns[] = {{"section", column_type::Int, true}, {"x", column_type::Double}};
	static uint32_t const   bySection[] = {0};
	static order_by const   order{bySection, false};
	create_table const      tables[] = {{"spawn_points", columns, &order}};
	auto s = BuildSchema(tables);
	if (!s)
	{
		return tl::make_unexpected(std::move(s.error()));
	}
	return relation_store(std::move(*s));
}

tl::expected<relation_store, ErrorContext> DisplayOwnersStore()
{
	static add_input const  inputs[]  = {{"live", column_type::Flags}};
	static add_column const columns[] = {
		{"display", column_type::Hash, true, UINT32_MAX, key_domain::Rows()},
		{"needs", column_type::Flags, false, 0},
		{"object", column_type::Hash}};
	static uint32_t const   byDisplay[] = {0};
	static order_by const   order{byDisplay, false};
	create_table const      tables[] = {{"owner", columns, &order, false, inputs, pick_policy::Best}};
	auto s = BuildSchema(tables);
	if (!s)
	{
		return tl::make_unexpected(std::move(s.error()));
	}
	return relation_store(std::move(*s));
}

tl::expected<void, ErrorContext> MeasureRelations(results& r, budget_sizes const& sizes)
{
	// A plain mutable table, the first version's spawn_points: 100 owners'
	// rows over 500 sections.
	auto spawn = SpawnPointsStore();
	if (!spawn)
	{
		return tl::make_unexpected(std::move(spawn.error()));
	}
	size_t const sections = 500;
	auto insertSpawn = [&](uint64_t owner, size_t k) -> tl::expected<void, ErrorContext>
	{
		cell const values[] = {cell::Integer(int64_t(k % sections)), cell::Real(double(k))};
		return spawn->Insert(insert_row{0, owner, values});
	};
	for (size_t k = 0; k < sizes.mutableRows; ++k)
	{
		TABULAR_TRY(insertSpawn(1 + k % 100, k));
	}
	size_t const inserts = 1000;
	size_t const perOwner = 10;
	// Each repeat inserts, then retracts what it inserted, which puts the table
	// back for the next; the two are timed apart, so neither number holds the
	// other's work.
	double insertNs  = std::numeric_limits<double>::infinity();
	double retractNs = std::numeric_limits<double>::infinity();
	for (size_t rep = 0; rep < sizes.repeats; ++rep)
	{
		steady::time_point const inserting = steady::now();
		for (size_t k = 0; k < inserts; ++k)
		{
			TABULAR_TRY(insertSpawn(5000 + k / perOwner, k));
		}
		insertNs = std::min(insertNs, Elapsed(inserting));
		steady::time_point const retracting = steady::now();
		for (size_t o = 0; o < inserts / perOwner; ++o)
		{
			spawn->Retract(retract_owner{5000 + o});
		}
		retractNs = std::min(retractNs, Elapsed(retracting));
	}
	TABULAR_TRY(r.Add("relation insert: mutable", insertNs / double(inserts) / 1000.0,
		cc("one Insert into a plain mutable table of ", sizes.mutableRows, " rows ordered by an int key; ", inserts,
			" inserts, the fastest of ", sizes.repeats)));
	TABULAR_TRY(r.Add("relation retract: mutable", retractNs / double(inserts / perOwner) / 1000.0,
		cc("one Retract of an owner's ", perOwner, " rows from that table; the fastest of ", sizes.repeats)));
	size_t found = 0;
	size_t const queries = 10000;
	double const queryNs = Fastest(sizes.repeats, [&]()
	{
		for (size_t q = 0; q < queries; ++q)
		{
			cell const key[] = {cell::Integer(int64_t(q % sections))};
			auto range = spawn->FindOrdered(0, key);
			found += range ? range->last - range->first : 0;
		}
	});
	if (found == 0)
	{
		return tl::make_unexpected(ErrorContext("the spawn point queries found nothing"));
	}
	TABULAR_TRY(r.Add("relation query: mutable", queryNs / double(queries),
		cc("one FindOrdered of a key's rows in that table; ", queries, " queries, the fastest of ", sizes.repeats)));

	// A mutable best table, the display owners: per display an engine default
	// row and three owners' ranked rows.
	auto display = DisplayOwnersStore();
	if (!display)
	{
		return tl::make_unexpected(std::move(display.error()));
	}
	auto addOwner = [&](uint64_t owner, size_t d, int64_t needs) -> tl::expected<void, ErrorContext>
	{
		cell const values[] = {cell::Integer(Hash(Member(d))), cell::Integer(needs), cell::Integer(int64_t(owner))};
		return display->Insert(insert_row{0, owner, values});
	};
	for (size_t d = 0; d < sizes.displays; ++d)
	{
		TABULAR_TRY(addOwner(1, d, 0));
		TABULAR_TRY(addOwner(2, d, 1));
		TABULAR_TRY(addOwner(3, d, 2));
		TABULAR_TRY(addOwner(4, d, 4));
	}
	double bestInsertNs  = std::numeric_limits<double>::infinity();
	double bestRetractNs = std::numeric_limits<double>::infinity();
	for (size_t rep = 0; rep < sizes.repeats; ++rep)
	{
		steady::time_point const inserting = steady::now();
		for (size_t d = 0; d < sizes.displays; ++d)
		{
			TABULAR_TRY(addOwner(10, d, 8));
		}
		bestInsertNs = std::min(bestInsertNs, Elapsed(inserting));
		steady::time_point const retracting = steady::now();
		display->Retract(retract_owner{10});
		bestRetractNs = std::min(bestRetractNs, Elapsed(retracting));
	}
	size_t const rows = sizes.displays * 4;
	TABULAR_TRY(r.Add("relation insert: best", bestInsertNs / double(sizes.displays) / 1000.0,
		cc("one Insert into a mutable best table of ", rows, " rows over ", sizes.displays,
			" keys (a new version, proved total), the fastest of ", sizes.repeats)));
	TABULAR_TRY(r.Add("relation retract: best", bestRetractNs / 1000.0,
		cc("one Retract of an owner's ", sizes.displays, " rows from that table, the fastest of ", sizes.repeats)));
	size_t picked = 0;
	std::optional<ErrorContext> pickError;
	double const pickNs = Fastest(sizes.repeats, [&]()
	{
		picked = 0;
		for (size_t q = 0; q < queries; ++q)
		{
			cell const keys[]   = {cell::Integer(Hash(Member(q % sizes.displays)))};
			cell const inputs[] = {cell::Integer(int64_t(q % 16))};
			auto best = PickBest(*display, 0, pick_query{keys, inputs});
			if (!best && !pickError)
			{
				pickError = std::move(best.error());
			}
			picked += best && *best ? 1 : 0;
		}
	});
	if (pickError)
	{
		return tl::make_unexpected(std::move(*pickError).with("a display owner pick"));
	}
	if (picked != queries)
	{
		return tl::make_unexpected(ErrorContext("a display owner pick found no row: ", picked, " of ", queries));
	}
	// Hashing the key's name is part of the loop; it is the caller's cost, and
	// small beside the pick, so the number stays an upper bound.
	TABULAR_TRY(r.Add("relation query: best", pickNs / double(queries),
		cc("one PickBest of that table with a ranked flags input (the key's hash computed in the loop too); ", queries,
			" picks, the fastest of ", sizes.repeats)));
	return {};
}

}

std::string_view UnitName(budget_unit unit)
{
	switch (unit)
	{
	case budget_unit::Nanoseconds:
		return "ns";
	case budget_unit::Microseconds:
		return "us";
	case budget_unit::Bytes:
		return "bytes";
	}
	return "?";
}

tl::expected<std::vector<measurement>, ErrorContext> RunBudgets(budget_sizes const& sizes)
{
	results r;
	std::unique_ptr<Workload> grazer;
	std::unique_ptr<Workload> brain;
	TABULAR_TRY(MeasureBuild(r, sizes, grazer, brain));
	TABULAR_TRY(MeasureMachine(r, sizes, "grazer", *grazer, 1, true));
	TABULAR_TRY(MeasureMachine(r, sizes, "brain", *brain, sizes.clauses, false));
	TABULAR_TRY(MeasureRelations(r, sizes));
	if (r.out.size() != std::size(kBudgets))
	{
		return tl::make_unexpected(ErrorContext("measured ", r.out.size(), " of ", std::size(kBudgets), " budgets"));
	}
	return std::move(r.out);
}

std::vector<std::string> Exceeded(std::span<measurement const> measured, double scale)
{
	std::vector<std::string> over;
	for (measurement const& m : measured)
	{
		if (!std::isfinite(m.measured) || m.measured <= 0)
		{
			over.push_back(cc(m.name, ": measured nothing (", Decimal(m.measured), ")"));
		}
		else if (m.measured > m.budget * scale)
		{
			over.push_back(cc(m.name, ": ", Decimal(m.measured), " ", UnitName(m.unit), " is over its budget of ",
				Decimal(m.budget * scale), " ", UnitName(m.unit)));
		}
	}
	return over;
}

#undef TABULAR_TRY

}
