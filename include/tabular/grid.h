#ifndef TABULAR_GRID_H
#define TABULAR_GRID_H

#include "tabular/relation.h"

#include <atomic>
#include <memory>

namespace tabular
{

// One version of a grid: a dense 2D array of one cell type, built whole by
// LoadGrid and never modified, like a TableVersion. Keys are looked up by
// binary search; a hot caller resolves RowOf / ColumnOf once and reads by
// index. The index overloads assert their bounds: indices come from RowOf and
// ColumnOf, and asserts are live in release, so a bad one aborts rather than
// reading past the array.
class GridVersion
{
public:
	~GridVersion();
	GridVersion(GridVersion const&)            = delete;
	GridVersion& operator=(GridVersion const&) = delete;

	table_def const& definition() const;
	uint32_t Rows() const;
	uint32_t Columns() const;
	uint64_t Owner() const;
	uint64_t Generation() const;

	tl::expected<uint32_t, ErrorContext>       RowOf(cell key) const;
	tl::expected<uint32_t, ErrorContext>       ColumnOf(cell key) const;
	tl::expected<cell, ErrorContext>           At(cell row_key, cell column_key) const;
	tl::expected<counted_string, ErrorContext> StringAt(cell row_key, cell column_key) const;
	cell                                       At(uint32_t row, uint32_t column) const;
	counted_string const&                      StringAt(uint32_t row, uint32_t column) const;

	void AddRef() const { _refs.fetch_add(1, std::memory_order_relaxed); }
	void Release() const;

private:
	friend class relation_store;
	friend struct version_access;
	struct Data;
	explicit GridVersion(std::unique_ptr<Data>&& data);

	std::unique_ptr<Data const> _data;
	mutable std::atomic<int>    _refs{1};
};

}

#endif // TABULAR_GRID_H
