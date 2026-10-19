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

struct domain_def;
struct load_refusal;
struct run_note;
struct table_def;
struct table_rows;

namespace detail
{

struct kept_runs;
struct row_span;

// The key tuples a version must be total over. The caller says, per key
// column, whether it is over its own rows or else which values its domain
// holds; ResolveDomain then lays the tuples out.
struct key_domains
{
	std::vector<uint8_t>              overRows;   // per key column
	std::vector<std::vector<int64_t>> values;     // per key column not over its rows: sorted, distinct
	// The message key, if any: its values are {0}, the blank, and ResolveDomain
	// adds every run the rows name whose other keys are in the domain.
	uint32_t                          messageKey = UINT32_MAX;
	// The domain: the distinct sub-tuples the rows name in the columns over
	// their rows, crossed with every other column's values (and the message
	// key's runs added). count tuples of one value per key, end to end, sorted
	// as Publish sorts rows (each key a signed int64, key 0 most significant)
	// and distinct.
	size_t               count = 0;
	std::vector<int64_t> tuples;

	// keys columns, none yet over its rows or a message key.
	void Reset(size_t keys)
	{
		overRows.assign(keys, 0);
		values.assign(keys, {});
		messageKey = UINT32_MAX;
	}
};

// Fills domains' tuples from its per-column sources and rows, the version the
// domain is read for. Work is proportional to the rows and the tuples.
void ResolveDomain(table_rows const& rows, key_domains& domains);

// The distinct values a value column holds across rows, sorted: a domain
// taken from another table's column.
void ColumnValues(table_rows const& rows, uint32_t column, std::vector<int64_t>& out);

// Key column k of domains as declared: its members, the values of another
// table's column (foreignDef and foreign, that table and its rows as the
// caller reads them; read only for a Column domain), over its own rows, or a
// message key. A column that is the other table's message key gives its
// types without its 0: that is no message, not a type the dependent key
// names. Load and a machine's Prepare both start a key from here; Prepare
// then overrides the keys its sources close.
void DeclaredDomain(domain_def const& declared, table_def const* foreignDef, table_rows const* foreign, uint32_t k,
	key_domains& domains);

// A best table's layout axes: per axis, the input it reads, one per ranged
// input a band reads, in input order.
void LayoutAxes(table_def const& def, std::vector<uint32_t>& out);

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
	// since the gap may be older than the change. With change and source both
	// empty there is no change: a standing version is proved over a domain of
	// someone else's (a machine's clause), and a gap is named by its first row.
	std::string_view        source;
	// Never refuse, and check nothing but totality: every in-domain run that is
	// not total is noted instead (a change that is never refused).
	bool                    survey = false;
	// Whose domain a tuple with no run is in, for messages.
	std::string_view        domain = "the table's domain";
};

// Every key tuple in the domain has a total run; in those runs no two owners'
// rows with equal flags overlap and no row is dead. The first failure in key
// order is refused; refusal, if given, names the fresh rows at fault. A run
// outside the domain is an orphan, appended to report if given; a survey
// appends every run that is not total too, and refuses nothing. Load and a
// machine's Prepare differ only in the domain they pass: keep, if given,
// receives every in-domain run's layout, one per tuple in domain order, with
// the rows named by their numbers in subject.rows.
tl::expected<void, ErrorContext> CheckBest(check_subject const& subject, key_domains const& domains,
	load_refusal* refusal, std::vector<run_note>* report, kept_runs* keep = nullptr);

// A table picked without a layout (an or or a weighted table), over a
// machine clause's domain: runs receives each in-domain run's version rows,
// one per tuple in domain order, a tuple no run has as an empty span. A run
// outside the domain is an orphan, appended to report if given. With
// everyTuple (a weighted draw needs a row) a tuple no run has is refused.
tl::expected<void, ErrorContext> CheckRuns(check_subject const& subject, key_domains const& domains, bool everyTuple,
	std::vector<run_note>* report, std::vector<row_span>& runs);

}

}

#endif // TABULAR_VERSION_CHECKS_H
