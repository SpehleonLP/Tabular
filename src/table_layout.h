#ifndef TABULAR_TABLE_LAYOUT_H
#define TABULAR_TABLE_LAYOUT_H

// What the front ends and the store agree on about a table's rows, without the
// store's Storage. Not a public header: nothing outside src/ includes it.

#include "tabular/relation.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace tabular
{

// A type as messages name it.
inline char const* TypeName(column_type type)
{
	switch (type)
	{
	case column_type::Hash:      return "hash";
	case column_type::Flags:     return "flag set";
	case column_type::Int:       return "int";
	case column_type::Double:    return "double";
	case column_type::Condition: return "condition";
	case column_type::Command:   return "command";
	case column_type::Vec2:      return "vec2";
	case column_type::String:    return "string";
	case column_type::Band:      return "band";
	}
	return "unknown type";
}

// Whether column of def is its fall-through key (fallthrough: true): its 0 is
// the blank run's, not a value.
inline bool IsFallThroughKey(table_def const& def, uint32_t column)
{
	if (!def.columns[column].key || def.domains.empty())
	{
		return false;
	}
	// Domains are one per key column, in column order.
	uint32_t k = 0;
	for (uint32_t c = 0; c < column; ++c)
	{
		k += def.columns[c].key ? 1 : 0;
	}
	return def.domains[k].kind == domain_kind::FallThrough;
}

// The most fall-through keys a table may declare: a lookup that misses may try
// every combination of blanks, 2^n - 1 retries, so n stays small (Part 4 of
// the commands-and-messages spec).
inline constexpr size_t kMaxFallThroughKeys = 8;

// A table's fall-through keys, as indices among its keys in key order, which
// is the order they give way in (the first is the lowest bit of the retry
// count). Fixed-size, so a lookup holds it without allocating.
struct fall_through_keys
{
	std::array<uint32_t, kMaxFallThroughKeys> key{};
	uint32_t                                  count = 0;
};

inline fall_through_keys FallThroughKeysOf(table_def const& def)
{
	// Domains are one per key column, in column order, as a picked table's
	// keys are; the schema refuses more than kMaxFallThroughKeys.
	fall_through_keys out;
	for (uint32_t k = 0; k < def.domains.size() && out.count < kMaxFallThroughKeys; ++k)
	{
		if (def.domains[k].kind == domain_kind::FallThrough)
		{
			out.key[out.count++] = k;
		}
	}
	return out;
}

// A flag set (AngelScript uint32) and a hash (CityHash32) are 32-bit values
// carried as int64_t, widened without sign. Anything outside [0, 2^32) is
// either not one of them or a sign-extended copy, which would be a different
// key that never matches the zero-extended one.
inline bool Fits32(int64_t v)
{
	return v >= 0 && v <= INT64_C(0xFFFFFFFF);
}

inline bool Is32BitColumn(column_type type)
{
	return type == column_type::Hash || type == column_type::Flags;
}

// The one refusal for a 32-bit column's value outside [0, 2^32), whichever
// front end or table kind it came through.
inline tl::expected<void, ErrorContext> Check32(column_def const& column, table_def const& def, int64_t v)
{
	if (Is32BitColumn(column.type) && !Fits32(v))
	{
		return tl::make_unexpected(ErrorContext("column '", column.name, "' of '", def.name, "' is a ",
			column.type == column_type::Hash ? "hash" : "flag set",
			" and holds a 32-bit value widened without sign; got ", v));
	}
	return {};
}

// A band is a half-open range that can hold something: both ends finite and
// start before end. NaN fails every comparison, so it is refused here too.
inline bool IsWellFormedBand(cell band)
{
	return std::isfinite(band.v[0]) && std::isfinite(band.v[1]) && band.v[0] < band.v[1];
}

// An edge column's cell that holds no edge (NoEdge(): both ends NaN). A
// well-formed band has neither end NaN, so the two never meet.
inline bool IsNoEdge(cell band)
{
	return std::isnan(band.v[0]) && std::isnan(band.v[1]);
}

// The edge band of a row at a column, or none when the column is no edge or the
// row's cell there holds no edge. rowCells is the row's value cells and slot
// the column's position among them. One reading, so the proof, the pick and
// the display cannot disagree on which cells are edges.
inline std::optional<cell> EdgeBandAt(column_def const& column, cell const* rowCells, uint32_t slot)
{
	if (!column.edge || IsNoEdge(rowCells[slot]))
	{
		return std::nullopt;
	}
	return rowCells[slot];
}

// Each column's position among the columns of its kind, as the store numbers
// them. Every front end that fills a row asks for it here, so a layout is
// decided once.
struct slot_layout
{
	uint32_t              valueCount     = 0;
	uint32_t              conditionCount = 0;
	uint32_t              commandCount   = 0;
	uint32_t              stringCount    = 0;
	std::vector<uint32_t> slot;          // per column: its position among columns of its kind

	explicit slot_layout(table_def const& def)
	{
		slot.resize(def.columns.size());
		for (uint32_t c = 0; c < def.columns.size(); ++c)
		{
			switch (def.columns[c].type)
			{
			case column_type::Condition: slot[c] = conditionCount++; break;
			case column_type::Command:   slot[c] = commandCount++;   break;
			case column_type::String:    slot[c] = stringCount++;    break;
			default:                     slot[c] = valueCount++;     break;
			}
		}
	}
};

}

#endif // TABULAR_TABLE_LAYOUT_H
