#pragma once

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/types/value.hpp"

namespace duckdb {

struct MmcifCategoryMetadata {
	Value is_single_row = Value(LogicalType::BOOLEAN);
	Value description = Value(LogicalType::VARCHAR);
	Value category_groups = Value(LogicalType::LIST(LogicalType::VARCHAR));
	Value is_mandatory = Value(LogicalType::BOOLEAN);
};

// Offline dictionary metadata; independent of the categories/rows in a file.
class MmcifCategoryMetadataIndex {
public:
	static const MmcifCategoryMetadataIndex &Get();
	const MmcifCategoryMetadata &Lookup(const string &category) const;

private:
	MmcifCategoryMetadataIndex();
	case_insensitive_map_t<MmcifCategoryMetadata> categories;
	MmcifCategoryMetadata unknown;
};

} // namespace duckdb
