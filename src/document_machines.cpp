#include "document_internal.h"
#include "csv_cells.h"
#include "machine_internal.h"

#include <algorithm>

namespace tabular::detail
{

namespace
{

constexpr size_t           kClauseWidth = 7;
constexpr std::string_view kClauseHeader[kClauseWidth] = {"list", "from", "where", "given", "set", "emit", "overrides"};
constexpr std::string_view kListNames[kTriggerCount]   = {"create", "entry", "step", "exit"};

// A clause's where or given target, as a cell type, for reading a literal: a
// key column's or a table input's. Unknown names read as an int; CheckMachine
// refuses them by name afterwards, so this never has to.
column_type TargetType(schema const& tables, std::string_view table, std::string_view target, bool isKey)
{
	auto t = tables.FindTable(table);
	if (!t)
	{
		return column_type::Int;
	}
	table_def const& def = tables.tables()[*t];
	if (isKey)
	{
		auto column = tables.FindColumn(*t, target);
		return column ? def.columns[*column].type : column_type::Int;
	}
	for (input_def const& input : def.inputs)
	{
		if (input.name == target)
		{
			return input.type;
		}
	}
	return column_type::Int;
}

struct name_value
{
	std::string_view name;
	std::string_view value;
};

// A cell of space-separated `name=value` pairs. A value may hold spaces inside
// parentheses or a string, as `(literal "a b")` does. A blank cell is no pairs.
tl::expected<std::vector<name_value>, ErrorContext> SplitPairs(std::string_view cell, std::string_view what)
{
	std::vector<name_value> pairs;
	size_t                  at = 0;
	while (at < cell.size())
	{
		while (at < cell.size() && (cell[at] == ' ' || cell[at] == '\t' || cell[at] == '\n' || cell[at] == '\r'))
		{
			++at;
		}
		if (at == cell.size())
		{
			break;
		}
		size_t   end      = at;
		size_t   depth    = 0;
		bool     inString = false;
		while (end < cell.size())
		{
			char const c = cell[end];
			if (inString)
			{
				if (c == '\\')
				{
					++end;
				}
				else if (c == '"')
				{
					inString = false;
				}
			}
			else if (c == '"')
			{
				inString = true;
			}
			else if (c == '(')
			{
				++depth;
			}
			else if (c == ')' && depth > 0)
			{
				--depth;
			}
			else if (depth == 0 && (c == ' ' || c == '\t' || c == '\n' || c == '\r'))
			{
				break;
			}
			++end;
		}
		end = std::min(end, cell.size());
		std::string_view const token = cell.substr(at, end - at);
		size_t const           equals = token.find('=');
		if (equals == std::string_view::npos || equals == 0 || equals + 1 == token.size())
		{
			return tl::make_unexpected(ErrorContext(what, " has '", token, "'; each entry is name=source"));
		}
		name_value pair{token.substr(0, equals), token.substr(equals + 1)};
		for (name_value const& earlier : pairs)
		{
			if (earlier.name == pair.name)
			{
				return tl::make_unexpected(ErrorContext(what, " names '", pair.name, "' twice"));
			}
		}
		pairs.push_back(pair);
		at = end;
	}
	return pairs;
}

constexpr char kSourceShapes[] = "; a source is state.<column>, input.<name>, clause.<n>.<column> or (literal <text>)";

tl::expected<source, ErrorContext> ReadSource(std::string_view value, std::string_view target, column_type type,
	hash_names& names, std::string_view label, uint32_t line)
{
	if (value.front() == '(')
	{
		auto list = ParseList(value, line, label);
		if (!list)
		{
			// The caller names the file line once, with the clause.
			return tl::make_unexpected(ErrorContext(std::move(list.error().message)));
		}
		if (list->items.size() != 2 || list->items[0].what != sexpr::kind::Atom || list->items[0].text != "literal"
			|| list->items[1].what == sexpr::kind::List)
		{
			return tl::make_unexpected(ErrorContext("'", target, "' has the source '", value,
				"'; a literal is (literal <text>)"));
		}
		sexpr const&      item = list->items[1];
		std::string const text = item.what == sexpr::kind::String ? Unescape(item) : std::string(item.text);
		auto cellValue = ReadCell(column_def{std::string(target), type}, text, names, line);
		if (!cellValue)
		{
			return tl::make_unexpected(std::move(cellValue.error()).with("the literal for '", target, "'"));
		}
		source out;
		out.literal = *cellValue;
		return out;
	}
	source out;
	if (value.starts_with("state."))
	{
		out.kind = source_kind::State;
		out.name = value.substr(6);
	}
	else if (value.starts_with("input."))
	{
		out.kind = source_kind::Input;
		out.name = value.substr(6);
	}
	else if (value.starts_with("clause."))
	{
		std::string_view const rest  = value.substr(7);
		size_t const           dot   = rest.find('.');
		uint32_t               index = 0;
		if (dot == std::string_view::npos || !ParseWhole(rest.substr(0, dot), index))
		{
			return tl::make_unexpected(ErrorContext("'", target, "' reads '", value,
				"'; an earlier clause is named clause.<n>.<column>, n its position in the list"));
		}
		out.kind   = source_kind::Clause;
		out.clause = index;
		out.name   = rest.substr(dot + 1);
	}
	else
	{
		return tl::make_unexpected(ErrorContext("'", target, "' reads '", value, "'", kSourceShapes));
	}
	if (out.name.empty())
	{
		return tl::make_unexpected(ErrorContext("'", target, "' reads '", value, "', which names nothing"));
	}
	return out;
}

tl::expected<void, ErrorContext> ReadBinds(std::string_view cell, std::string_view key, schema const& tables,
	std::string_view table, hash_names& names, std::string_view label, uint32_t line, std::vector<clause_bind>& out)
{
	auto pairs = SplitPairs(cell, key);
	if (!pairs)
	{
		return tl::make_unexpected(std::move(pairs.error()));
	}
	for (name_value const& pair : *pairs)
	{
		auto from = ReadSource(pair.value, pair.name, TargetType(tables, table, pair.name, key == "where"), names, label, line);
		if (!from)
		{
			return tl::make_unexpected(std::move(from.error()).with(key));
		}
		out.push_back(clause_bind{pair.name, *from});
	}
	return {};
}

tl::expected<void, ErrorContext> ReadSets(std::string_view cell, std::vector<set_state>& out)
{
	auto pairs = SplitPairs(cell, "set");
	if (!pairs)
	{
		return tl::make_unexpected(std::move(pairs.error()));
	}
	for (name_value const& pair : *pairs)
	{
		if (pair.value.front() == '(')
		{
			return tl::make_unexpected(ErrorContext("set writes '", pair.name, "' from something that is not a column name"));
		}
		out.push_back(set_state{pair.name, pair.value});
	}
	return {};
}

// `(state name type [(members ..)] (initial x))`
tl::expected<void, ErrorContext> ReadState(sexpr const& option, size_t index, hash_names& names, std::string_view label,
	DocumentStorage& storage, machine_storage& store)
{
	if (option.items.size() < 3)
	{
		return Refuse(label, option.line, "(state name type ..) needs a name and a type");
	}
	auto name = DeclaredName(option.items[1], storage, label, "a state column's name");
	if (!name)
	{
		return tl::make_unexpected(std::move(name.error()));
	}
	auto type = TypeOf(option.items[2], label);
	if (!type)
	{
		return tl::make_unexpected(std::move(type.error()));
	}
	column_def const column{std::string(*name), *type};
	bool             hasMembers = false;
	sexpr const*     initial    = nullptr;
	for (size_t i = 3; i < option.items.size(); ++i)
	{
		sexpr const& part = option.items[i];
		bool const   isMembers = part.what == sexpr::kind::List && !part.items.empty() && IsAtom(part.items[0], "members");
		bool const   isInitial = part.what == sexpr::kind::List && !part.items.empty() && IsAtom(part.items[0], "initial");
		if (!isMembers && !isInitial)
		{
			return Refuse(label, part.line, "state column '", *name, "' has the unknown option '",
				part.what == sexpr::kind::List && !part.items.empty() ? part.items[0].text : part.text,
				"'; a state column takes (members ..) and (initial ..)");
		}
		if ((isMembers && hasMembers) || (isInitial && initial != nullptr))
		{
			return Refuse(label, part.line, "state column '", *name, "' repeats '", part.items[0].text, "'");
		}
		if (isInitial)
		{
			initial = &part;
			continue;
		}
		hasMembers = true;
		for (size_t m = 1; m < part.items.size(); ++m)
		{
			auto text = NameOf(part.items[m], storage, label, "a member");
			if (!text)
			{
				return tl::make_unexpected(std::move(text.error()));
			}
			auto value = ReadCell(column, *text, names, part.items[m].line);
			if (!value)
			{
				return tl::make_unexpected(std::move(value.error()).with("member ", m - 1, " of state column '", *name, "'")
					.with(Place(label, part.items[m].line)));
			}
			store.members[index].push_back(*value);
		}
	}
	if (initial == nullptr || initial->items.size() != 2)
	{
		return Refuse(label, option.line, "state column '", *name, "' needs (initial <value>)");
	}
	auto text = NameOf(initial->items[1], storage, label, "an initial value");
	if (!text)
	{
		return tl::make_unexpected(std::move(text.error()));
	}
	auto value = ReadCell(column, *text, names, initial->line);
	if (!value)
	{
		return tl::make_unexpected(std::move(value.error()).with("the initial value of state column '", *name, "'")
			.with(Place(label, initial->line)));
	}
	store.state.push_back(add_state{*name, *type, store.members[index], *value});
	return {};
}

}

tl::expected<void, ErrorContext> ReadMachines(std::span<section const> sections, std::string_view label,
	schema const& tables, DocumentStorage& storage)
{
	size_t machineCount = 0;
	for (section const& s : sections)
	{
		if (s.header.items[0].text == "machine")
		{
			++machineCount;
		}
	}
	storage.machines.resize(machineCount);
	storage.commands.resize(machineCount);

	size_t built = 0;
	for (section const& section : sections)
	{
		sexpr const& header = section.header;
		if (header.items[0].text != "machine")
		{
			continue;
		}
		machine_storage& store = storage.machines[built];
		create_machine&  out   = storage.commands[built];
		++built;

		if (header.items.size() < 2)
		{
			return Refuse(label, header.line, "[machine] needs a name");
		}
		auto machineName = DeclaredName(header.items[1], storage, label, "a machine's name");
		if (!machineName)
		{
			return tl::make_unexpected(std::move(machineName.error()));
		}
		std::string const whose = cc(std::string("machine '"), *machineName, "'");

		size_t stateCount = 0;
		for (size_t i = 2; i < header.items.size(); ++i)
		{
			sexpr const& opt = header.items[i];
			if (opt.what == sexpr::kind::List && !opt.items.empty() && IsAtom(opt.items[0], "state"))
			{
				++stateCount;
			}
		}
		store.members.resize(stateCount);

		hash_names names;
		size_t     stateAt = 0;
		for (size_t i = 2; i < header.items.size(); ++i)
		{
			sexpr const& opt = header.items[i];
			if (opt.what != sexpr::kind::List || opt.items.empty() || opt.items[0].what != sexpr::kind::Atom)
			{
				return Refuse(label, opt.line, whose, " has '", opt.what == sexpr::kind::List ? "a list" : opt.text,
					"' where an option in parentheses belongs, like (state mode hash (initial idle))");
			}
			if (opt.items[0].text == "state")
			{
				auto read = ReadState(opt, stateAt++, names, label, storage, store);
				if (!read)
				{
					return tl::make_unexpected(std::move(read.error()).with(whose));
				}
			}
			else if (opt.items[0].text == "input")
			{
				auto input = ReadInputOption(opt, storage, label, std::string());
				if (!input)
				{
					return tl::make_unexpected(std::move(input.error()).with(whose));
				}
				store.inputs.push_back(*input);
			}
			else
			{
				return Refuse(label, opt.line, "unknown option '", opt.items[0].text, "' on ", whose,
					"; a machine takes (state ..) and (input ..)");
			}
		}

		auto body = csv_text::Read(section.body, label, section.body_line);
		if (!body)
		{
			return tl::make_unexpected(std::move(body.error()));
		}
		storage.bodies.push_back(std::move(*body));
		csv_text const& csv = storage.bodies.back();

		std::array<size_t, kTriggerCount> counts{};
		if (csv.rows() > 0)
		{
			bool matches = csv.used_width(0) == kClauseWidth;
			for (size_t c = 0; matches && c < kClauseWidth; ++c)
			{
				matches = csv.cell(c, 0) == kClauseHeader[c];
			}
			if (!matches)
			{
				return Refuse(label, csv.line_of_row(0), whose, "'s body starts with the header 'list, from, where, given, set, emit, overrides'");
			}
		}
		for (size_t r = 1; r < csv.rows(); ++r)
		{
			uint32_t const line = csv.line_of_row(r);
			if (csv.used_width(r) > kClauseWidth)
			{
				return Refuse(label, line, whose, " has a clause row of ", csv.used_width(r), " cells; a clause has ", kClauseWidth);
			}
			size_t list = kTriggerCount;
			for (size_t l = 0; l < kTriggerCount; ++l)
			{
				if (csv.cell(0, r) == kListNames[l])
				{
					list = l;
				}
			}
			if (list == kTriggerCount)
			{
				return Refuse(label, line, whose, " has the clause list '", csv.cell(0, r),
					"'; a list is create, entry, step or exit");
			}
			++counts[list];
		}
		for (size_t l = 0; l < kTriggerCount; ++l)
		{
			store.where[l].resize(counts[l]);
			store.given[l].resize(counts[l]);
			store.sets[l].resize(counts[l]);
		}
		std::array<size_t, kTriggerCount>                placed{};
		std::array<std::vector<uint32_t>, kTriggerCount> rowLines;
		for (size_t r = 1; r < csv.rows(); ++r)
		{
			uint32_t const line = csv.line_of_row(r);
			size_t         list = 0;
			while (csv.cell(0, r) != kListNames[list])
			{
				++list;
			}
			size_t const c = placed[list]++;
			rowLines[list].push_back(line);
			std::string_view const from = csv.cell(1, r);
			auto here = [&](ErrorContext&& error)
			{
				return tl::make_unexpected(std::move(error).with(ClausePlace(*machineName, list, c, from)).with(Place(label, line)));
			};
			if (from.empty())
			{
				return here(ErrorContext("the clause names no table"));
			}
			auto where = ReadBinds(csv.cell(2, r), "where", tables, from, names, label, line, store.where[list][c]);
			if (!where)
			{
				return here(std::move(where.error()));
			}
			auto given = ReadBinds(csv.cell(3, r), "given", tables, from, names, label, line, store.given[list][c]);
			if (!given)
			{
				return here(std::move(given.error()));
			}
			auto sets = ReadSets(csv.cell(4, r), store.sets[list][c]);
			if (!sets)
			{
				return here(std::move(sets.error()));
			}
			uint32_t overrides = UINT32_MAX;
			std::string_view const overridesCell = csv.cell(6, r);
			if (!overridesCell.empty() && !ParseWhole(overridesCell, overrides))
			{
				return here(ErrorContext("overrides is '", overridesCell, "'; it is the position of the clause overridden"));
			}
			store.clauses[list].push_back(add_clause{from, store.where[list][c], store.given[list][c],
				store.sets[list][c], csv.cell(5, r), overrides});
		}

		out = create_machine{*machineName, store.state, store.inputs, store.clauses[0], store.clauses[1],
			store.clauses[2], store.clauses[3]};
		auto checked = CheckMachine(out, tables);
		if (!checked)
		{
			// CheckMachine names the clause (ClausePlace) or the machine in a
			// context layer of its own; match those layers, never the message.
			uint32_t where = 0;
			if (std::ranges::find(checked.error().context, whose) != checked.error().context.end())
			{
				where = header.line;
			}
			std::span<add_clause const> const lists[kTriggerCount] = {out.create, out.entry, out.step, out.exit};
			for (size_t l = 0; l < kTriggerCount; ++l)
			{
				for (size_t c = 0; c < lists[l].size(); ++c)
				{
					if (std::ranges::find(checked.error().context, ClausePlace(*machineName, l, c, lists[l][c].table))
						!= checked.error().context.end())
					{
						where = rowLines[l][c];
					}
				}
			}
			return tl::make_unexpected(std::move(checked.error()).with(where != 0 ? Place(label, where) : std::string(label)));
		}
	}
	return {};
}

}
