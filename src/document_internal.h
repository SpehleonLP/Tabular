#ifndef TABULAR_DOCUMENT_INTERNAL_H
#define TABULAR_DOCUMENT_INTERNAL_H

// What the file-format reader's parts share: the storage a schema_document owns,
// and the small readers for names, types and inputs. Not a public header.

#include "tabular/document.h"
#include "tabular/machine.h"
#include "csv_text.h"
#include "document_sections.h"
#include "Support/shared_array.hpp"

#include <array>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace tabular
{

// Everything a machine's commands view. Each array is sized once, from a count
// taken before it is filled, so no view into it moves.
struct machine_storage
{
	shared_array<shared_array<cell>>                                     members;   // per state column
	shared_array<shared_array<std::string_view>>                         member_names;
	shared_array<add_state>                                              state;
	shared_array<add_input>                                              inputs;
	std::array<shared_array<shared_array<clause_bind>>, kTriggerCount> where;     // per clause
	std::array<shared_array<shared_array<clause_bind>>, kTriggerCount> given;
	std::array<shared_array<shared_array<set_state>>, kTriggerCount>   sets;
	std::array<shared_array<add_clause>, kTriggerCount>                clauses;
};

struct DocumentStorage
{
	std::string                    text;    // the file, which every view below points into
	std::string                    label;
	shared_array<section_rows>     rows;
	// Per entry of rows: a table's body compiled to plain CSV, and the file
	// line of each of its records. A grid's entry is unused; its rows view text.
	shared_array<std::string>              compiled;
	shared_array<shared_array<uint32_t>>   compiled_lines;
	// Every table's name, by table number, so rows can be refused a store built
	// from another schema.
	shared_array<std::string>      table_names;
	shared_array<machine_storage>  machines;
	shared_array<create_machine>   commands;
	// Each machine's body, whose cells its clauses' names view.
	shared_array<detail::csv_text> bodies;
	// The sections the caller claimed, and every one's words end to end, which
	// each section's words views.
	shared_array<host_section>     hosts;
	shared_array<std::string_view> host_words;
	// Names that had escapes in the file and so are not a view of it; a deque
	// keeps each where it was put while the count is still unknown.
	std::deque<std::string>        owned;
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
