// MmcifWriteStore: mutable, no-deps write model materialized from the index.
//
// The index->store seam is MmcifIndex::Materialize(), defined here so the
// read-index TU carries no write-model knowledge while this module still
// reaches the index privates (data_block_name, categories, content_data)
// without a friend declaration.

#include "mmcif_write_store.hpp"

#include "duckdb/common/string_util.hpp"

#include "mmcif_index.hpp"

namespace duckdb {

// Decode one raw cell span into the stored cell string, matching the RCSB
// parser's stored forms (so write-back is byte-identical):
//   - "." / "?"  -> stored literally (null markers)
//   - 'x' / "x"  -> quotes stripped, interior kept (doubled quotes preserved)
//   - ";...;"    -> multi-line text, leading ';' and trailing ';'/ws stripped,
//                   internal newlines kept
//   - otherwise  -> unquoted token as-is
static string MmcifDecodeValue(const char *p, idx_t len) {
	if (len == 0) {
		return "";
	}
	char c = p[0];
	if (c == '\'' || c == '"') {
		idx_t interior = len > 2 ? len - 2 : 0;
		return string(p + 1, interior);
	}
	if (c == ';') {
		idx_t start = 1;
		idx_t end = len;
		string val(p + start, end - start);
		while (!val.empty() && (val.back() == ' ' || val.back() == '\t' || val.back() == '\n' || val.back() == '\r' ||
		                        val.back() == ';')) {
			val.pop_back();
		}
		return val;
	}
	return string(p, len);
}

shared_ptr<MmcifWriteStore> MmcifIndex::Materialize() {
	auto store = shared_ptr<MmcifWriteStore>(new MmcifWriteStore());
	store->data_block_name = data_block_name;
	for (auto &cat : categories) {
		MmcifWriteCategory wc;
		wc.name = cat->name;
		wc.is_loop = cat->is_loop;
		wc.columns.assign(cat->columns.begin(), cat->columns.end());
		idx_t ncols = wc.columns.size();
		if (cat->is_loop) {
			idx_t loop_ncols = cat->loop_col_map.size();
			MmcifValueCursor cursor(content_data, cat->data_start, cat->data_end);
			const char *out;
			idx_t len;
			bool is_null;
			std::vector<string> row(loop_ncols, "");
			idx_t li = 0;
			while (cursor.Next(&out, &len, &is_null)) {
				if (is_null) {
					row[li] = string(out, len); // "." or "?"
				} else {
					row[li] = MmcifDecodeValue(out, len);
				}
				li++;
				if (li == loop_ncols) {
					std::vector<string> full(ncols, "");
					for (idx_t i = 0; i < loop_ncols; i++) {
						full[cat->loop_col_map[i]] = std::move(row[i]);
					}
					wc.rows.push_back(std::move(full));
					row.assign(loop_ncols, "");
					li = 0;
				}
			}
			if (li != 0) {
				// Partial trailing row.
				std::vector<string> full(ncols, "");
				for (idx_t i = 0; i < loop_ncols; i++) {
					full[cat->loop_col_map[i]] = std::move(row[i]);
				}
				wc.rows.push_back(std::move(full));
			}
		} else {
			// Single-tag category: exactly one row, cells keyed by full column.
			std::vector<string> full(ncols, "");
			for (auto &sc : cat->singles) {
				if (sc.is_null) {
					full[sc.col] = string(content_data + sc.off, sc.len); // "." / "?"
				} else {
					full[sc.col] = MmcifDecodeValue(content_data + sc.off, sc.len);
				}
			}
			wc.rows.push_back(std::move(full));
		}
		store->categories.push_back(std::move(wc));
	}
	return store;
}

MmcifWriteCategory *MmcifWriteStore::FindCategory(const string &name) {
	for (auto &cat : categories) {
		if (StringUtil::CIEquals(cat.name, name)) {
			return &cat;
		}
	}
	return nullptr;
}

std::vector<string> MmcifWriteStore::GetCategoryNames() const {
	std::vector<string> names;
	for (auto &cat : categories) {
		names.push_back(cat.name);
	}
	return names;
}

idx_t MmcifWriteStore::GetNumRows(MmcifWriteCategory &cat) const {
	return cat.rows.size();
}

const std::vector<string> &MmcifWriteStore::GetRow(MmcifWriteCategory &cat, idx_t row) const {
	return cat.rows[row];
}

void MmcifWriteStore::AddRow(MmcifWriteCategory &cat, const std::vector<string> &row) {
	cat.rows.push_back(row);
}

void MmcifWriteStore::DeleteRows(MmcifWriteCategory &cat, const std::vector<unsigned int> &rows) {
	// rows is already sorted + de-duplicated by the caller; delete from the end
	// so indices stay valid.
	for (idx_t i = rows.size(); i > 0; i--) {
		cat.rows.erase(cat.rows.begin() + rows[i - 1]);
	}
}

void MmcifWriteStore::UpdateCell(MmcifWriteCategory &cat, idx_t row, const string &col, const string &value) {
	idx_t col_index = 0;
	for (idx_t i = 0; i < cat.columns.size(); i++) {
		if (StringUtil::CIEquals(cat.columns[i], col)) {
			col_index = i;
			break;
		}
	}
	cat.rows[row][col_index] = value;
}

} // namespace duckdb
