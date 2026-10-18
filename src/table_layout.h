#ifndef TABULAR_TABLE_LAYOUT_H
#define TABULAR_TABLE_LAYOUT_H

// What the front ends and the store agree on about a table's rows, without the
// store's Storage. Not a public header: nothing outside src/ includes it.

#include "tabular/relation.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace tabular
{

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
