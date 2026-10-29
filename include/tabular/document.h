#ifndef TABULAR_DOCUMENT_H
#define TABULAR_DOCUMENT_H

#include "tabular/relation.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>

namespace tabular
{

struct DocumentStorage;
struct create_machine;   // tabular/machine.h, which a caller reading machines() includes

// One table's CSV body from the file, kept for loading. text views into the
// document's own copy of the file; first_line is the file line text starts on.
struct section_rows
{
	uint32_t         table = 0;
	std::string_view text;
	uint32_t         first_line = 1;
};

// Tables, the machines that read them, and the row text of the tables, from one
// .tabular file. The machine commands and the row text are views into the
// document, valid while it lives; tables can be moved out into a relation_store
// without disturbing them.
struct schema_document
{
	schema_document();
	~schema_document();
	schema_document(schema_document&&) noexcept;
	schema_document& operator=(schema_document&&) noexcept;

	schema tables;

	std::span<create_machine const> machines() const;
	// Only the tables that have a body, in file order.
	std::span<section_rows const>   rows() const;

	std::unique_ptr<DocumentStorage> storage;   // what machines() and rows() view
};

// A .tabular file. Bracket lines are s-expressions that declare:
//
//   [table spawn_points
//     (key package hash (domain rows))     # (key|column name type [(domain ..)] [fallthrough] [(input t)] [edge])
//     (column x double)
//     (input t double 0..1 cyclic)         # range and cyclic optional
//     (pick best)                          # none given: best; also all, none, (or out), (weighted w)
//     (order package unique)
//     (suffix command) (pivot headers cells)]
//   package,x                              # the CSV body under a bracket: its rows
//   meadow,1.5
//
//   [grid g (rows r int) (columns c int) (cells int)]
//   [machine grazer (state mode hash (members idle walk) (initial idle)) (input event hash)]
//   list,from,where,given,set,emit,overrides      # a machine's body: its clauses
//   step,grazer_brain,mode=state.mode kind=input.event,,mode=next,,
//
// Every table a file declares is read-only: a reload loads a new version.
// label names the source in errors; an error's message carries no position, and
// its context is `<label>:<line>`, as csv_text writes it. The result owns a
// copy of text. Each machine is checked against the tables before it is returned.
tl::expected<schema_document, ErrorContext> ReadDocument(std::string_view text, std::string_view label);

// A file of tables alone; refused when it declares machines or has rows, which a
// schema would drop, naming ReadDocument.
tl::expected<schema, ErrorContext> ReadSchema(std::string_view text, std::string_view label);

// Every table body in the file into store under owner, in file order: LoadCsv
// (pivoted when the table has a pivot), LoadGridCsv for a grid. Stops at the
// first refused body, whose error names its table. A caller treats a refused
// file as not loaded and retracts its owner, as any reload does.
tl::expected<void, ErrorContext> LoadDocumentRows(relation_store& store, schema_document const& document,
	uint64_t owner, std::function<tl::expected<closure, ErrorContext>(std::string_view)> const& condition);

}

#endif // TABULAR_DOCUMENT_H
