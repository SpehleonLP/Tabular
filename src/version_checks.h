#ifndef TABULAR_VERSION_CHECKS_H
#define TABULAR_VERSION_CHECKS_H

// Internal to Tabular: what a best table's version must be before it is
// published, read off the run layout. Not a public header.

#include "Errors/errorcontext.h"
#include <expected/include/tl/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace tabular
{

struct load_refusal;
struct run_note;
struct table_def;
struct table_rows;

namespace detail
{

// The key tuples a version must be total over. The caller says, per key
// column, whether it is over its own rows or else which values its domain
// holds; ResolveDomain then lays the tuples out.
struct key_domains
{
	std::vector<uint8_t>              overRows;   // per key column
	std::vector<std::vector<int64_t>> values;     // per key column not over its rows: sorted, distinct
	// The domain: the distinct sub-tuples the rows name in the columns over
	// their rows, crossed with every other column's values. count tuples of one
	// value per key, end to end, sorted as Publish sorts rows (each key a
	// signed int64, key 0 most significant) and distinct.
	size_t               count = 0;
	std::vector<int64_t> tuples;
};

// Fills domains' tuples from its per-column sources and rows, the version the
// domain is read for. Work is proportional to the rows and the tuples.
void ResolveDomain(table_rows const& rows, key_domains& domains);

// A best table's candidate version, and which of its rows the change brought.
struct check_subject
{
	uint32_t                table = 0;
	table_def const&        def;
	table_rows const&       rows;
	// Per row: its index among the rows the change brought, or SIZE_MAX for a
	// row kept from the version before. A refusal names fresh rows by index.
	// Unread in a survey.
	std::span<size_t const> fresh;
	std::string_view        noun;     // a fresh row, for messages: "loaded row"
	std::string_view        change;   // the change, for messages: "this load"
	// The table this one's domain is read from, when the change is to that
	// table and not this one: a refusal then says this table is not total,
	// since the gap may be older than the change.
	std::string_view        source;
	// Never refuse, and check nothing but totality: every in-domain run that is
	// not total is noted instead (a change that is never refused).
	bool                    survey = false;
};

// Every key tuple in the domain has a total run; in those runs no two owners'
// rows with equal flags overlap and no row is dead. The first failure in key
// order is refused; refusal, if given, names the fresh rows at fault. A run
// outside the domain is an orphan, appended to report if given; a survey
// appends every run that is not total too, and refuses nothing.
tl::expected<void, ErrorContext> CheckBest(check_subject const& subject, key_domains const& domains,
	load_refusal* refusal, std::vector<run_note>* report);

}

}

#endif // TABULAR_VERSION_CHECKS_H
