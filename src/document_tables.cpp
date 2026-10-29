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
	Key, Column, Input, Pick, Order, Suffix, Pivot, Domain, Rows, Columns, Cells, Count
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
	{"domain",  option::Domain,  false, true},
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
	std::string                   pivotCells;    // the cells column as the bracket writes it, `clip: string`
	grid_shape                    grid;
	bool                          pickGiven = false;
	// `(domain key ..)` options, resolved against the header's keys.
	std::vector<sexpr const*>     domains;
	// The header row's column names, bare, as the compiled body's header says
	// them; a pivot's flag headers follow its keys.
	std::vector<std::string_view> headerNames;
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
		return ReadMembers(std::span<sexpr const>(what.items).subspan(1), column, type);
	}

	// A key's literal members, read as cells of its type.
	tl::expected<key_domain, ErrorContext> ReadMembers(std::span<sexpr const> items, std::string_view column,
		column_type type)
	{
		build.members.emplace_back();
		std::vector<cell>& members = build.members.back();
		hash_names         names;
		column_def const   asColumn{std::string(column), type};
		for (size_t m = 0; m < items.size(); ++m)
		{
			auto text = NameOf(items[m], storage, label, "a domain member");
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
			auto value = ReadCell(asColumn, *text, names, items[m].line);
			if (!value)
			{
				return tl::make_unexpected(std::move(value.error()).with("member ", m, " of the domain of column '",
					column, "'").with(Place(label, items[m].line)));
			}
			members.push_back(*value);
		}
		return key_domain{domain_kind::Members, members, {}, {}};
	}

	// `(domain key a b ..)` for members, `(domain key table.column)` for
	// another table's column.
	tl::expected<key_domain, ErrorContext> ReadDomainOption(sexpr const& option, std::string_view column,
		column_type type)
	{
		sexpr const& first = option.items[2];
		size_t const dot   = first.what == sexpr::kind::Atom ? first.text.find('.') : std::string_view::npos;
		if (option.items.size() == 3 && dot != std::string_view::npos)
		{
			std::string_view const table = first.text.substr(0, dot);
			std::string_view const other = first.text.substr(dot + 1);
			if (table.empty() || other.empty() || other.find('.') != std::string_view::npos)
			{
				return Refuse(label, first.line, "the domain of column '", column, "' names '", first.text,
					"'; another table's column is written table.column");
			}
			return key_domain{domain_kind::Column, {}, table, other};
		}
		return ReadMembers(std::span<sexpr const>(option.items).subspan(2), column, type);
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
		build.pickGiven = true;
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
			if (option.items.size() < 3)
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
			// The cells column is declared here, as a header cell is: `clip: string`
			// may be read as one atom or as several.
			for (size_t i = 2; i < option.items.size(); ++i)
			{
				if (option.items[i].what == sexpr::kind::List)
				{
					return Refuse(label, option.items[i].line, "(pivot headers cells) has a list where the cells column belongs");
				}
				build.pivotCells += (i > 2 ? " " : "");
				build.pivotCells += option.items[i].text;
			}
			build.pivoted = true;
			return {};
		}
		case option::Domain:
			if (option.items.size() < 3)
			{
				return Refuse(label, option.line, "(domain key ..) names a key and its members, or a key and table.column");
			}
			build.domains.push_back(&option);
			return {};
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

// Whether a header cell says more than a name. A table declared by options has
// a header row of bare names; a typed one means the header declares the columns.
bool IsTyped(std::string_view cell)
{
	return cell.starts_with('>') || cell.starts_with('^') || cell.find_first_of(":=?") != std::string_view::npos;
}

// The columns a header row declares, in its order, as the Spellings in the
// shorthand plan have them: `>key`, `>key?`, `^edge`, `name: type`, a band or a
// ranking flags column named for its input, and a last command cell that is the
// suffix. A pivot's row is its keys, then flag headers; its other two columns
// are the bracket's.
tl::expected<void, ErrorContext> ReadHeader(reader& in, csv_text const& body)
{
	table_build&           build = in.build;
	std::string_view const label = in.label;
	bool const             any   = body.rows() > 0;   // a keyless pivot may have no header row
	uint32_t const         line  = any ? body.line_of_row(0) : 0;
	size_t const           width = any ? body.used_width(0) : 0;
	std::vector<bool>      fallsThrough;
	for (size_t h = 0; h < width; ++h)
	{
		std::string_view const text = body.cell(h, 0);
		if (build.pivoted && !text.starts_with('>'))
		{
			for (size_t f = h; f < width; ++f)
			{
				build.headerNames.push_back(body.cell(f, 0));
			}
			break;
		}
		auto cell = ParseHeaderCell(text);
		if (!cell)
		{
			return tl::make_unexpected(std::move(cell.error()).with("table '", build.name, "'").with(Place(label, line)));
		}
		auto type = TypeOfCell(*cell);
		if (!type)
		{
			return tl::make_unexpected(std::move(type.error()).with("table '", build.name, "'").with(Place(label, line)));
		}
		if (build.pivoted && cell->what != header_cell::mark::Key)
		{
			return Refuse(label, line, "pivoted table '", build.name, "' has '", cell->text,
				"' among its keys; a pivot's header row is its keys, then its flag headers");
		}
		build.headerNames.push_back(cell->name);
		bool const key = cell->what == header_cell::mark::Key;
		if (!key && *type == column_type::Command && h + 1 == width)
		{
			build.suffix = cell->name;
			continue;
		}
		add_column column{cell->name, *type, key};
		column.edge          = cell->what == header_cell::mark::Edge;
		uint32_t const input = IndexOfInput(build, cell->name);
		if (*type == column_type::Band)
		{
			if (input == UINT32_MAX)
			{
				return Refuse(label, line, "column '", cell->name, "' of table '", build.name,
					"' is a band, and the table has no input '", cell->name, "'; a band is named for the input it bands");
			}
			column.input = input;
		}
		else if (*type == column_type::Flags)
		{
			column.input = input;   // named for an input, it ranks; otherwise it is a value
		}
		build.columns.push_back(column);
		build.columnInputs.push_back(reference{});
		fallsThrough.push_back(cell->fallthrough);
	}

	if (build.pivoted)
	{
		uint32_t const line = build.pivotNames[0].line;
		auto           cell = ParseHeaderCell(build.pivotCells);
		if (!cell)
		{
			return tl::make_unexpected(std::move(cell.error()).with("the pivot of table '", build.name, "'")
				.with(Place(label, line)));
		}
		if (cell->what == header_cell::mark::Key || cell->fallback)
		{
			return Refuse(label, line, "the pivot of table '", build.name, "' declares its cells column '", cell->text,
				"'; it is a name, a type and '^' for an edge");
		}
		auto type = TypeOfCell(*cell);
		if (!type)
		{
			return tl::make_unexpected(std::move(type.error()).with("the pivot of table '", build.name, "'")
				.with(Place(label, line)));
		}
		uint32_t const input = IndexOfInput(build, cell->name);
		if (!cell->type && *type == column_type::Hash && input != UINT32_MAX)
		{
			*type = column_type::Band;   // named for an input, the cells band it
		}
		if (*type == column_type::Band && input == UINT32_MAX)
		{
			return Refuse(label, line, "the pivot of table '", build.name, "' has the band '", cell->name,
				"', and the table has no input '", cell->name, "'; a band is named for the input it bands");
		}
		uint32_t const keys = static_cast<uint32_t>(build.columns.size());
		build.columns.push_back(add_column{build.pivotNames[0].name, column_type::Flags, false,
			IndexOfInput(build, build.pivotNames[0].name)});
		build.columns.push_back(add_column{cell->name, *type, false,
			*type == column_type::Band || *type == column_type::Flags ? input : UINT32_MAX, {},
			cell->what == header_cell::mark::Edge});
		build.columnInputs.resize(build.columns.size());
		build.pivot = pivot_by{keys, keys + 1};
	}

	// A column named weight weighs the rows unless a (pick ..) says otherwise.
	if (!build.pickGiven && IndexOfColumn(build, "weight") != UINT32_MAX)
	{
		build.pick   = pick_policy::Weighted;
		build.weight = reference{"weight", line, true};
	}
	// Only a best table's keys have domains: its rows unless a (domain ..)
	// lists them.
	bool const best = build.pick == pick_policy::Best;
	for (size_t c = 0; c < fallsThrough.size(); ++c)
	{
		if (build.columns[c].key)
		{
			build.columns[c].domain = fallsThrough[c] ? key_domain::FallThrough() : best ? key_domain::Rows() : key_domain{};
		}
	}
	std::vector<bool> given(build.columns.size(), false);
	for (sexpr const* option : build.domains)
	{
		auto name = NameOf(option->items[1], in.storage, label, "a domain's key");
		if (!name)
		{
			return tl::make_unexpected(std::move(name.error()));
		}
		uint32_t const c = IndexOfColumn(build, *name);
		if (c == UINT32_MAX || !build.columns[c].key)
		{
			return Refuse(label, option->line, "(domain ", *name, " ..) on table '", build.name, "' names '", *name,
				"', which is not one of its keys");
		}
		if (fallsThrough[c])
		{
			return Refuse(label, option->line, "key '", *name, "' of table '", build.name,
				"' falls through, so its domain is open; it takes no (domain ..)");
		}
		if (given[c])
		{
			return Refuse(label, option->line, "table '", build.name, "' gives key '", *name, "' a second (domain ..)");
		}
		given[c]    = true;
		auto domain = in.ReadDomainOption(*option, *name, build.columns[c].type);
		if (!domain)
		{
			return tl::make_unexpected(std::move(domain.error()));
		}
		build.columns[c].domain = *domain;
	}
	return {};
}

bool IsBlank(char c)
{
	return c == ' ' || c == '\t' || c == '\r';
}

// A cell as plain CSV: quoted only where csv_text would otherwise read other
// text, so the cell comes back exactly.
void AppendCell(std::string& out, std::string_view text, bool first)
{
	bool const quote = text.find_first_of(",\"\n") != std::string_view::npos
		|| (!text.empty() && (IsBlank(text.front()) || IsBlank(text.back())))
		|| (first && text.starts_with('#'));
	if (!quote)
	{
		out += text;
		return;
	}
	out += '"';
	for (char c : text)
	{
		if (c == '"')
		{
			out += '"';
		}
		out += c;
	}
	out += '"';
}

// The one place a table's rows are rewritten into the plain CSV the loaders
// read: a header-form header becomes bare names, and every record keeps the
// file line it came from.
void CompileBody(table_build const& build, csv_text const& body, bool headerForm, std::string& text,
	shared_array<uint32_t>& lines)
{
	lines = shared_array<uint32_t>(body.rows());
	for (size_t r = 0; r < body.rows(); ++r)
	{
		if (r == 0 && headerForm)
		{
			for (size_t h = 0; h < build.headerNames.size(); ++h)
			{
				text += h == 0 ? "" : ",";
				AppendCell(text, build.headerNames[h], h == 0);
			}
		}
		else
		{
			for (size_t c = 0; c < body.width(r); ++c)
			{
				text += c == 0 ? "" : ",";
				AppendCell(text, body.cell(c, r), c == 0);
			}
		}
		text += '\n';
		lines[r] = body.line_of_row(r);
	}
}

// Options come in any order, so the names they refer to resolve after the last.
tl::expected<void, ErrorContext> Resolve(table_build& build, std::string_view label, bool headerForm)
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
	if (build.pivoted && !headerForm)
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
	// Each table's body as records. Header-form names view into them, so they
	// live until the schema is built.
	std::deque<csv_text>      bodies;
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
	storage.rows           = shared_array<section_rows>(rowsCount);
	storage.compiled       = shared_array<std::string>(rowsCount);
	storage.compiled_lines = shared_array<shared_array<uint32_t>>(rowsCount);
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
		// A table declares its columns by options or by its header row, never
		// both: the header replaces the options.
		bool const options = seen[static_cast<size_t>(option::Key)] || seen[static_cast<size_t>(option::Column)]
			|| seen[static_cast<size_t>(option::Suffix)];
		if (!isGrid)
		{
			auto read = csv_text::Read(section.body, label, section.body_line);
			if (!read)
			{
				return tl::make_unexpected(std::move(read.error()));
			}
			bodies.push_back(std::move(*read));
			csv_text const& body = bodies.back();
			if (options)
			{
				if (!build.domains.empty())
				{
					return Refuse(label, build.domains.front()->line, "table '", build.name,
						"' declares its columns with (key ..) and (column ..); (domain ..) goes with a header row that declares them");
				}
				if (build.pivoted && build.pivotCells != build.pivotNames[1].name)
				{
					return Refuse(label, build.pivotNames[1].line, "table '", build.name,
						"' declares its columns with (key ..) and (column ..); a pivot's cells column is typed with a header row");
				}
				for (size_t h = 0; body.rows() > 0 && h < body.used_width(0); ++h)
				{
					if (IsTyped(body.cell(h, 0)))
					{
						return Refuse(label, body.line_of_row(0), "table '", build.name, "' declares its columns with options and its header cell '",
							body.cell(h, 0), "' declares one too; the header row declares the columns and replaces (key ..), (column ..) and (suffix ..)");
					}
				}
			}
			else
			{
				// A pivot's bracket declares its two columns, so one with no keys
				// needs no header row.
				if (body.rows() == 0 && !build.pivoted)
				{
					return Refuse(label, header.line, "table '", build.name,
						"' has no columns; the first row of its body declares them, as `>key, name: type`");
				}
				auto declared = ReadHeader(in, body);
				if (!declared)
				{
					return tl::make_unexpected(std::move(declared.error()));
				}
			}
		}
		auto resolved = Resolve(build, label, !options);
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
			uint32_t const table = static_cast<uint32_t>(commands.size() - 1);
			if (isGrid)
			{
				storage.rows[rowsAt] = section_rows{table, section.body, section.body_line};
			}
			else
			{
				std::string&            text  = storage.compiled[rowsAt];
				shared_array<uint32_t>& lines = storage.compiled_lines[rowsAt];
				CompileBody(build, bodies.back(), !options, text, lines);
				storage.rows[rowsAt] = section_rows{table, text, section.body_line,
					std::span<uint32_t const>(lines.begin(), lines.size())};
			}
			++rowsAt;
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
