// Dictionary type index: lazy singleton loaded from the embedded
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

// Call fn(first, second) for each "first\tsecond" line of an embedded gzip'd
// TSV, skipping blank and '#' comment lines.
template <class FN>
static void MmcifForEachTsvRow(const unsigned char *gz, idx_t gz_size, FN fn) {
	auto content = GZipFileSystem::UncompressGZIPString(string(reinterpret_cast<const char *>(gz), gz_size));
	for (auto &line : StringUtil::Split(content, '\n')) {
		auto tab = line.find('\t');
		if (line.empty() || line[0] == '#' || tab == string::npos) {
			continue;
		}
		fn(line.substr(0, tab), line.substr(tab + 1));
	}
}

DictionaryIndex::DictionaryIndex() {
	MmcifForEachTsvRow(MMCIFF_TYPE_INDEX_GZ, MMCIFF_TYPE_INDEX_GZ_SIZE, [&](string item, const string &type_str) {
		if (type_str == "DOUBLE") {
			types[item] = LogicalType::DOUBLE;
		} else if (type_str == "BIGINT") {
			types[item] = LogicalType::BIGINT;
		} else {
			types[item] = LogicalType::VARCHAR;
		}
		auto dot = item.find('.');
		if (dot != string::npos && !item.empty() && item[0] == '_') {
			columns[item.substr(1, dot - 1)].push_back(item.substr(dot + 1));
		}
	});
	MmcifForEachTsvRow(MMCIFF_RELATIONSHIPS_GZ, MMCIFF_RELATIONSHIPS_GZ_SIZE,
	                   [&](string parent, string child) { relationships.emplace_back(parent, child); });
	MmcifForEachTsvRow(MMCIFF_DOCUMENTATION_GZ, MMCIFF_DOCUMENTATION_GZ_SIZE,
	                   [&](string key, string url) { documentation[key] = std::move(url); });
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

string DictionaryIndex::GetCategoryUrl(const string &category) const {
	auto entry = documentation.find(category);
	if (entry != documentation.end()) {
		return entry->second + "/Categories/" + entry->first + ".html";
	}
	return "https://mmcif.wwpdb.org/dictionaries/mmcif_pdbx_v50.dic/Categories/" + category + ".html";
}

string DictionaryIndex::GetItemUrl(const string &category, const string &column) const {
	auto key = "_" + category + "." + column;
	auto entry = documentation.find(key);
	// Prefer the defining dictionary's canonical spelling when available.
	// Unknown items retain the existing base-dictionary URL fallback.
	if (entry != documentation.end()) {
		return entry->second + "/Items/" + entry->first + ".html";
	}
	return "https://mmcif.wwpdb.org/dictionaries/mmcif_pdbx_v50.dic/Items/" + key + ".html";
}

const vector<string> *DictionaryIndex::GetColumns(const string &category) const {
	auto entry = columns.find(category);
	return entry == columns.end() ? nullptr : &entry->second;
}

const std::vector<std::pair<std::string, std::string>> &DictionaryIndex::GetRelationships() const {
	return relationships;
}

} // namespace duckdb
