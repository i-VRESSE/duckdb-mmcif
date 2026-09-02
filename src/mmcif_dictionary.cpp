// Dictionary type index (issue 03): lazy singleton loaded from the embedded
// gzip'd TSV artifacts. Type keys are "_category.item"; relationships are
// (parent_item, child_item) pairs where each item is a "_category.item" key
// carrying both the category (table) and the data item (column).

#include "mmcif_dictionary.hpp"

#include "duckdb/common/gzip_file_system.hpp"
#include "duckdb/common/string_util.hpp"

#include "mmcif_dict_data.hpp" // embedded gzip'd dictionary artifacts (CMake)

#include <string>
#include <utility>

namespace duckdb {

DictionaryIndex &DictionaryIndex::Get() {
	static DictionaryIndex instance;
	return instance;
}

DictionaryIndex::DictionaryIndex() {
	LoadTypes();
	LoadRelationships();
}

void DictionaryIndex::LoadTypes() {
	auto content = GZipFileSystem::UncompressGZIPString(
	    string(reinterpret_cast<const char *>(MMCIFF_TYPE_INDEX_GZ), MMCIFF_TYPE_INDEX_GZ_SIZE));
	auto lines = StringUtil::Split(content, '\n');
	for (auto &line : lines) {
		if (line.empty() || line[0] == '#') {
			continue;
		}
		auto tab = line.find('\t');
		if (tab == string::npos) {
			continue;
		}
		auto item = line.substr(0, tab);
		auto type_str = line.substr(tab + 1);
		LogicalType type;
		if (type_str == "DOUBLE") {
			type = LogicalType::DOUBLE;
		} else if (type_str == "BIGINT") {
			type = LogicalType::BIGINT;
		} else {
			type = LogicalType::VARCHAR;
		}
		types[item] = std::move(type);
	}
}

void DictionaryIndex::LoadRelationships() {
	auto content = GZipFileSystem::UncompressGZIPString(
	    string(reinterpret_cast<const char *>(MMCIFF_RELATIONSHIPS_GZ), MMCIFF_RELATIONSHIPS_GZ_SIZE));
	auto lines = StringUtil::Split(content, '\n');
	for (auto &line : lines) {
		if (line.empty() || line[0] == '#') {
			continue;
		}
		auto tab = line.find('\t');
		if (tab == string::npos) {
			continue;
		}
		relationships.emplace_back(line.substr(0, tab), line.substr(tab + 1));
	}
}

// "_category.item" -> DuckDB type; unknown -> VARCHAR
LogicalType DictionaryIndex::LookupType(const string &category, const string &column) const {
	auto key = "_" + category + "." + column;
	auto entry = types.find(key);
	if (entry != types.end()) {
		return entry->second;
	}
	return LogicalType::VARCHAR;
}

const std::vector<std::pair<std::string, std::string>> &DictionaryIndex::GetRelationships() const {
	return relationships;
}

} // namespace duckdb
