#include "document_internal.h"
#include "document_header.h"
#include "csv_cells.h"

#include <deque>

namespace tabular::detail
{

namespace
{

enum class option : uint8_t
{
	Key, Column, Input, Pick, Order, Suffix, Pivot, Rows, Columns, Cells, Count
};

struct option_name
{
	std::string_view name;
	option           which;
	bool             grid;       // taken by a [grid] rather than a [table]
	bool             repeats;    // may appear more than once
};

constexpr option_name kOptions[] = {
	{"key",     option::Key,     false, true},
	{"column",  option::Column,  false, true},
	{"input",   option::Input,   false, true},
	{"pick",    option::Pick,    false, false},
	{"order",   option::Order,   false, false},
	{"suffix",  option::Suffix,  false, false},
	{"pivot",   option::Pivot,   false, false},
	{"rows",    option::Rows,    true,  false},
	{"columns", option::Columns, true,  false},
	{"cells",   option::Cells,   true,  false},
};

// A name some other option refers to, resolved once every option is read, since
// options come in any order.
struct reference
{
	std::string_view name;
	uint32_t         line = 0;
	bool             set  = false;
};

struct table_build
{
	std::string_view              name;
	std::vector<add_column>       columns;
	std::vector<reference>        columnInputs;   // per column
	std::vector<add_input>        inputs;
	std::deque<std::vector<cell>> members;
	std::vector<uint32_t>         orderColumns;
	order_by                      order;
	bool                          ordered = false;
	std::vector<reference>        orderNames;
	pick_policy                   pick = pick_policy::Best;
	reference                     output;
	reference                     weight;
	uint32_t                      outputColumn = UINT32_MAX;
	uint32_t                      weightColumn = UINT32_MAX;
	std::string_view              suffix;
	pivot_by                      pivot;
	bool                          pivoted = false;
	std::array<reference, 2>      pivotNames;
	grid_shape                    grid;
};

// Whether a body has any row. A table's first record is its header, so it needs
// a second; a grid's header is part of the grid, so one record is rows. A
// record is skipped as csv_text skips it: blank, nothing but separators (a
// spreadsheet's empty row), or a first unquoted character of '#'.
bool HasRows(std::string_view body, bool headerIsData)
{
	size_t records = 0;
	size_t at      = 0;
	while (at < body.size())
	{
		size_t end = body.find('\n', at);
		if (end == std::string_view::npos)
		{
			end = body.size();
		}
		std::string_view const line = body.substr(at, end - at);
		size_t const first = line.find_first_not_of(" \t\r");
		size_t const data  = line.find_first_not_of(", \t\r");
		if (first != std::string_view::npos && line[first] != '#' && data != std::string_view::npos)
		{
			++records;
		}
		at = end + 1;
	}
	return records > (headerIsData ? 0u : 1u);
}

// `(key name type ...)` and its kin: the option's head word, for refusals.
std::string_view HeadOf(sexpr const& option)
{
	return option.items.empty() ? std::string_view() : option.items[0].text;
}

struct reader
{
	std::string_view label;
	DocumentStorage& storage;
	table_build&     build;

	// The domain of a key column: rows, a list of members, or another table's column.
	tl::expected<key_domain, ErrorContext> ReadDomain(sexpr const& domain, std::string_view column, column_type type)
	{
		constexpr char const* kShapes = "; a domain is (domain rows), (domain (members ..)) or (domain (from table column))";
		if (domain.items.size() < 2)
		{
			return Refuse(label, domain.line, "the domain of column '", column, "' is empty", kShapes);
		}
		sexpr const& what = domain.items[1];
		// What the domain says comes before how many words follow it, so
		// (domain rows (members a)) is refused for the rows it also names.
		if (IsAtom(what, "rows"))
		{
			if (domain.items.size() > 2)
			{
				return Refuse(label, domain.items[2].line, "the domain of column '", column,
					"' is rows and has more after it; rows takes nothing", kShapes);
			}
			return key_domain::Rows();
		}
		if (domain.items.size() > 2)
		{
			sexpr const& extra = domain.items[2];
			if (extra.what != sexpr::kind::List)
			{
				return Refuse(label, extra.line, "the domain of column '", column, "' has the unknown option '", extra.text,
					"'", kShapes);
			}
			return Refuse(label, extra.line, "the domain of column '", column,
				"' names members or a table's column, one of the two", kShapes);
		}
		if (what.what != sexpr::kind::List || what.items.empty())
		{
			return Refuse(label, domain.line, "column '", column, "' has the unknown domain '", what.text, "'", kShapes);
		}
		if (IsAtom(what.items[0], "from"))
		{
			if (what.items.size() != 3)
			{
				return Refuse(label, what.line, "the domain of column '", column,
					"' names a table and its column, both", kShapes);
			}
			auto table = NameOf(what.items[1], storage, label, "a domain's table");
			if (!table)
			{
				return tl::make_unexpected(std::move(table.error()));
			}
			auto other = NameOf(what.items[2], storage, label, "a domain's column");
			if (!other)
			{
				return tl::make_unexpected(std::move(other.error()));
			}
			return key_domain{domain_kind::Column, {}, *table, *other};
		}
		if (!IsAtom(what.items[0], "members"))
		{
			return Refuse(label, what.line, "column '", column, "' has the unknown domain form '", what.items[0].text,
				"'", kShapes);
		}
		build.members.emplace_back();
		std::vector<cell>& members = build.members.back();
		hash_names         names;
		column_def const   asColumn{std::string(column), type};
		for (size_t m = 1; m < what.items.size(); ++m)
		{
			auto text = NameOf(what.items[m], storage, label, "a domain member");
			if (!text)
			{
				return tl::make_unexpected(std::move(text.error()));
			}
			// The builder refuses members on any key but a hash or an int, so
			// another type's members are passed uninterpreted and never read.
			if (type != column_type::Hash && type != column_type::Int)
			{
				members.push_back(cell::Integer(0));
				continue;
			}
			auto value = ReadCell(asColumn, *text, names, what.items[m].line);
			if (!value)
			{
				return tl::make_unexpected(std::move(value.error()).with("member ", m - 1, " of the domain of column '",
					column, "'").with(Place(label, what.items[m].line)));
			}
			members.push_back(*value);
		}
		return key_domain{domain_kind::Members, members, {}, {}};
	}

	// `(key|column name type [(domain ..)] [fallthrough] [(input t)] [edge])`
	tl::expected<void, ErrorContext> ReadColumn(sexpr const& option, bool key)
	{
		std::string_view const head = HeadOf(option);
		if (option.items.size() < 3)
		{
			return Refuse(label, option.line, "(", head, " name type) needs a name and a type");
		}
		auto name = DeclaredName(option.items[1], storage, label, "a column's name");
		if (!name)
		{
			return tl::make_unexpected(std::move(name.error()));
		}
		auto type = TypeOf(option.items[2], label);
		if (!type)
		{
			return tl::make_unexpected(std::move(type.error()));
		}
		key_domain domain;
		bool       hasDomain = false;
		bool       fall      = false;
		bool       edge      = false;
		reference  input;
		for (size_t i = 3; i < option.items.size(); ++i)
		{
			sexpr const& part = option.items[i];
			if (IsAtom(part, "fallthrough") || IsAtom(part, "edge"))
			{
				bool& flag = part.text == "edge" ? edge : fall;
				if (flag)
				{
					return Refuse(label, part.line, "column '", *name, "' repeats '", part.text, "'");
				}
				flag = true;
			}
			else if (part.what == sexpr::kind::List && !part.items.empty() && IsAtom(part.items[0], "domain"))
			{
				if (hasDomain)
				{
					return Refuse(label, part.line, "column '", *name, "' repeats 'domain'");
				}
				auto read = ReadDomain(part, *name, *type);
				if (!read)
				{
					return tl::make_unexpected(std::move(read.error()));
				}
				domain    = *read;
				hasDomain = true;
			}
			else if (part.what == sexpr::kind::List && !part.items.empty() && IsAtom(part.items[0], "input"))
			{
				if (input.set)
				{
					return Refuse(label, part.line, "column '", *name, "' repeats 'input'");
				}
				if (part.items.size() != 2)
				{
					return Refuse(label, part.line, "(input name) on column '", *name, "' names one input");
				}
				// Presence, not emptiness: (input "") is a mistake, not "no input".
				auto text = NameOf(part.items[1], storage, label, "a column's input");
				if (!text)
				{
					return tl::make_unexpected(std::move(text.error()));
				}
				input = reference{*text, part.line, true};
			}
			else
			{
				return Refuse(label, part.line, "column '", *name, "' has the unknown option '",
					part.what == sexpr::kind::List ? HeadOf(part) : part.text, "'; a column takes (domain ..), fallthrough, (input ..) and edge");
			}
		}
		// A fall-through key's domain is open, so it declares no other: the
		// declaration is the domain (the builder's FallThrough kind).
		if (fall)
		{
			if (hasDomain)
			{
				return Refuse(label, option.line, "column '", *name, "' of table '", build.name,
					"' falls through and declares a domain; a fall-through key is open, any value may come");
			}
			domain = key_domain::FallThrough();
		}
		build.columns.push_back(add_column{*name, *type, key, UINT32_MAX, domain, edge});
		build.columnInputs.push_back(input);
		return {};
	}

	tl::expected<void, ErrorContext> ReadPick(sexpr const& option)
	{
		constexpr char const* kShapes = "; a pick is best, all, none, (pick or <flags column>) or (pick weighted <double column>)";
		if (option.items.size() < 2 || option.items[1].what != sexpr::kind::Atom)
		{
			return Refuse(label, option.line, "(pick ..) names a policy", kShapes);
		}
		std::string_view const policy = option.items[1].text;
		size_t                 arity  = 2;
		if (policy == "best")
		{
			build.pick = pick_policy::Best;
		}
		else if (policy == "all")
		{
			build.pick = pick_policy::All;
		}
		else if (policy == "none")
		{
			build.pick = pick_policy::None;
		}
		else if (policy == "or")
		{
			build.pick = pick_policy::Or;
			arity      = 3;
		}
		else if (policy == "weighted")
		{
			build.pick = pick_policy::Weighted;
			arity      = 3;
		}
		else
		{
			return Refuse(label, option.line, "table '", build.name, "' picks '", policy, "'", kShapes);
		}
		if (option.items.size() != arity)
		{
			return Refuse(label, option.line, "(pick ", policy, arity == 3 ? " ..) names the column it reads" : ") takes nothing more", kShapes);
		}
		if (arity == 3)
		{
			// Presence, not emptiness: an empty name is a mistake, not "no column".
			auto column = NameOf(option.items[2], storage, label, "a pick's column");
			if (!column)
			{
				return tl::make_unexpected(std::move(column.error()));
			}
			(policy == "or" ? build.output : build.weight) = reference{*column, option.line, true};
		}
		return {};
	}

	tl::expected<void, ErrorContext> ReadOrder(sexpr const& option)
	{
		size_t end = option.items.size();
		bool   unique = false;
		// Only a bare `unique` is the flag; a column of that name is quoted.
		if (end > 1 && IsAtom(option.items[end - 1], "unique"))
		{
			unique = true;
			--end;
		}
		if (end < 2)
		{
			return Refuse(label, option.line, "(order ..) names at least one column");
		}
		for (size_t i = 1; i < end; ++i)
		{
			auto name = NameOf(option.items[i], storage, label, "an order column");
			if (!name)
			{
				return tl::make_unexpected(std::move(name.error()));
			}
			build.orderNames.push_back(reference{*name, option.items[i].line, true});
		}
		build.ordered = true;
		build.order.unique = unique;
		return {};
	}

	tl::expected<grid_axis, ErrorContext> ReadAxis(sexpr const& option)
	{
		if (option.items.size() != 3)
		{
			return Refuse(label, option.line, "(", HeadOf(option), " name type) needs a name and a type");
		}
		auto name = DeclaredName(option.items[1], storage, label, "an axis's name");
		if (!name)
		{
			return tl::make_unexpected(std::move(name.error()));
		}
		auto read = TypeOf(option.items[2], label);
		if (!read)
		{
			return tl::make_unexpected(std::move(read.error()));
		}
		return grid_axis{*name, *read};
	}

	tl::expected<void, ErrorContext> Read(sexpr const& option, option_name const& which)
	{
		switch (which.which)
		{
		case option::Key:
			return ReadColumn(option, true);
		case option::Column:
			return ReadColumn(option, false);
		case option::Input:
		{
			auto input = ReadInputOption(option, storage, label, cc(std::string(" of table '"), build.name, "'"));
			if (!input)
			{
				return tl::make_unexpected(std::move(input.error()));
			}
			build.inputs.push_back(*input);
			return {};
		}
		case option::Pick:
			return ReadPick(option);
		case option::Order:
			return ReadOrder(option);
		case option::Suffix:
		{
			if (option.items.size() != 2)
			{
				return Refuse(label, option.line, "(suffix ..) names one command column");
			}
			auto name = DeclaredName(option.items[1], storage, label, "the suffix's name");
			if (!name)
			{
				return tl::make_unexpected(std::move(name.error()));
			}
			build.suffix = *name;
			return {};
		}
		case option::Pivot:
		{
			if (option.items.size() != 3)
			{
				return Refuse(label, option.line, "(pivot headers cells) names the flags column of the headers and the column of the cells");
			}
			for (size_t i = 0; i < 2; ++i)
			{
				auto name = NameOf(option.items[i + 1], storage, label, "a pivot column");
				if (!name)
				{
					return tl::make_unexpected(std::move(name.error()));
				}
				build.pivotNames[i] = reference{*name, option.line, true};
			}
			build.pivoted = true;
			return {};
		}
		case option::Rows:
		{
			auto axis = ReadAxis(option);
			if (!axis)
			{
				return tl::make_unexpected(std::move(axis.error()));
			}
			build.grid.rows = *axis;
			return {};
		}
		case option::Columns:
		{
			auto axis = ReadAxis(option);
			if (!axis)
			{
				return tl::make_unexpected(std::move(axis.error()));
			}
			build.grid.columns = *axis;
			return {};
		}
		case option::Cells:
		{
			if (option.items.size() != 2)
			{
				return Refuse(label, option.line, "(cells type) names the type of a grid's cells");
			}
			auto type = TypeOf(option.items[1], label);
			if (!type)
			{
				return tl::make_unexpected(std::move(type.error()));
			}
			build.grid.cells = *type;
			return {};
		}
		case option::Count:
			break;
		}
		return {};
	}
};

uint32_t IndexOfColumn(table_build const& build, std::string_view name)
{
	for (uint32_t c = 0; c < build.columns.size(); ++c)
	{
		if (build.columns[c].name == name)
		{
			return c;
		}
	}
	return UINT32_MAX;
}

uint32_t IndexOfInput(table_build const& build, std::string_view name)
{
	for (uint32_t i = 0; i < build.inputs.size(); ++i)
	{
		if (build.inputs[i].name == name)
		{
			return i;
		}
	}
	return UINT32_MAX;
}

// Options come in any order, so the names they refer to resolve after the last.
tl::expected<void, ErrorContext> Resolve(table_build& build, std::string_view label)
{
	for (size_t c = 0; c < build.columns.size(); ++c)
	{
		reference const& input = build.columnInputs[c];
		if (!input.set)
		{
			continue;
		}
		uint32_t const found = IndexOfInput(build, input.name);
		if (found == UINT32_MAX)
		{
			return Refuse(label, input.line, "column '", build.columns[c].name, "' of table '", build.name,
				"' reads input '", input.name, "', which the table does not declare");
		}
		build.columns[c].input = found;
	}
	for (reference const& name : build.orderNames)
	{
		uint32_t const found = IndexOfColumn(build, name.name);
		if (found == UINT32_MAX)
		{
			return Refuse(label, name.line, "the order of table '", build.name, "' names column '", name.name,
				"', which the table does not have");
		}
		build.orderColumns.push_back(found);
	}
	build.order.columns = build.orderColumns;

	auto resolve = [&](reference const& ref, char const* what, uint32_t& out) -> tl::expected<void, ErrorContext>
	{
		if (!ref.set)
		{
			return {};
		}
		out = IndexOfColumn(build, ref.name);
		if (out == UINT32_MAX)
		{
			return Refuse(label, ref.line, "table '", build.name, "' names ", what, " '", ref.name,
				"', which the table does not have");
		}
		return {};
	};
	auto output = resolve(build.output, "output", build.outputColumn);
	if (!output)
	{
		return output;
	}
	auto weight = resolve(build.weight, "weight", build.weightColumn);
	if (!weight)
	{
		return weight;
	}
	if (build.pivoted)
	{
		for (reference const& ref : build.pivotNames)
		{
			if (IndexOfColumn(build, ref.name) == UINT32_MAX)
			{
				return Refuse(label, ref.line, "the pivot of table '", build.name, "' names a column the table does not have");
			}
		}
		build.pivot = pivot_by{IndexOfColumn(build, build.pivotNames[0].name), IndexOfColumn(build, build.pivotNames[1].name)};
	}
	return {};
}

std::string_view OptionHead(sexpr const& item)
{
	return item.what == sexpr::kind::List ? HeadOf(item) : item.text;
}

}

tl::expected<std::string_view, ErrorContext> NameOf(sexpr const& item, DocumentStorage& storage,
	std::string_view label, std::string_view what)
{
	if (item.what == sexpr::kind::List)
	{
		return Refuse(label, item.line, what, " is a list; it is a name");
	}
	if (item.what == sexpr::kind::String && item.text.find('\\') != std::string_view::npos)
	{
		storage.owned.push_back(Unescape(item));
		return std::string_view(storage.owned.back());
	}
	return item.text;
}

tl::expected<std::string_view, ErrorContext> DeclaredName(sexpr const& item, DocumentStorage& storage,
	std::string_view label, std::string_view what)
{
	auto name = NameOf(item, storage, label, what);
	if (!name)
	{
		return name;
	}
	if (item.what == sexpr::kind::String)
	{
		return Refuse(label, item.line, what, " '", *name, "' is quoted; a declared name is a bare word");
	}
	return name;
}

tl::expected<column_type, ErrorContext> TypeOf(sexpr const& item, std::string_view label)
{
	if (item.what != sexpr::kind::List)
	{
		if (auto type = TypeNamed(item.text))
		{
			return *type;
		}
	}
	return Refuse(label, item.line, "unknown type '", item.text, "'; a type is hash, flags, int, double, condition, command, vec2, string or band");
}

tl::expected<add_input, ErrorContext> ReadInputOption(sexpr const& option, DocumentStorage& storage,
	std::string_view label, std::string const& whose)
{
	if (option.items.size() < 3)
	{
		return Refuse(label, option.line, "(input name type) needs a name and a type");
	}
	auto name = DeclaredName(option.items[1], storage, label, "an input's name");
	if (!name)
	{
		return tl::make_unexpected(std::move(name.error()));
	}
	auto type = TypeOf(option.items[2], label);
	if (!type)
	{
		return tl::make_unexpected(std::move(type.error()));
	}
	add_input input{*name, *type};
	bool      hasRange = false;
	for (size_t i = 3; i < option.items.size(); ++i)
	{
		sexpr const& part = option.items[i];
		if (IsAtom(part, "cyclic"))
		{
			if (input.cyclic)
			{
				return Refuse(label, part.line, "input '", *name, "'", whose, " repeats 'cyclic'");
			}
			input.cyclic = true;
			continue;
		}
		if (part.what == sexpr::kind::List)
		{
			return Refuse(label, part.line, "input '", *name, "'", whose, " has the unknown option '", OptionHead(part),
				"'; an input takes a range and cyclic");
		}
		if (hasRange)
		{
			return Refuse(label, part.line, "input '", *name, "'", whose, " has a second range '", part.text, "'");
		}
		hasRange = true;
		auto text = NameOf(part, storage, label, "an input's range");
		if (!text)
		{
			return tl::make_unexpected(std::move(text.error()));
		}
		input.ranged = ParseRange(*text, input.lo, input.hi);
		if (!input.ranged)
		{
			return Refuse(label, part.line, "input '", *name, "'", whose, " has range '", *text,
				"'; a range is written lo..hi");
		}
	}
	return input;
}

tl::expected<schema, ErrorContext> ReadTables(std::span<section const> sections, std::string_view label,
	DocumentStorage& storage)
{
	std::deque<table_build>  builds;
	std::vector<create_table> commands;
	std::vector<uint32_t>     headerLines;   // per table, the line its bracket opens on

	size_t tableCount = 0;
	size_t rowsCount  = 0;
	for (section const& section : sections)
	{
		std::string_view const head = section.header.items[0].text;
		if (section.header.items[0].what == sexpr::kind::Atom && (head == "table" || head == "grid"))
		{
			++tableCount;
			rowsCount += HasRows(section.body, head == "grid") ? 1 : 0;
		}
	}
	storage.table_names = shared_array<std::string>(tableCount);
	storage.rows        = shared_array<section_rows>(rowsCount);
	size_t rowsAt       = 0;

	for (size_t s = 0; s < sections.size(); ++s)
	{
		section const& section = sections[s];
		sexpr const&   header  = section.header;
		if (header.items[0].what != sexpr::kind::Atom)
		{
			return Refuse(label, header.line, "a bracket starts with table, grid or machine");
		}
		std::string_view const head = header.items[0].text;
		if (head == "machine")
		{
			continue;
		}
		if (head != "table" && head != "grid")
		{
			return Refuse(label, header.line, "unknown bracket '", head, "'; a file declares [table ..], [grid ..] and [machine ..]");
		}
		bool const isGrid = head == "grid";
		if (header.items.size() < 2)
		{
			return Refuse(label, header.line, "[", head, "] needs a name");
		}
		builds.emplace_back();
		table_build& build = builds.back();
		auto name = DeclaredName(header.items[1], storage, label, "a table's name");
		if (!name)
		{
			return tl::make_unexpected(std::move(name.error()));
		}
		build.name = *name;
		storage.table_names[commands.size()] = std::string(*name);
		headerLines.push_back(header.line);
		if (isGrid)
		{
			build.pick = pick_policy::None;
		}

		std::array<bool, static_cast<size_t>(option::Count)> seen{};
		reader in{label, storage, build};
		for (size_t i = 2; i < header.items.size(); ++i)
		{
			sexpr const& opt = header.items[i];
			if (opt.what != sexpr::kind::List || opt.items.empty() || opt.items[0].what != sexpr::kind::Atom)
			{
				return Refuse(label, opt.line, "table '", build.name, "' has '", opt.what == sexpr::kind::List ? "a list" : opt.text,
					"' where an option in parentheses belongs, like (column x int)");
			}
			option_name const* found = nullptr;
			for (option_name const& candidate : kOptions)
			{
				if (candidate.name == opt.items[0].text)
				{
					found = &candidate;
				}
			}
			if (found == nullptr)
			{
				return Refuse(label, opt.line, "unknown option '", opt.items[0].text, "' on ", head, " '", build.name, "'");
			}
			if (found->grid != isGrid)
			{
				return Refuse(label, opt.line, "option '", found->name, "' is not taken by a [", head, "]; it belongs to a [",
					isGrid ? "table" : "grid", "]");
			}
			bool& flag = seen[static_cast<size_t>(found->which)];
			if (flag && !found->repeats)
			{
				return Refuse(label, opt.line, "option '", found->name, "' is given twice on ", head, " '", build.name, "'");
			}
			flag = true;
			auto read = in.Read(opt, *found);
			if (!read)
			{
				return tl::make_unexpected(std::move(read.error()));
			}
		}
		if (isGrid)
		{
			for (option_name const& required : kOptions)
			{
				if (required.grid && !seen[static_cast<size_t>(required.which)])
				{
					return Refuse(label, header.line, "grid '", build.name, "' needs (", required.name, " ..)");
				}
			}
		}
		auto resolved = Resolve(build, label);
		if (!resolved)
		{
			return tl::make_unexpected(std::move(resolved.error()));
		}
		// Every table a file declares is read-only: a reload loads a new version,
		// and a mutable table is declared elsewhere.
		commands.push_back(create_table{build.name, build.columns, build.ordered ? &build.order : nullptr, true,
			build.inputs, build.pick, build.outputColumn, build.pivoted ? &build.pivot : nullptr,
			isGrid ? &build.grid : nullptr, build.suffix, build.weightColumn});
		if (HasRows(section.body, isGrid))
		{
			storage.rows[rowsAt++] = section_rows{static_cast<uint32_t>(commands.size() - 1), section.body, section.body_line};
		}
	}

	uint32_t refused = UINT32_MAX;
	auto     built   = BuildSchema(commands, &refused);
	if (!built)
	{
		// The builder numbers the table it refused; the header's line is where
		// the author finds it.
		if (refused < headerLines.size())
		{
			return tl::make_unexpected(std::move(built.error()).with(Place(label, headerLines[refused])));
		}
		return tl::make_unexpected(std::move(built.error()).with(std::string(label)));
	}
	return std::move(*built);
}

}
