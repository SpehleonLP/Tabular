#include "tabular/document.h"
#include "document_internal.h"
#include "csv_cells.h"

#include <algorithm>

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

std::span<host_section const> schema_document::host_sections() const
{
	if (!storage)
	{
		return {};
	}
	return storage->hosts;
}

namespace
{

bool Claimed(detail::section const& section, std::span<std::string_view const> host_heads)
{
	detail::sexpr const& head = section.header.items[0];
	if (head.what != detail::sexpr::kind::Atom)
	{
		return false;
	}
	return std::find(host_heads.begin(), host_heads.end(), head.text) != host_heads.end();
}

// Takes the claimed sections out of sections into storage, unread, and leaves
// the rest for the table and machine readers.
tl::expected<void, ErrorContext> TakeHostSections(std::vector<detail::section>& sections,
	std::span<std::string_view const> host_heads, std::string_view label, DocumentStorage& storage)
{
	size_t hostCount = 0;
	size_t wordCount = 0;
	for (detail::section const& section : sections)
	{
		if (Claimed(section, host_heads))
		{
			++hostCount;
			wordCount += section.header.items.size() - 1;
		}
	}
	if (hostCount == 0)
	{
		return {};
	}
	storage.hosts      = shared_array<host_section>(hostCount);
	storage.host_words = shared_array<std::string_view>(wordCount);

	std::vector<detail::section> unclaimed;
	unclaimed.reserve(sections.size() - hostCount);
	uint32_t host = 0;
	uint32_t word = 0;
	for (detail::section& section : sections)
	{
		if (!Claimed(section, host_heads))
		{
			unclaimed.push_back(std::move(section));
			continue;
		}
		std::span<detail::sexpr const> const items = section.header.items;
		uint32_t const first = word;
		for (detail::sexpr const& item : items.subspan(1))
		{
			// A host reads a word as one name; a list or a quoted string would
			// need Tabular to say how it reads, which it does not.
			if (item.what != detail::sexpr::kind::Atom)
			{
				return detail::Refuse(label, item.line, "[", items[0].text, "] takes bare words after its head, not ",
					item.what == detail::sexpr::kind::List ? "a list" : "a quoted string");
			}
			storage.host_words[word++] = item.text;
		}
		host_section& out = storage.hosts[host++];
		out.head      = items[0].text;
		out.words     = std::span<std::string_view const>(storage.host_words.begin() + first, word - first);
		out.body      = section.body;
		out.line      = section.header.line;
		out.body_line = section.body_line;
	}
	sections = std::move(unclaimed);
	return {};
}

}

tl::expected<schema_document, ErrorContext> ReadDocument(std::string_view text, std::string_view label,
	std::span<std::string_view const> host_heads)
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
	auto hosts = TakeHostSections(*sections, host_heads, held->label, *held);
	if (!hosts)
	{
		return tl::make_unexpected(std::move(hosts.error()));
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
		std::string const& declared = document.storage->table_names[body.table];
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
