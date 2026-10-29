#ifndef TABULAR_DOCUMENT_INTERNAL_H
#define TABULAR_DOCUMENT_INTERNAL_H

// What the file-format reader's parts share: the storage a schema_document owns,
// and the small readers for names, types and inputs. Not a public header.

#include "tabular/document.h"
#include "tabular/machine.h"
#include "csv_text.h"
#include "document_sections.h"

#include <array>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tabular
{

// Everything a machine's commands view, sized before any view into it is taken,
// so nothing moves.
struct machine_storage
{
	std::vector<std::vector<cell>>                                   members;   // per state column
	std::vector<add_state>                                           state;
	std::vector<add_input>                                           inputs;
	std::array<std::vector<std::vector<clause_bind>>, kTriggerCount> where;   // per clause
	std::array<std::vector<std::vector<clause_bind>>, kTriggerCount> given;
	std::array<std::vector<std::vector<set_state>>, kTriggerCount>   sets;
	std::array<std::vector<add_clause>, kTriggerCount>               clauses;
};

struct DocumentStorage
{
	std::string                  text;    // the file, which every view below points into
	std::string                  label;
	std::vector<section_rows>    rows;
	// Every table's name, by table number, so rows can be refused a store built
	// from another schema.
	std::vector<std::string>     tableNames;
	std::vector<machine_storage> machines;
	std::vector<create_machine>  commands;
	// A machine body's cells, which its clauses' names view; a deque keeps each
	// body where it was put.
	std::deque<detail::csv_text> bodies;
	// Names that had escapes in the file and so are not a view of it.
	std::deque<std::string>      owned;
	// What an older reader keeps its views alive with.
	std::shared_ptr<void>        keepalive;
};

namespace detail
{

inline std::string Place(std::string_view label, uint32_t line)
{
	return cc(std::string(), label, ":", line);
}

inline bool IsAtom(sexpr const& item, std::string_view text)
{
	return item.what == sexpr::kind::Atom && item.text == text;
}

// An error whose message says what is wrong and whose context says where, as
// csv_text writes it.
template<typename... Args>
tl::unexpected<ErrorContext> Refuse(std::string_view label, uint32_t line, Args const&... message)
{
	return tl::make_unexpected(ErrorContext(cc(std::string(), message...)).with(Place(label, line)));
}

// An atom or a string as a name; a list is not one. Escapes in a string are
// undone into the storage.
tl::expected<std::string_view, ErrorContext> NameOf(sexpr const& item, DocumentStorage& storage,
	std::string_view label, std::string_view what);

// A name a declaration introduces: a bare word, since a clause cell is split on
// whitespace and cannot refer to a quoted name or one holding a space.
tl::expected<std::string_view, ErrorContext> DeclaredName(sexpr const& item, DocumentStorage& storage,
	std::string_view label, std::string_view what);

tl::expected<column_type, ErrorContext> TypeOf(sexpr const& item, std::string_view label);

// `(input name type [range] [cyclic])`, of a table or a machine. Presence, not
// emptiness, decides whether it is ranged. whose names the owner in a refusal
// (" of table 't'"), or nothing.
tl::expected<add_input, ErrorContext> ReadInputOption(sexpr const& option, DocumentStorage& storage,
	std::string_view label, std::string const& whose);

// Tables and grids, in file order, built into a schema; their bodies go into
// storage.rows. Refuses a head that is not table, grid or machine.
tl::expected<schema, ErrorContext> ReadTables(std::span<section const> sections, std::string_view label,
	DocumentStorage& storage);

// Machines into storage.commands, each checked against tables.
tl::expected<void, ErrorContext> ReadMachines(std::span<section const> sections, std::string_view label,
	schema const& tables, DocumentStorage& storage);

}

}

#endif // TABULAR_DOCUMENT_INTERNAL_H
