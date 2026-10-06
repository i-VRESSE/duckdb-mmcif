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

// The stored string of a raw cell: null markers ("." / "?") verbatim, any
// other value with its quotes or text-field delimiters stripped.
static string MmcifCellString(const char *p, idx_t len, bool is_null) {
	if (!is_null) {
		MmcifUnquote(p, len);
	}
	return string(p, len);
}

// Whole-line span of a key-value row: from the line holding the tag of its
// first item through the line its last value ends on. Values may be separated
// from their tags by any number of comment or blank lines.
static MmcifRowSpan MmcifRowSpanFor(const char *base, idx_t size, idx_t first_tag, idx_t last_end) {
	return MmcifRowSpan {MmcifLineStart(base, first_tag), MmcifLineEnd(base, size, last_end)};
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
			auto flush_row = [&]() {
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
			};
			while (cursor.Next(&out, &len, &is_null)) {
				row[li] = MmcifCellString(out, len, is_null);
				idx_t off = idx_t(out - content_data);
				spans[cat->loop_col_map[li]] = MmcifCellSpan {off, len, false};
				if (li == 0) {
					row_first = off;
				}
				row_last = off + len;
				li++;
				if (li == loop_ncols) {
					flush_row();
				}
			}
			if (li != 0) {
				flush_row(); // partial trailing row
			}
		} else {
			// Single-tag category: exactly one row, cells keyed by full column.
			std::vector<string> full(ncols, "");
			std::vector<MmcifCellSpan> spans(ncols);
			bool any = false;
			idx_t row_first = 0;
			idx_t row_last = 0;
			for (auto &sc : cat->singles) {
				full[sc.col] = MmcifCellString(content_data + sc.off, sc.len, sc.is_null);
				spans[sc.col] = MmcifCellSpan {sc.off, sc.len, false};
				if (!any || sc.tag_off < row_first) {
					row_first = sc.tag_off;
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
			wc.row_spans.push_back(any ? MmcifRowSpanFor(content_data, text.size(), row_first, row_last)
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

void MmcifWriteStore::DeleteRows(MmcifWriteCategory &cat, const std::vector<idx_t> &rows) {
	// rows is already sorted + de-duplicated by the caller; delete from the end
	// so indices stay valid.
	for (idx_t i = rows.size(); i > 0; i--) {
		idx_t r = rows[i - 1];
		if (cat.row_spans[r].start != MMCIF_NO_SPAN) {
			// Remember tokens separately: comments between cells/items do not
			// belong to the deleted row. The patch merges whitespace-only gaps.
			if (!cat.is_loop) {
				for (auto &cell : SourceCategory(cat.name)->singles) {
					cat.deleted_rows.push_back(MmcifRowSpan {cell.tag_off, cell.tag_end});
				}
			}
			for (auto &cell : cat.cell_spans[r]) {
				if (cell.off != MMCIF_NO_SPAN && cell.len != 0) {
					cat.deleted_rows.push_back(MmcifRowSpan {cell.off, cell.off + cell.len});
				}
			}
		}
		cat.rows.erase(cat.rows.begin() + r);
		cat.row_spans.erase(cat.row_spans.begin() + r);
		cat.cell_spans.erase(cat.cell_spans.begin() + r);
	}
	dirty = true;
}

void MmcifWriteStore::UpdateCell(MmcifWriteCategory &cat, idx_t row, idx_t col, const string &value) {
	cat.rows[row][col] = value;
	// Marks the source bytes to replace. A cell with no source span lives in an
	// inserted row, whose whole line is generated on write-back.
	cat.cell_spans[row][col].edited = true;
	dirty = true;
}

MmcifWriteStore::MmcifWriteStore(shared_ptr<MmcifIndex> source_index_p) : source_index(std::move(source_index_p)) {
}

const char *MmcifWriteStore::SourceData() const {
	return source_index->GetData();
}

idx_t MmcifWriteStore::SourceSize() const {
	return source_index->GetSize();
}

MmcifCategory *MmcifWriteStore::SourceCategory(const string &name) const {
	return source_index->FindCategory(name);
}

} // namespace duckdb
