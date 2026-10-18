#include "version_checks.h"

#include "float_text.h"
#include "layout.h"
#include "relation_internal.h"

#include "Support/strings.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <string>

namespace tabular::detail
{

namespace
{

// Signed int64 order over tuples of width values, key 0 most significant: the
// order Publish sorts rows by.
bool TupleLess(int64_t const* a, int64_t const* b, size_t width)
{
	return std::lexicographical_compare(a, a + width, b, b + width);
}

// Sorts count tuples of width values laid end to end, and drops repeats when
// distinct is set; returns how many are left.
size_t SortTuples(std::vector<int64_t>& tuples, size_t width, size_t count, bool distinct)
{
	std::vector<size_t> order(count);
	for (size_t t = 0; t < count; ++t)
	{
		order[t] = t;
	}
	int64_t const* base = tuples.data();
	std::sort(order.begin(), order.end(),
		[&](size_t a, size_t b) { return TupleLess(base + a * width, base + b * width, width); });
	std::vector<int64_t> sorted;
	sorted.reserve(count * width);
	size_t kept = 0;
	for (size_t t : order)
	{
		int64_t const* tuple = base + t * width;
		if (distinct && kept > 0 && std::equal(tuple, tuple + width, sorted.end() - static_cast<std::ptrdiff_t>(width)))
		{
			continue;
		}
		sorted.insert(sorted.end(), tuple, tuple + width);
		++kept;
	}
	tuples.swap(sorted);
	return kept;
}

// Calls onRun(first, last, tuple, inDomain) for every key run, and with
// first == last for every tuple of the domain no run has, in key order, which
// is row order: the keys lead the order. A run that sorts before the next
// tuple matched none of the earlier ones, so it is outside the domain. Stops
// at the first error a callback returns.
template<typename OnRun>
tl::expected<void, ErrorContext> WalkDomain(table_rows const& rows, uint32_t keys, key_domains const& domains,
	OnRun&& onRun)
{
	size_t const n = rows.RowCount();
	auto keyAt = [&](size_t r, uint32_t k) { return rows.cells[r * rows.valueCount + rows.orderSlots[k]].i; };
	auto runEnd = [&](size_t r)
	{
		size_t e = r + 1;
		while (e < n)
		{
			bool same = true;
			for (uint32_t k = 0; k < keys && same; ++k)
			{
				same = keyAt(e, k) == keyAt(r, k);
			}
			if (!same)
			{
				break;
			}
			++e;
		}
		return e;
	};
	auto compareRun = [&](size_t r, std::span<int64_t const> tuple)
	{
		for (uint32_t k = 0; k < keys; ++k)
		{
			if (keyAt(r, k) != tuple[k])
			{
				return keyAt(r, k) < tuple[k] ? -1 : 1;
			}
		}
		return 0;
	};
	std::vector<int64_t> runKey(keys);
	size_t               r = 0;
	auto orphan = [&]() -> tl::expected<void, ErrorContext>
	{
		size_t const e = runEnd(r);
		for (uint32_t k = 0; k < keys; ++k)
		{
			runKey[k] = keyAt(r, k);
		}
		auto ok = onRun(r, e, std::span<int64_t const>(runKey), false);
		r = e;
		return ok;
	};

	for (size_t t = 0; t < domains.count; ++t)
	{
		std::span<int64_t const> const tuple(domains.tuples.data() + t * keys, keys);
		while (r < n && compareRun(r, tuple) < 0)
		{
			auto ok = orphan();
			if (!ok)
			{
				return ok;
			}
		}
		size_t const e  = r < n && compareRun(r, tuple) == 0 ? runEnd(r) : r;
		auto         ok = onRun(r, e, tuple, true);
		if (!ok)
		{
			return ok;
		}
		r = e;
	}
	while (r < n)
	{
		auto ok = orphan();
		if (!ok)
		{
			return ok;
		}
	}
	return {};
}

std::string TupleText(std::span<int64_t const> tuple)
{
	std::string text;
	for (int64_t v : tuple)
	{
		text = cc(text, text.empty() ? "" : ", ", v);
	}
	return text;
}

// One best table's rows, read as the layout needs them: a box per row over
// the ranged inputs its bands read, its flags and whether it is scripted.
class run_reader
{
public:
	// Each run is laid into keep when given, after the runs before it, so a
	// machine keeps them all; otherwise into a layout of the reader's own that
	// holds one run at a time.
	run_reader(table_def const& def, table_rows const& rows, kept_runs* keep) : _def(def), _rows(rows), _keep(keep)
	{
		LayoutAxes(def, _axes);
		for (uint32_t i : _axes)
		{
			_domain.push_back({def.inputs[i].lo, def.inputs[i].hi});
		}
	}

	std::span<interval const> Domain() const { return _domain; }
	size_t                    Dims() const { return _axes.size(); }
	bool                      Ranked() const { return _def.ranking != UINT32_MAX; }

	uint32_t FlagsOf(size_t r) const
	{
		return Ranked() ? static_cast<uint32_t>(Cells(r)[_rows.slot[_def.ranking]].i) : 0u;
	}

	// At most one condition column, so a row's condition is its first.
	bool Scripted(size_t r) const
	{
		return _rows.conditionCount != 0 && _rows.conditions[r * _rows.conditionCount]->get().function != nullptr;
	}

	// The row's box: each axis's range, cut down by every band on that input.
	void BoxOf(size_t r, std::vector<interval>& out) const
	{
		std::span<cell const> cells = Cells(r);
		for (size_t a = 0; a < _axes.size(); ++a)
		{
			interval box = _domain[a];
			for (uint32_t c = 0; c < _def.columns.size(); ++c)
			{
				if (_def.columns[c].type == column_type::Band && _def.columns[c].input == _axes[a])
				{
					box.lo = std::max(box.lo, cells[_rows.slot[c]].v[0]);
					box.hi = std::min(box.hi, cells[_rows.slot[c]].v[1]);
				}
			}
			out.push_back(box);
		}
	}

	// Lays out rows [first, last), naming them by their row numbers in rows.
	void Lay(size_t first, size_t last)
	{
		_boxes.clear();
		_laid.clear();
		for (size_t r = first; r < last; ++r)
		{
			BoxOf(r, _boxes);
			_laid.push_back(layout_row{FlagsOf(r), Scripted(r)});
		}
		if (!_keep)
		{
			Clear(_layout);
		}
		_span = LayOut(_domain, _boxes, _laid, static_cast<uint32_t>(first), Target(), _scratch);
	}

	run_layout const&         Layout() const { return _keep ? _keep->layout : _layout; }
	run_span                  Span() const { return _span; }
	std::span<interval const> Boxes() const { return _boxes; }

	// The lowest input the default rows (flags 0, which an unranked table's
	// rows all have) leave without a row that always holds, if any.
	std::optional<std::vector<float>> Hole() const
	{
		return FirstOpen(Layout(), _domain, FindGroup(Layout(), _span, 0));
	}

	std::string WhereText(std::vector<float> const& point) const
	{
		std::string where;
		for (size_t a = 0; a < _axes.size(); ++a)
		{
			where = cc(where, a == 0 ? "" : ", ", _def.inputs[_axes[a]].name, " ", FloatText(point[a]));
		}
		return where;
	}

	std::string KeyText(size_t r) const
	{
		std::string key;
		for (uint32_t k = 0; k < _def.key_count; ++k)
		{
			key = cc(key, key.empty() ? "" : ", ", Cells(r)[_rows.orderSlots[k]].i);
		}
		return key;
	}

	// A kept row, described by what its author can find in the owner's file:
	// key values, ranking flags and band values.
	std::string SurvivorText(size_t r) const
	{
		std::vector<std::string> parts;
		if (_def.key_count > 0)
		{
			parts.push_back(cc(std::string("key ("), KeyText(r), ")"));
		}
		if (Ranked())
		{
			parts.push_back(cc(std::string("flags "), FlagsOf(r)));
		}
		for (uint32_t c = 0; c < _def.columns.size(); ++c)
		{
			if (_def.columns[c].type == column_type::Band)
			{
				cell const& band = Cells(r)[_rows.slot[c]];
				parts.push_back(cc(_def.columns[c].name, " ", FloatText(band.v[0]), "..", FloatText(band.v[1])));
			}
		}
		std::string text = cc(std::string("owner "), _rows.owners[r], "'s row");
		for (size_t p = 0; p < parts.size(); ++p)
		{
			text = cc(text, p == 0 ? " with " : ", ", parts[p]);
		}
		return text;
	}

	char const* NoDefault() const
	{
		return Ranked() ? "no row with no flags and no script condition" : "no row with no script condition";
	}

	// A dependent's gap may be older than the change that found it, so its
	// refusal says the dependent is not total, not that the change broke it.
	ErrorContext NotTotal(std::string const& key, std::optional<std::vector<float>> const& hole,
		std::string const& why, std::string_view source) const
	{
		ErrorContext error = _axes.empty()
			? ErrorContext("table '", _def.name, "' key (", key, ") has ", NoDefault(), why)
			: ErrorContext("table '", _def.name, "' key (", key, "): ", WhereText(*hole), " has ", NoDefault(), why);
		if (source.empty())
		{
			return error;
		}
		return std::move(error).with("table '", _def.name, "' takes a key domain from table '", source,
			"', and is not total");
	}

private:
	run_layout& Target() { return _keep ? _keep->layout : _layout; }

	std::span<cell const> Cells(size_t r) const
	{
		return std::span<cell const>(_rows.cells).subspan(r * _rows.valueCount, _rows.valueCount);
	}

	table_def const&        _def;
	table_rows const&       _rows;
	std::vector<uint32_t>   _axes;
	std::vector<interval>   _domain;
	std::vector<interval>   _boxes;
	std::vector<layout_row> _laid;
	kept_runs*              _keep = nullptr;
	run_layout              _layout;
	run_span                _span;
	layout_scratch          _scratch;
};

bool Meets(interval const* a, interval const* b, size_t dims)
{
	bool meets = true;
	for (size_t x = 0; x < dims; ++x)
	{
		meets = meets && std::max(a[x].lo, b[x].lo) < std::min(a[x].hi, b[x].hi);
	}
	return meets;
}


std::vector<cell> KeyCells(std::span<int64_t const> tuple)
{
	std::vector<cell> key;
	key.reserve(tuple.size());
	for (int64_t v : tuple)
	{
		key.push_back(cell::Integer(v));
	}
	return key;
}

}

void ColumnValues(table_rows const& rows, uint32_t column, std::vector<int64_t>& out)
{
	out.clear();
	for (size_t r = 0; r < rows.RowCount(); ++r)
	{
		out.push_back(rows.cells[r * rows.valueCount + rows.slot[column]].i);
	}
	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());
}

void DeclaredDomain(domain_def const& declared, table_rows const* foreign, uint32_t k, key_domains& domains)
{
	std::vector<int64_t>& values = domains.values[k];
	values.clear();
	domains.overRows[k] = 0;
	switch (declared.kind)
	{
	case domain_kind::Members:
		values.assign(declared.members.begin(), declared.members.end());
		break;
	case domain_kind::Rows:
		domains.overRows[k] = 1;
		break;
	case domain_kind::Column:
		ColumnValues(*foreign, declared.column, values);
		break;
	case domain_kind::None:
		break;   // the schema refuses a best table's key with no domain
	}
}

void LayoutAxes(table_def const& def, std::vector<uint32_t>& out)
{
	out.clear();
	for (uint32_t i = 0; i < def.inputs.size(); ++i)
	{
		for (column_def const& col : def.columns)
		{
			if (col.type == column_type::Band && col.input == i)
			{
				out.push_back(i);
				break;
			}
		}
	}
}

void ResolveDomain(table_rows const& rows, key_domains& domains)
{
	size_t const keys = domains.overRows.size();
	std::vector<size_t> rowKeys;     // the key columns over their rows
	std::vector<size_t> otherKeys;   // the rest, each with its declared values
	for (size_t k = 0; k < keys; ++k)
	{
		(domains.overRows[k] ? rowKeys : otherKeys).push_back(k);
	}

	// The rows' joint sub-tuples in the columns over their rows. Rows sort by
	// every key, so these are sorted only when those columns lead the keys.
	std::vector<int64_t> joint;
	size_t               jointCount = 1;   // no column over its rows: the one empty sub-tuple
	if (!rowKeys.empty())
	{
		joint.reserve(rows.RowCount() * rowKeys.size());
		for (size_t r = 0; r < rows.RowCount(); ++r)
		{
			for (size_t k : rowKeys)
			{
				joint.push_back(rows.cells[r * rows.valueCount + rows.orderSlots[k]].i);
			}
		}
		jointCount = SortTuples(joint, rowKeys.size(), rows.RowCount(), true);
	}

	size_t count = jointCount;
	for (size_t k : otherKeys)
	{
		count *= domains.values[k].size();
	}
	domains.count = count;
	domains.tuples.assign(count * keys, 0);
	// Each joint sub-tuple crossed with the other columns' values, as an
	// odometer over those columns, the last one fastest.
	std::vector<size_t> digit(otherKeys.size(), 0);
	for (size_t t = 0; t < count; ++t)
	{
		int64_t* tuple = domains.tuples.data() + t * keys;
		size_t const p = t / (count / jointCount);
		for (size_t j = 0; j < rowKeys.size(); ++j)
		{
			tuple[rowKeys[j]] = joint[p * rowKeys.size() + j];
		}
		for (size_t o = 0; o < otherKeys.size(); ++o)
		{
			tuple[otherKeys[o]] = domains.values[otherKeys[o]][digit[o]];
		}
		for (size_t o = otherKeys.size(); o > 0; --o)
		{
			if (++digit[o - 1] < domains.values[otherKeys[o - 1]].size())
			{
				break;
			}
			digit[o - 1] = 0;
		}
	}
	// In key order only when the columns over their rows lead the keys, and
	// the walk needs key order.
	SortTuples(domains.tuples, keys, count, false);
}

tl::expected<void, ErrorContext> CheckBest(check_subject const& subject, key_domains const& domains,
	load_refusal* refusal, std::vector<run_note>* report, kept_runs* keep)
{
	table_def const&  def  = subject.def;
	table_rows const& rows = subject.rows;
	run_reader        reader(def, rows, keep);
	size_t const      dims = reader.Dims();

	auto isFresh = [&](size_t r) { return subject.fresh[r] != SIZE_MAX; };
	auto label = [&](size_t r)
	{
		return isFresh(r) ? cc(std::string(subject.noun), " ", subject.fresh[r]) : reader.SurvivorText(r);
	};
	auto refuseRow = [&](size_t row, size_t coveredBy)
	{
		if (refusal)
		{
			refusal->row        = row;
			refusal->covered_by = coveredBy;
		}
	};

	// An in-domain tuple no run has is a run of no rows: its layout has no
	// groups, so its hole is the low corner of the band inputs.
	auto onRun = [&](size_t start, size_t end, std::span<int64_t const> key, bool inDomain)
		-> tl::expected<void, ErrorContext>
	{
		if (!inDomain)
		{
			if (report)
			{
				report->push_back(run_note{subject.table, run_status::Orphan, KeyCells(key)});
			}
			return {};
		}
		reader.Lay(start, end);
		if (subject.survey)
		{
			if (report && reader.Hole())
			{
				report->push_back(run_note{subject.table, run_status::NotTotal, KeyCells(key)});
			}
			return {};
		}
		size_t firstFresh = SIZE_MAX;
		for (size_t i = start; i < end && firstFresh == SIZE_MAX; ++i)
		{
			firstFresh = subject.fresh[i];
		}
		std::span<interval const> boxes = reader.Boxes();

		// Two owners' rows with equal flags may not overlap: that is two
		// contributors claiming the same inputs, decided only by an order
		// among owners. Checked before the dead rows, which would report the
		// same pair as a shadowed row. One owner's rows overlapping is
		// priority by row order. Every row counts, scripted or not, and every
		// pair: a run that just entered the domain was never checked before.
		for (size_t b = start + 1; b < end; ++b)
		{
			for (size_t a = start; a < b; ++a)
			{
				if (rows.owners[a] == rows.owners[b] || reader.FlagsOf(a) != reader.FlagsOf(b)
					|| !Meets(&boxes[(a - start) * dims], &boxes[(b - start) * dims], dims))
				{
					continue;
				}
				refuseRow(isFresh(b) ? subject.fresh[b] : subject.fresh[a], SIZE_MAX);
				auto claimant = [&](size_t s)
				{
					return isFresh(s) ? cc(std::string("owner "), rows.owners[s], "'s ", subject.noun, " ", subject.fresh[s])
					                  : reader.SurvivorText(s);
				};
				return tl::make_unexpected(ErrorContext("table '", def.name, "' key (", reader.KeyText(start), "): ",
					claimant(a), " and ", claimant(b), reader.Ranked() ? " have equal flags and overlap" : " overlap",
					"; two owners may not claim the same inputs"));
			}
		}

		// Totality: every list of the default group ends in a row with no
		// script. A ranked row never closes one: an input with no flags skips
		// every ranked group.
		auto hole = reader.Hole();
		if (hole)
		{
			refuseRow(firstFresh, SIZE_MAX);
			// A run the change brings no rows to was stranded by an earlier bare
			// retract, or by this change vacating the owner's own rows from it,
			// or it has just entered the domain; name a survivor so the author
			// can find it.
			std::string why;
			if (start == end)
			{
				why = cc(std::string("; no row has this key, and the key is in "), subject.domain);
			}
			else if (!subject.source.empty() || subject.change.empty())
			{
				why = cc(std::string("; its first row is "), reader.SurvivorText(start));
			}
			else if (firstFresh == SIZE_MAX)
			{
				why = cc(std::string("; "), subject.change,
					" brings the key no row, and the version it would publish is not total (its first row is ",
					reader.SurvivorText(start), ")");
			}
			return tl::make_unexpected(reader.NotTotal(TupleText(key), hole, why, subject.source));
		}

		// Dead rows: a row in no list of its group is never tried before a row
		// that always holds, for any input. Within a group rows are tried in
		// row order (owner id, then index within the owner's change); across
		// groups the input flags decide, but a row's own flags make it a full
		// match outranking every other group, so only its own group can
		// shadow it.
		run_layout const& layout = reader.Layout();
		for (size_t b = start; b < end; ++b)
		{
			if (layout.listed[b - start])
			{
				continue;
			}
			interval const* own = &boxes[(b - start) * dims];
			bool emptyBox = false;
			for (size_t a = 0; a < dims; ++a)
			{
				emptyBox = emptyBox || !(own[a].lo < own[a].hi);
			}
			if (emptyBox)
			{
				refuseRow(subject.fresh[b], SIZE_MAX);
				return tl::make_unexpected(ErrorContext("table '", def.name, "' key (", reader.KeyText(b), "): ",
					label(b), " is never picked; its bands hold for no input"));
			}
			// The rows that take its place: those tried before it, with no script
			// and the same flags, whose box meets its own. A row tried earlier on
			// other inputs only does not.
			std::vector<size_t> covering;
			for (size_t a = start; a < b; ++a)
			{
				if (!reader.Scripted(a) && reader.FlagsOf(a) == reader.FlagsOf(b)
					&& Meets(&boxes[(a - start) * dims], own, dims))
				{
					covering.push_back(a);
				}
			}
			refuseRow(subject.fresh[b], covering.empty() ? SIZE_MAX : subject.fresh[covering.back()]);
			bool const one      = covering.size() == 1;
			bool       allFresh = true;
			std::string names;
			for (size_t a : covering)
			{
				allFresh = allFresh && isFresh(a);
			}
			for (size_t a : covering)
			{
				names = cc(names, names.empty() ? "" : ", ", allFresh ? cc(std::string(), subject.fresh[a]) : label(a));
			}
			std::string const by = allFresh
				? cc(std::string(subject.noun), one ? " " : "s ", names, one ? " covers it" : " cover it")
				: cc(names, one ? " covers it" : " cover it");
			return tl::make_unexpected(ErrorContext("table '", def.name, "' key (", reader.KeyText(b), "): ",
				label(b), " is never picked; ", by));
		}
		if (keep)
		{
			keep->runs.push_back(reader.Span());
		}
		return {};
	};

	return WalkDomain(rows, def.key_count, domains, onRun);
}

tl::expected<void, ErrorContext> CheckRuns(check_subject const& subject, key_domains const& domains, bool everyTuple,
	std::vector<run_note>* report, std::vector<row_span>& runs)
{
	table_def const& def = subject.def;
	runs.clear();
	auto onRun = [&](size_t start, size_t end, std::span<int64_t const> key, bool inDomain)
		-> tl::expected<void, ErrorContext>
	{
		if (!inDomain)
		{
			if (report)
			{
				report->push_back(run_note{subject.table, run_status::Orphan, KeyCells(key)});
			}
			return {};
		}
		if (start == end && everyTuple)
		{
			return tl::make_unexpected(ErrorContext("table '", def.name, "' key (", TupleText(key),
				") has no row to draw; no row has this key, and the key is in ", subject.domain));
		}
		runs.push_back(row_span{static_cast<uint32_t>(start), static_cast<uint32_t>(end - start)});
		return {};
	};
	return WalkDomain(subject.rows, def.key_count, domains, onRun);
}

}
