// Dictionary type index (issue 03): lazy singleton loaded from the embedded
// gzip'd TSV artifacts (dict/*.tsv.gz, embedded at build time). Type keys are
// "_category.item"; relationships are (parent_item, child_item) pairs where
// each item is a "_category.item" key carrying both the category (table) and
// the data item (column).

#pragma once

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/types.hpp"

#include <string>
#include <utility>
#include <vector>

namespace duckdb {

class DictionaryIndex {
public:
	static DictionaryIndex &Get();

	// "_category.item" -> DuckDB type; unknown -> VARCHAR
	LogicalType LookupType(const string &category, const string &column) const;

	const std::vector<std::pair<std::string, std::string>> &GetRelationships() const;

private:
	DictionaryIndex();

	void LoadTypes();
	void LoadRelationships();

	case_insensitive_map_t<LogicalType> types;
	std::vector<std::pair<std::string, std::string>> relationships;
};

} // namespace duckdb
