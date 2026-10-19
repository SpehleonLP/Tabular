#ifndef TABULAR_SCHEMA_YAML_H
#define TABULAR_SCHEMA_YAML_H

#include "tabular/relation.h"

#include <memory>
#include <span>
#include <string_view>

namespace tabular
{

// A YAML document of table definitions, built into a schema. label names the
// source in errors. The document is
//
//   tables:
//     - name: spawn_points
//       columns:
//         - {name: package, type: hash, key: true}
//         - {name: x, type: double}
//       order_by: {columns: [package], unique: true}   # optional
//       read_only: true                                # optional
//
// and type is one of hash, flags, int, double, condition, command. A document
// that also defines machines is read with DocumentFromYaml; this refuses one.
tl::expected<schema, ErrorContext> SchemaFromYaml(std::string_view text, std::string_view label);

struct DocumentStorage;
struct create_machine;   // tabular/machine.h, which a caller reading machines() includes

// Tables and the machines that read them, from one YAML document. The machine
// commands are views into the document, valid while it lives; tables can be
// moved out into a relation_store without disturbing them.
struct schema_document
{
	schema_document();
	~schema_document();
	schema_document(schema_document&&) noexcept;
	schema_document& operator=(schema_document&&) noexcept;

	schema tables;

	std::span<create_machine const> machines() const;

	std::unique_ptr<DocumentStorage> storage;   // what machines() views
};

// A document of tables, as SchemaFromYaml reads them, and of machines:
//
//   machines:
//     - name: grazer
//       state:
//         - {name: mode, type: hash, members: [idle, walk], initial: idle}
//       inputs:
//         - {name: energy, type: double, range: 0..1}
//         - {name: message, type: hash}
//       message: {type: message, arguments: []}  # optional; inputs Step fills
//       step:                                   # also create, entry, exit
//         - from:  grazer_brain                 # a best, or or weighted table
//           where: {mode: state.mode}           # every key column
//           given: {energy: input.energy}       # every table input
//           set:   {mode: next}                 # state column: value column
//           emit:  command                      # a command column
//
// A source is state.<column>, input.<name>, clause.<n>.<column> (the row an
// earlier clause of the same list picked) or {literal: <text>}, read as a cell
// of the target's type. Hash members and literals are hashed as CSV cells are,
// so within one machine hash 0 and two names with one hash are refused. A key
// the table declares `domain: message` is bound to input.<the message type>,
// and no other key may be. Each
// machine is checked against the tables (CheckMachine) before it is returned.
tl::expected<schema_document, ErrorContext> DocumentFromYaml(std::string_view text, std::string_view label);

}

#endif // TABULAR_SCHEMA_YAML_H
