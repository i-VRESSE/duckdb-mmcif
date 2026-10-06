// MmcifWriteStore: mutable, no-deps write model materialized from the index.
//
// The index->store seam is MmcifIndex::Materialize(), defined here so the
// read-index TU carries no write-model knowledge while this module still
// reaches the index privates (data_block_name, categories, text)
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

// Line boundaries around a byte offset in the source buffer.
static idx_t MmcifLineStart(const char *base, idx_t pos) {
	while (pos > 0 && base[pos - 1] != '\n') {
		pos--;
	}
	return pos;
}

static idx_t MmcifLineEnd(const char *base, idx_t size, idx_t pos) {
	idx_t p = pos;
	while (p < size && base[p] != '\n') {
		p++;
	}
	return p < size ? p + 1 : size;
}

static bool MmcifOnlyWhitespace(const char *base, idx_t from, idx_t to) {
	for (idx_t i = from; i < to; i++) {
		if (!isspace(static_cast<unsigned char>(base[i]))) {
			return false;
		}
	}
	return true;
}

// Whole-line span of a key-value row: from the line holding the tag of its
// first value through the line its last value ends on. The tag shares the
// value's line, or sits alone on the line above a value written on its own line
// (a
// ';' text field), so cutting the row removes tags and values together.
static MmcifRowSpan MmcifRowSpanFor(const char *base, idx_t size, idx_t first_off, idx_t last_end) {
	MmcifRowSpan rs;
	idx_t line_start = MmcifLineStart(base, first_off);
	if (MmcifOnlyWhitespace(base, line_start, first_off) && line_start > 0) {
		line_start = MmcifLineStart(base, line_start - 1);
	}
	rs.start = line_start;
	rs.end = MmcifLineEnd(base, size, last_end);
	return rs;
}

shared_ptr<MmcifWriteStore> MmcifIndex::Materialize() {
	// Keep the source around: the write-back patches it instead of regenerating.
	auto store = make_shared_ptr<MmcifWriteStore>(shared_from_this());
	const char *content_data = text.data();
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
			std::vector<MmcifCellSpan> spans(ncols);
			idx_t li = 0;
			idx_t row_first = 0;
			idx_t row_last = 0;
			while (cursor.Next(&out, &len, &is_null)) {
				if (is_null) {
					row[li] = string(out, len); // "." or "?"
				} else {
					row[li] = MmcifDecodeValue(out, len);
				}
				idx_t off = idx_t(out - content_data);
				spans[cat->loop_col_map[li]] = MmcifCellSpan {off, len, false};
				if (li == 0) {
					row_first = off;
				}
				row_last = off + len;
				li++;
				if (li == loop_ncols) {
					std::vector<string> full(ncols, "");
					for (idx_t i = 0; i < loop_ncols; i++) {
						full[cat->loop_col_map[i]] = std::move(row[i]);
					}
					wc.rows.push_back(std::move(full));
					wc.row_spans.push_back(MmcifRowSpan {row_first, row_last});
					wc.cell_spans.push_back(std::move(spans));
					spans.assign(ncols, MmcifCellSpan {});
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
				wc.row_spans.push_back(MmcifRowSpan {row_first, row_last});
				wc.cell_spans.push_back(std::move(spans));
			}
		} else {
			// Single-tag category: exactly one row, cells keyed by full column.
			std::vector<string> full(ncols, "");
			std::vector<MmcifCellSpan> spans(ncols);
			bool any = false;
			idx_t row_first = 0;
			idx_t row_last = 0;
			for (auto &sc : cat->singles) {
				if (sc.is_null) {
					full[sc.col] = string(content_data + sc.off, sc.len); // "." / "?"
				} else {
					full[sc.col] = MmcifDecodeValue(content_data + sc.off, sc.len);
				}
				spans[sc.col] = MmcifCellSpan {sc.off, sc.len, false};
				if (!any || sc.off < row_first) {
					row_first = sc.off;
					any = true;
				}
				if (sc.off + sc.len > row_last) {
					row_last = sc.off + sc.len;
				}
			}
			wc.rows.push_back(std::move(full));
			wc.cell_spans.push_back(std::move(spans));
			// A key-value item owns its whole line (tag and value), so the row
			// covers every line its items are written on.
			wc.row_spans.push_back(any ? MmcifRowSpanFor(content_data, original_text_size, row_first, row_last)
			                           : MmcifRowSpan {});
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

void MmcifWriteStore::AddRow(MmcifWriteCategory &cat, const std::vector<string> &row) {
	cat.rows.push_back(row);
	// The row has no bytes in the source file: the write-back generates it and
	// splices it in after the category's last original row.
	cat.row_spans.push_back(MmcifRowSpan {});
	cat.cell_spans.push_back(std::vector<MmcifCellSpan>(row.size()));
	dirty = true;
}

void MmcifWriteStore::DeleteRows(MmcifWriteCategory &cat, const std::vector<unsigned int> &rows) {
	// rows is already sorted + de-duplicated by the caller; delete from the end
	// so indices stay valid.
	for (idx_t i = rows.size(); i > 0; i--) {
		idx_t r = rows[i - 1];
		if (r < cat.row_spans.size() && cat.row_spans[r].start != MMCIF_NO_SPAN) {
			// Remember which bytes to cut out of the source file.
			cat.deleted_rows.push_back(cat.row_spans[r]);
		}
		cat.rows.erase(cat.rows.begin() + r);
		if (r < cat.row_spans.size()) {
			cat.row_spans.erase(cat.row_spans.begin() + r);
		}
		if (r < cat.cell_spans.size()) {
			cat.cell_spans.erase(cat.cell_spans.begin() + r);
		}
	}
	dirty = true;
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
	if (row < cat.cell_spans.size() && col_index < cat.cell_spans[row].size()) {
		// Marks the source bytes to replace. A cell with no source span lives
		// in an inserted row, whose whole line is generated on write-back.
		cat.cell_spans[row][col_index].edited = true;
	}
	dirty = true;
}

MmcifWriteStore::MmcifWriteStore(shared_ptr<MmcifIndex> source_index_p) : source_index(std::move(source_index_p)) {
}

const char *MmcifWriteStore::SourceData() const {
	return source_index->GetData();
}

idx_t MmcifWriteStore::SourceSize() const {
	return source_index->GetOriginalTextSize();
}

MmcifCategory *MmcifWriteStore::SourceCategory(const string &name) const {
	return source_index->FindCategory(name);
}

} // namespace duckdb
