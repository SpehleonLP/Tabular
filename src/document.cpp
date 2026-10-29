#include "tabular/document.h"
#include "document_internal.h"
#include "csv_cells.h"

namespace tabular
{

schema_document::schema_document() = default;
schema_document::~schema_document() = default;
schema_document::schema_document(schema_document&&) noexcept = default;
schema_document& schema_document::operator=(schema_document&&) noexcept = default;

std::span<create_machine const> schema_document::machines() const
{
	if (!storage)
	{
		return {};
	}
	return storage->commands;
}

std::span<section_rows const> schema_document::rows() const
{
	if (!storage)
	{
		return {};
	}
	return storage->rows;
}

tl::expected<schema_document, ErrorContext> ReadDocument(std::string_view text, std::string_view label)
{
	auto held = std::make_unique<DocumentStorage>();
	// Every view the document hands out points into this copy, so the caller's
	// buffer need not outlive the read.
	held->text  = std::string(text);
	held->label = std::string(label);

	auto sections = detail::SplitSections(held->text, held->label);
	if (!sections)
	{
		return tl::make_unexpected(std::move(sections.error()));
	}
	auto tables = detail::ReadTables(*sections, held->label, *held);
	if (!tables)
	{
		return tl::make_unexpected(std::move(tables.error()));
	}
	auto machines = detail::ReadMachines(*sections, held->label, *tables, *held);
	if (!machines)
	{
		return tl::make_unexpected(std::move(machines.error()));
	}

	schema_document document;
	document.tables  = std::move(*tables);
	document.storage = std::move(held);
	return document;
}

tl::expected<schema, ErrorContext> ReadSchema(std::string_view text, std::string_view label)
{
	auto document = ReadDocument(text, label);
	if (!document)
	{
		return tl::make_unexpected(std::move(document.error()));
	}
	if (!document->machines().empty())
	{
		return tl::make_unexpected(ErrorContext("the file declares machines, which a schema alone would drop; ",
			"read it with ReadDocument").with(std::string(label)));
	}
	if (!document->rows().empty())
	{
		return tl::make_unexpected(ErrorContext("the file has rows, which a schema alone would drop; ",
			"read it with ReadDocument").with(std::string(label)));
	}
	return std::move(document->tables);
}

tl::expected<void, ErrorContext> LoadDocumentRows(relation_store& store, schema_document const& document,
	uint64_t owner, std::function<tl::expected<closure, ErrorContext>(std::string_view)> const& condition)
{
	if (!document.storage)
	{
		return {};
	}
	// The store's schema, not the document's: the tables move into a store.
	std::span<table_def const> const tables = store.definition().tables();
	for (section_rows const& body : document.storage->rows)
	{
		// The rows were read for the document's own schema; a store built from
		// another would take them into the wrong table, or past its end.
		std::string const& declared = document.storage->tableNames[body.table];
		if (body.table >= tables.size() || tables[body.table].name != declared)
		{
			return tl::make_unexpected(ErrorContext("the document's table '", declared, "' (number ", body.table,
				") is not table ", body.table, " of the store; the rows were read for another schema")
				.with(document.storage->label));
		}
		table_def const& def = tables[body.table];
		tl::expected<void, ErrorContext> loaded;
		if (def.is_grid)
		{
			loaded = LoadGridCsv(store, csv_grid{body.table, owner, body.text, document.storage->label, body.first_line});
		}
		else
		{
			csv_rows command{body.table, owner, body.text, document.storage->label, body.first_line, condition};
			command.pivoted = def.pivot_headers != UINT32_MAX;
			loaded          = LoadCsv(store, command);
		}
		if (!loaded)
		{
			return tl::make_unexpected(std::move(loaded.error()).with("table '", def.name, "'"));
		}
	}
	return {};
}

}
