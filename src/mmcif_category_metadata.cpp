#include "mmcif_category_metadata.hpp"

#include "duckdb/common/gzip_file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "mmcif_dict_data.hpp"

namespace duckdb {

static string UnescapeCategoryText(const string &text) {
	string result;
	for (idx_t i = 0; i < text.size(); i++) {
		if (text[i] == '\\' && i + 1 < text.size()) {
			i++;
			switch (text[i]) {
			case 'n':
				result += '\n';
				break;
			case 'r':
				result += '\r';
				break;
			case 't':
				result += '\t';
				break;
			default:
				result += text[i];
				break;
			}
		} else {
			result += text[i];
		}
	}
	return result;
}

const MmcifCategoryMetadataIndex &MmcifCategoryMetadataIndex::Get() {
	static MmcifCategoryMetadataIndex instance;
	return instance;
}

MmcifCategoryMetadataIndex::MmcifCategoryMetadataIndex() {
	auto content = GZipFileSystem::UncompressGZIPString(
	    string(reinterpret_cast<const char *>(MMCIFF_CATEGORIES_GZ), MMCIFF_CATEGORIES_GZ_SIZE));
	case_insensitive_map_t<vector<Value>> groups;
	for (auto &line : StringUtil::Split(content, '\n')) {
		if (line.empty() || line[0] == '#') {
			continue;
		}
		auto fields = StringUtil::Split(line, '\t');
		if (fields.size() != 3) {
			throw InternalException("Invalid bundled mmCIF category metadata");
		}
		auto &metadata = categories[fields[0]];
		auto &field = fields[1];
		auto value = UnescapeCategoryText(fields[2]);
		if (field == "is_single_row") {
			metadata.is_single_row = Value::BOOLEAN(value == "true");
		} else if (field == "description") {
			metadata.description = Value(value);
		} else if (field == "is_mandatory") {
			metadata.is_mandatory = Value::BOOLEAN(value == "true");
		} else if (field == "group") {
			groups[fields[0]].push_back(Value(value));
		}
	}
	for (auto &group : groups) {
		categories[group.first].category_groups = Value::LIST(LogicalType::VARCHAR, group.second);
	}
}

const MmcifCategoryMetadata &MmcifCategoryMetadataIndex::Lookup(const string &category) const {
	auto entry = categories.find(category);
	return entry == categories.end() ? unknown : entry->second;
}

} // namespace duckdb
