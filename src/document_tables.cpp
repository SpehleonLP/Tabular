#include "document_internal.h"
#include "document_header.h"
#include "csv_cells.h"

#include <algorithm>
#include <charconv>
#include <deque>

namespace tabular::detail
{

namespace
{

enum class option : uint8_t
{
	Input, Pick, Order, Pivot, Domain, Rows, Columns, Cells, Count
};

struct option_name
{
	std::string_view name;
	option           which;
	bool             grid;       // taken by a [grid] rather than a [table]
	bool             repeats;    // may appear more than once
};

constexpr option_name kOptions[] = {
	{"input",   option::Input,   false, true},
	{"pick",    option::Pick,    false, false},
	{"order",   option::Order,   false, false},
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

// What a blank cell of one header column compiles to.
struct column_fill
{
	enum class kind : uint8_t
	{
		Blank,      // left blank: the loader decides, as for a row written out
		Text,       // the column's default
		Key,        // the row's own cell of a key the default names
		Range,      // a band's input's whole range
		Unranged,   // a band whose input has no range to stand for: refused
	};
	kind        what = kind::Blank;
	std::string text;               // Text and Range
	uint32_t    key  = UINT32_MAX;  // Key: the key's header position
};

struct compiled_row
{
	std::vector<std::string_view> cells;
	uint32_t                      line = 0;      // the line the author wrote it on
	bool                          copy = false;  // one of a `*` row's copies
};

struct table_build
{
	std::string_view              name;
	std::vector<add_column>       columns;
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
	reference                     pivotHeaders;   // the flags column of the headers
	std::string                   pivotCells;    // the cells column as the bracket writes it, `clip: string`
	grid_shape                    grid;
	bool                          pickGiven = false;
	// `(domain key ..)` options, resolved against the header's keys.
	std::vector<sexpr const*>     domains;
	// The header row's column names, bare, as the compiled body's header says
	// them; a pivot's flag headers follow its keys.
	std::vector<std::string_view> headerNames;
	// Per header cell, what a blank cell compiles to; empty for a pivot, whose
	// blank is a cell with no row.
	std::vector<column_fill>      fills;
	// Per column, for a key: the members its (domain ..) lists, which a `*`
	// copies its row for; with none, the rows' own values are. A key whose
	// domain is another table's column takes no `*`.
	std::vector<std::vector<std::string_view>> starMembers;
	std::vector<bool>                          foreignDomain;
	// Rows its [fragment ..] sections add after its own, in section order.
	std::vector<compiled_row>                  fragmentRows;
	csv_text const*                            body     = nullptr;   // its own rows; null for a grid
	std::string_view                           text;                 // its section's body, as written
	bool                                       ownRows  = false;     // its body has rows, not just a header
	uint32_t                                   table    = 0;
	uint32_t                                   bodyLine = 0;
	bool                                       isGrid   = false;
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
		case option::Pivot:
		{
			if (option.items.size() < 3)
			{
				return Refuse(label, option.line, "(pivot headers cells) names the flags column of the headers and the column of the cells");
			}
			auto headers = NameOf(option.items[1], storage, label, "a pivot column");
			if (!headers)
			{
				return tl::make_unexpected(std::move(headers.error()));
			}
			build.pivotHeaders = reference{*headers, option.line, true};
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
	std::vector<std::optional<std::string_view>> fallbacks;
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
		fallbacks.push_back(cell->fallback);
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
		fallsThrough.push_back(cell->fallthrough);
	}

	if (build.pivoted)
	{
		uint32_t const line = build.pivotHeaders.line;
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
		build.columns.push_back(add_column{build.pivotHeaders.name, column_type::Flags, false,
			IndexOfInput(build, build.pivotHeaders.name)});
		build.columns.push_back(add_column{cell->name, *type, false,
			*type == column_type::Band || *type == column_type::Flags ? input : UINT32_MAX, {},
			cell->what == header_cell::mark::Edge});
		build.pivot = pivot_by{keys, keys + 1};
	}

	if (!build.pivoted)
	{
		build.fills.resize(fallbacks.size());
		for (size_t h = 0; h < fallbacks.size(); ++h)
		{
			column_fill& fill = build.fills[h];
			if (fallbacks[h])
			{
				// The suffix is last, so a column's header position is its index.
				uint32_t const named = IndexOfColumn(build, *fallbacks[h]);
				bool const     key   = named != UINT32_MAX && build.columns[named].key;
				fill.what = key ? column_fill::kind::Key : column_fill::kind::Text;
				fill.text = key ? std::string() : std::string(*fallbacks[h]);
				fill.key  = key ? named : UINT32_MAX;
				continue;
			}
			if (h >= build.columns.size() || build.columns[h].type != column_type::Band || build.columns[h].edge)
			{
				continue;
			}
			add_input const& input = build.inputs[build.columns[h].input];
			if (!input.ranged)
			{
				fill.what = column_fill::kind::Unranged;
				continue;
			}
			// Shortest round-trip text, so the loader reads back the very floats.
			char lo[32];
			char hi[32];
			char* const loEnd = std::to_chars(lo, lo + sizeof(lo), input.lo).ptr;
			char* const hiEnd = std::to_chars(hi, hi + sizeof(hi), input.hi).ptr;
			fill.what = column_fill::kind::Range;
			fill.text = cc(std::string(lo, loEnd), "..", std::string_view(hi, hiEnd - hi));
		}
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
	build.starMembers.resize(build.columns.size());
	build.foreignDomain.resize(build.columns.size());
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
		build.foreignDomain[c]  = domain->kind == domain_kind::Column;
		for (size_t m = 2; domain->kind == domain_kind::Members && m < option->items.size(); ++m)
		{
			build.starMembers[c].push_back(option->items[m].text);
		}
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

bool IsStar(std::string_view cell)
{
	return cell == "*";
}

// The values each key's `*` copies a row for: its domain's members, or else the
// distinct values the rows name for it, in the order they first appear.
tl::expected<std::vector<std::vector<std::string_view>>, ErrorContext> StarValues(table_build const& build,
	std::span<compiled_row const> rows, std::string_view label)
{
	std::vector<std::vector<std::string_view>> values(build.columns.size());
	for (size_t c = 0; c < build.columns.size(); ++c)
	{
		if (!build.columns[c].key)
		{
			continue;
		}
		auto const star = std::find_if(rows.begin(), rows.end(),
			[c](compiled_row const& row) { return c < row.cells.size() && IsStar(row.cells[c]); });
		if (star == rows.end())
		{
			continue;
		}
		std::string_view const name = build.columns[c].name;
		if (c < build.foreignDomain.size() && build.foreignDomain[c])
		{
			return Refuse(label, star->line, "key '", name, "' of table '", build.name,
				"' has '*', and its domain is another table's column; '*' copies its row for each member, so list its members with (domain ",
				name, " a b ..)");
		}
		if (c < build.starMembers.size() && !build.starMembers[c].empty())
		{
			values[c] = build.starMembers[c];
			continue;
		}
		for (compiled_row const& row : rows)
		{
			std::string_view const value = c < row.cells.size() ? row.cells[c] : std::string_view();
			if (!value.empty() && !IsStar(value) && std::find(values[c].begin(), values[c].end(), value) == values[c].end())
			{
				values[c].push_back(value);
			}
		}
		if (values[c].empty())
		{
			return Refuse(label, star->line, "key '", name, "' of table '", build.name,
				"' has '*' and no value for it to stand for; list its members with (domain ", name, " a b ..)");
		}
	}
	return values;
}

// Each `*` row as its copies, where it is written: one per value of each
// starred key, the first starred key varying slowest.
void Expand(compiled_row const& row, std::span<std::vector<std::string_view> const> values,
	std::vector<compiled_row>& out)
{
	std::vector<size_t> stars;
	for (size_t c = 0; c < row.cells.size() && c < values.size(); ++c)
	{
		if (IsStar(row.cells[c]) && !values[c].empty())
		{
			stars.push_back(c);
		}
	}
	if (stars.empty())
	{
		out.push_back(row);
		return;
	}
	std::vector<size_t> at(stars.size(), 0);
	while (true)
	{
		compiled_row copy = row;
		copy.copy         = true;
		for (size_t s = 0; s < stars.size(); ++s)
		{
			copy.cells[stars[s]] = values[stars[s]][at[s]];
		}
		out.push_back(std::move(copy));
		size_t s = stars.size();
		while (s > 0 && ++at[s - 1] == values[stars[s - 1]].size())
		{
			at[--s] = 0;
		}
		if (s == 0)
		{
			return;
		}
	}
}

// A blank cell takes its column's fill, after expansion, so a default naming a
// key takes the copy's key.
tl::expected<void, ErrorContext> Fill(table_build const& build, compiled_row& row, std::string_view label)
{
	using kind = column_fill::kind;
	std::vector<std::string_view>& cells = row.cells;
	size_t const suffixAt = build.suffix.empty() ? SIZE_MAX : build.headerNames.size() - 1;
	// The suffix runs to the row's end, so it is blank only if all of it is.
	bool const suffixBlank = suffixAt < cells.size()
		&& std::all_of(cells.begin() + suffixAt, cells.end(), [](std::string_view c) { return c.empty(); });
	for (size_t c = 0; c < cells.size() && c < build.fills.size(); ++c)
	{
		column_fill const& fill = build.fills[c];
		if (!cells[c].empty() || fill.what == kind::Blank || (c == suffixAt && !suffixBlank))
		{
			continue;
		}
		switch (fill.what)
		{
		case kind::Text:
		case kind::Range:
			cells[c] = fill.text;
			break;
		case kind::Key:
			cells[c] = fill.key < cells.size() ? cells[fill.key] : std::string_view();
			break;
		case kind::Unranged:
			return Refuse(label, row.line, "column '", build.headerNames[c], "' of table '", build.name,
				"' is blank, and its input '", build.inputs[build.columns[c].input].name,
				"' has no range for a blank band to stand for; write the band, or give the input a range");
		case kind::Blank:
			break;
		}
	}
	return {};
}

// Whether a row answers every input it is picked with: no condition, no edge,
// and every band its input's whole range.
bool Unconditional(table_build const& build, compiled_row const& row)
{
	for (size_t c = 0; c < build.columns.size() && c < row.cells.size(); ++c)
	{
		add_column const&      column = build.columns[c];
		std::string_view const cell   = row.cells[c];
		if (column.type == column_type::Condition || (column.type == column_type::Band && column.edge))
		{
			if (!cell.empty())
			{
				return false;
			}
		}
		else if (column.type == column_type::Band)
		{
			add_input const& input = build.inputs[column.input];
			float            lo    = 0;
			float            hi    = 0;
			if (!input.ranged || !ParseRange(cell, lo, hi) || lo != input.lo || hi != input.hi)
			{
				return false;
			}
		}
	}
	return true;
}

// A copy an earlier unconditional row with the same keys and ranking flags
// already answers would be a dead row: the `*` yields to the key's own row.
bool Answered(table_build const& build, std::span<compiled_row const> earlier, compiled_row const& copy)
{
	for (compiled_row const& row : earlier)
	{
		bool same = true;
		for (size_t c = 0; same && c < build.columns.size(); ++c)
		{
			add_column const& column  = build.columns[c];
			bool const        ranking = column.type == column_type::Flags && column.input != UINT32_MAX;
			if (column.key || ranking)
			{
				std::string_view const a = c < row.cells.size() ? row.cells[c] : std::string_view();
				std::string_view const b = c < copy.cells.size() ? copy.cells[c] : std::string_view();
				same = a == b;
			}
		}
		if (same && Unconditional(build, row))
		{
			return true;
		}
	}
	return false;
}

// The one place a table's rows are rewritten into the plain CSV the loaders
// read: the header becomes bare names, fragments' rows follow the table's own,
// `*` rows become their copies, blank
// cells take their column's fill, and every record keeps the file line it came
// from.
tl::expected<void, ErrorContext> CompileBody(table_build const& build, csv_text const& body, std::string_view label,
	std::string& text, shared_array<uint32_t>& lines)
{
	// A row shorter than the header still takes the fills it has cells for.
	size_t fillWidth = 0;
	for (size_t h = 0; h < build.fills.size(); ++h)
	{
		if (build.fills[h].what != column_fill::kind::Blank)
		{
			fillWidth = h + 1;
		}
	}
	std::vector<compiled_row> written;
	for (size_t r = 1; r < body.rows(); ++r)
	{
		compiled_row& row = written.emplace_back();
		row.line          = body.line_of_row(r);
		row.cells.assign(std::max(body.width(r), fillWidth), std::string_view());
		for (size_t c = 0; c < body.width(r); ++c)
		{
			row.cells[c] = body.cell(c, r);
		}
	}
	for (compiled_row const& fragment : build.fragmentRows)
	{
		compiled_row& row = written.emplace_back(fragment);
		row.cells.resize(std::max(row.cells.size(), fillWidth));
	}
	auto values = StarValues(build, written, label);
	if (!values)
	{
		return tl::make_unexpected(std::move(values.error()));
	}
	std::vector<compiled_row> expanded;
	for (compiled_row const& row : written)
	{
		Expand(row, *values, expanded);
	}
	std::vector<compiled_row> kept;
	for (compiled_row& row : expanded)
	{
		auto filled = Fill(build, row, label);
		if (!filled)
		{
			return filled;
		}
		// A pivot's cells are its columns' flag headers, so no copy of one is
		// judged dead here; the loader judges every row.
		if (row.copy && !build.pivoted && Answered(build, kept, row))
		{
			continue;
		}
		kept.push_back(std::move(row));
	}

	lines = shared_array<uint32_t>(kept.size() + (body.rows() > 0 ? 1 : 0));
	if (body.rows() > 0)
	{
		lines[0] = body.line_of_row(0);
		for (size_t h = 0; h < build.headerNames.size(); ++h)
		{
			text += h == 0 ? "" : ",";
			AppendCell(text, build.headerNames[h], h == 0);
		}
		text += '\n';
	}
	for (size_t r = 0; r < kept.size(); ++r)
	{
		lines[r + 1] = kept[r].line;
		for (size_t c = 0; c < kept[r].cells.size(); ++c)
		{
			text += c == 0 ? "" : ",";
			AppendCell(text, kept[r].cells[c], c == 0);
		}
		text += '\n';
	}
	return {};
}

// Options come in any order, so the names they refer to resolve after the last.
tl::expected<void, ErrorContext> Resolve(table_build& build, std::string_view label)
{
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
	return {};
}

std::string_view OptionHead(sexpr const& item)
{
	return item.what == sexpr::kind::List ? HeadOf(item) : item.text;
}

// `[fragment t (col v ..)..]`: rows for a table declared above it. Its header
// names the table's columns, bare; a bracket constant sets a column on every
// row, one copy per value it lists.
tl::expected<void, ErrorContext> ReadFragment(section const& section, std::deque<table_build>& builds,
	std::deque<csv_text>& bodies, DocumentStorage& storage, std::string_view label)
{
	sexpr const& header = section.header;
	if (header.items.size() < 2)
	{
		return Refuse(label, header.line, "[fragment] names the table it adds rows to");
	}
	auto name = NameOf(header.items[1], storage, label, "a fragment's table");
	if (!name)
	{
		return tl::make_unexpected(std::move(name.error()));
	}
	auto const found = std::find_if(builds.rbegin(), builds.rend(), [&](table_build const& b) { return b.name == *name; });
	if (found == builds.rend())
	{
		return Refuse(label, header.line, "fragment of '", *name,
			"' names no table declared above it; a [fragment ..] follows the [table ..] it adds rows to");
	}
	table_build& table = *found;
	if (table.isGrid || table.pivoted)
	{
		return Refuse(label, header.line, "fragment of '", *name, "' adds rows to a ", table.isGrid ? "grid" : "pivot",
			", which is written whole in its own section");
	}
	auto positionOf = [&](std::string_view column) -> size_t
	{
		auto const at = std::find(table.headerNames.begin(), table.headerNames.end(), column);
		return at == table.headerNames.end() ? SIZE_MAX : static_cast<size_t>(at - table.headerNames.begin());
	};

	struct constant
	{
		size_t                        at = SIZE_MAX;
		std::vector<std::string_view> values;
		uint32_t                      line = 0;
	};
	std::vector<constant> constants;
	for (size_t i = 2; i < header.items.size(); ++i)
	{
		sexpr const& option = header.items[i];
		if (option.what != sexpr::kind::List || option.items.size() < 2)
		{
			return Refuse(label, option.line, "fragment of '", *name, "' has '", OptionHead(option),
				"' where a constant (column value ..) belongs");
		}
		auto column = NameOf(option.items[0], storage, label, "a fragment constant's column");
		if (!column)
		{
			return tl::make_unexpected(std::move(column.error()));
		}
		constant& c = constants.emplace_back();
		c.at        = positionOf(*column);
		c.line      = option.line;
		if (c.at == SIZE_MAX)
		{
			return Refuse(label, option.line, "fragment of '", *name, "' sets column '", *column,
				"', which table '", *name, "' does not declare");
		}
		if (std::any_of(constants.begin(), constants.end() - 1, [&](constant const& o) { return o.at == c.at; }))
		{
			return Refuse(label, option.line, "fragment of '", *name, "' sets column '", *column, "' twice");
		}
		for (size_t v = 1; v < option.items.size(); ++v)
		{
			auto value = NameOf(option.items[v], storage, label, "a fragment constant's value");
			if (!value)
			{
				return tl::make_unexpected(std::move(value.error()));
			}
			c.values.push_back(*value);
		}
		bool const star = std::any_of(c.values.begin(), c.values.end(), IsStar);
		if (star && (c.values.size() != 1 || c.at >= table.columns.size() || !table.columns[c.at].key))
		{
			return Refuse(label, option.line, "fragment of '", *name, "' sets column '", *column,
				"' to '*' among other values or on a column that is not a key; (key *) alone copies per key value");
		}
	}

	auto read = csv_text::Read(section.body, label, section.body_line);
	if (!read)
	{
		return tl::make_unexpected(std::move(read.error()));
	}
	bodies.push_back(std::move(*read));
	csv_text const& body = bodies.back();
	if (body.rows() == 0)
	{
		return {};
	}
	uint32_t const      headerLine = body.line_of_row(0);
	size_t const        width      = body.used_width(0);
	std::vector<size_t> positions;
	for (size_t h = 0; h < width; ++h)
	{
		auto cell = ParseHeaderCell(body.cell(h, 0));
		if (!cell)
		{
			return tl::make_unexpected(std::move(cell.error()).with("fragment of '", *name, "'").with(Place(label, headerLine)));
		}
		if (cell->what != header_cell::mark::None || cell->fallthrough || cell->type || cell->fallback)
		{
			return Refuse(label, headerLine, "fragment of '", *name, "' has the header cell '", cell->text,
				"'; a fragment's header names the table's columns, bare, and the table declares them");
		}
		size_t const at = positionOf(cell->name);
		if (at == SIZE_MAX)
		{
			return Refuse(label, headerLine, "fragment of '", *name, "' names column '", cell->name, "', which table '",
				*name, "' does not declare");
		}
		if (std::find(positions.begin(), positions.end(), at) != positions.end())
		{
			return Refuse(label, headerLine, "fragment of '", *name, "' names column '", cell->name, "' twice");
		}
		auto const constant = std::find_if(constants.begin(), constants.end(), [at](auto const& c) { return c.at == at; });
		if (constant != constants.end())
		{
			return Refuse(label, constant->line, "fragment of '", *name, "' sets column '", cell->name,
				"' in its bracket and names it in its header too");
		}
		positions.push_back(at);
	}
	// The suffix is the table's last column, so a fragment that names it last
	// runs it to the row's end as the table does.
	bool const suffixLast = !table.suffix.empty() && !positions.empty()
		&& positions.back() == table.headerNames.size() - 1;
	for (size_t r = 1; r < body.rows(); ++r)
	{
		uint32_t const line = body.line_of_row(r);
		if (body.used_width(r) > width && !suffixLast)
		{
			return Refuse(label, line, "a row of the fragment of '", *name, "' has more cells than its header names");
		}
		std::vector<std::string_view> cells(table.headerNames.size());
		for (size_t h = 0; h < width && h < body.width(r); ++h)
		{
			cells[positions[h]] = body.cell(h, r);
		}
		for (size_t extra = width; suffixLast && extra < body.width(r); ++extra)
		{
			cells.push_back(body.cell(extra, r));
		}
		// One copy per value of each constant, the first constant varying slowest.
		std::vector<size_t> at(constants.size(), 0);
		while (true)
		{
			compiled_row& row = table.fragmentRows.emplace_back();
			row.cells         = cells;
			row.line          = line;
			for (size_t c = 0; c < constants.size(); ++c)
			{
				row.cells[constants[c].at] = constants[c].values[at[c]];
			}
			size_t c = constants.size();
			while (c > 0 && ++at[c - 1] == constants[c - 1].values.size())
			{
				at[--c] = 0;
			}
			if (c == 0)
			{
				break;
			}
		}
	}
	return {};
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
	for (section const& section : sections)
	{
		std::string_view const head = section.header.items[0].text;
		if (section.header.items[0].what == sexpr::kind::Atom && (head == "table" || head == "grid"))
		{
			++tableCount;
		}
	}
	storage.table_names = shared_array<std::string>(tableCount);

	for (size_t s = 0; s < sections.size(); ++s)
	{
		section const& section = sections[s];
		sexpr const&   header  = section.header;
		if (header.items[0].what != sexpr::kind::Atom)
		{
			return Refuse(label, header.line, "a bracket starts with table, grid, fragment or machine");
		}
		std::string_view const head = header.items[0].text;
		if (head == "machine")
		{
			continue;
		}
		if (head == "fragment")
		{
			auto added = ReadFragment(section, builds, bodies, storage, label);
			if (!added)
			{
				return tl::make_unexpected(std::move(added.error()));
			}
			continue;
		}
		if (head != "table" && head != "grid")
		{
			return Refuse(label, header.line, "unknown bracket '", head,
				"'; a file declares [table ..], [grid ..], [fragment ..] and [machine ..]");
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
					"' where an option in parentheses belongs, like (input x double)");
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
		if (!isGrid)
		{
			auto read = csv_text::Read(section.body, label, section.body_line);
			if (!read)
			{
				return tl::make_unexpected(std::move(read.error()));
			}
			bodies.push_back(std::move(*read));
			csv_text const& body = bodies.back();
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
		build.table    = static_cast<uint32_t>(commands.size() - 1);
		build.isGrid   = isGrid;
		build.text     = section.body;
		build.bodyLine = section.body_line;
		build.body     = isGrid ? nullptr : &bodies.back();
		build.ownRows  = HasRows(section.body, isGrid);
	}
	// Fragments add to tables above them, so a table compiles once the whole
	// file is read, and only then is it known which tables have rows.
	auto const hasRows = [](table_build const& b) { return b.ownRows || !b.fragmentRows.empty(); };
	size_t const rowsCount = static_cast<size_t>(std::count_if(builds.begin(), builds.end(), hasRows));
	storage.rows           = shared_array<section_rows>(rowsCount);
	storage.compiled       = shared_array<std::string>(rowsCount);
	storage.compiled_lines = shared_array<shared_array<uint32_t>>(rowsCount);
	size_t rowsAt          = 0;
	for (table_build const& build : builds)
	{
		if (!hasRows(build))
		{
			continue;
		}
		if (build.isGrid)
		{
			storage.rows[rowsAt++] = section_rows{build.table, build.text, build.bodyLine};
			continue;
		}
		std::string&            text     = storage.compiled[rowsAt];
		shared_array<uint32_t>& lines    = storage.compiled_lines[rowsAt];
		auto                    compiled = CompileBody(build, *build.body, label, text, lines);
		if (!compiled)
		{
			return tl::make_unexpected(std::move(compiled.error()));
		}
		storage.rows[rowsAt++] = section_rows{build.table, text, build.bodyLine,
			std::span<uint32_t const>(lines.begin(), lines.size())};
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
