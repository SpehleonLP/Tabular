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

// A section whose head the caller claimed (ReadDocument's host_heads): Tabular
// keeps it unread, so a host can put its own declarations in the same file
// without Tabular learning what they mean. Every view is into the document's own
// copy of the file; the host reads body with ReadCells (tabular/cell_rows.h).
struct host_section
{
	std::string_view                  head;        // "class", "messages", ...
	std::span<std::string_view const> words;       // the bracket's atoms after the head, as written
	std::string_view                  body;        // CSV text up to the next bracket; may be empty
	uint32_t                          line = 0;    // the bracket's line
	uint32_t                          body_line = 0;
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
	// The claimed sections, in file order.
	std::span<host_section const>   host_sections() const;

	std::unique_ptr<DocumentStorage> storage;   // what machines(), rows() and host_sections() view
};

// A .tabular file. A line starting with `[` declares a table, a grid or a
// machine with s-expressions, and the CSV lines beneath it are its rows (a
// machine's, its clauses). A line starting with `#` is a note. This exact text
// is read by Document.TheHeaderCommentsExampleIsAValidFile:
//
//   [table grazer_brain
//     (key mode hash (domain (members idle walk)))
//     (column next hash)
//     (order mode)
//     (suffix command)]
//   mode,next,command
//   idle,walk,go
//   walk,idle,stop
//
//   [grid g (rows r int) (columns c int) (cells int)]
//
//   [machine grazer (state mode hash (members idle walk) (initial idle))]
//   list,from,where,given,set,emit,overrides
//   step,grazer_brain,mode=state.mode,,mode=next,command,
//
// A table takes (key|column name type [(domain rows|(members ..)|(from t c))]
// [fallthrough] [(input t)] [edge]), (input name type [lo..hi] [cyclic]),
// (pick none|best|all), (pick or col), (pick weighted col), (order cols.. [unique]),
// (suffix name) and (pivot headers cells); with no (pick) it is best.
//
// Every table a file declares is read-only: a reload loads a new version.
// label names the source in errors; an error's message carries no position, and
// its context is `<label>:<line>`, as csv_text writes it. The result owns a
// copy of text. Each machine is checked against the tables before it is returned.
//
// A bracket whose head is one of host_heads is passed through as a host_section
// instead of being refused as unknown; its words after the head must be atoms.
tl::expected<schema_document, ErrorContext> ReadDocument(std::string_view text, std::string_view label,
	std::span<std::string_view const> host_heads = {});

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
