// mmcif lazy index + streaming scanner (big-pdb-too-slow.md, recommendations 4 & 5).
//
// Replaces the RCSB flex/bison parse path for READ-ONLY mmcif files with a
// hand-written streaming mmCIF scanner and a two-pass lazy index:
//
//   Pass 1 (index): scan the whole decompressed buffer once, line by line,
//   recording each category's columns and the byte ranges of its loop data.
//   No cell strings are materialized, so schema enumeration, mmcif_tables(),
//   and DESCRIBE are fast with no full parse.
//
//   Pass 2 (materialize): parse only the queried category's loop range, row
//   major, incrementally from a byte cursor. This gives per-category lazy load
//   and LIMIT pushdown (a LIMIT 10 parses ~10 rows, not 2.44M).
//
// Cells are referenced as (offset, len) into one decompressed buffer (a flat
// string arena) instead of vector<std::string>; nothing is copied until a cell
// is actually emitted.

#ifndef DUCKDB_MMCIF_INDEX_HPP
#define DUCKDB_MMCIF_INDEX_HPP

#include "duckdb.hpp"

#include <atomic>
#include <cctype>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace duckdb {

// ---------------------------------------------------------------------------
// MmcifValueCursor: reads mmCIF data values sequentially from a byte range of
// the decompressed buffer (row-major loop data). Handles plain tokens, single
// and double quotes (with '' / "" escaping), triple quotes, and ;...; multi-line
// values. '.' and '?' are NULL. Returns (offset, len) into the buffer.
// ---------------------------------------------------------------------------

class MmcifValueCursor {
public:
	MmcifValueCursor(const char *base_p, idx_t start_p, idx_t end_p) : base(base_p), end(end_p), pos(start_p) {
	}

	// Parse the next value. Returns false when the range is exhausted.
	bool Next(const char **out, idx_t *len, bool *is_null) {
		SkipWs();
		if (pos >= end) {
			return false;
		}
		char c = base[pos];
		if (c == '.' || c == '?') {
			*out = base + pos;
			*len = 1;
			*is_null = true;
			pos++;
			return true;
		}
		if (c == '\'' || c == '"') {
			char q = c;
			idx_t start = pos;
			pos++;
			bool triple = pos + 1 < end && base[pos] == q && base[pos + 1] == q;
			if (triple) {
				pos += 2;
				while (pos < end) {
					if (base[pos] == q && pos + 2 < end && base[pos + 1] == q && base[pos + 2] == q) {
						pos += 3;
						break;
					}
					pos++;
				}
			} else {
				while (pos < end) {
					if (base[pos] == q) {
						if (pos + 1 < end && base[pos + 1] == q) {
							pos += 2; // escaped doubled quote
							continue;
						}
						pos++;
						break;
					}
					pos++;
				}
			}
			*out = base + start;
			*len = pos - start;
			*is_null = false;
			return true;
		}
		if (c == ';') {
			idx_t start = pos;
			pos++;
			while (pos < end && base[pos] != '\n') {
				pos++;
			}
			if (pos < end) {
				pos++;
			}
			// Value continues until a line whose first character is ';'.
			while (pos < end) {
				if (base[pos] == ';') {
					pos++;
					break;
				}
				while (pos < end && base[pos] != '\n') {
					pos++;
				}
				if (pos < end) {
					pos++;
				}
			}
			*out = base + start;
			*len = pos - start;
			*is_null = false;
			return true;
		}
		idx_t start = pos;
		while (pos < end && !isspace(static_cast<unsigned char>(base[pos]))) {
			pos++;
		}
		*out = base + start;
		*len = pos - start;
		*is_null = false;
		return true;
	}

private:
	void SkipWs() {
		while (pos < end && isspace(static_cast<unsigned char>(base[pos]))) {
			pos++;
		}
	}
	const char *base;
	idx_t end;
	idx_t pos;
};

// ---------------------------------------------------------------------------
// MmcifCategory: pass-1 index record for one mmCIF category.
// ---------------------------------------------------------------------------

struct MmcifSingleCell {
	idx_t col; // full column index into columns
	idx_t off; // offset of the value in the decompressed buffer
	idx_t len; // length of the value
	bool is_null;
};

struct MmcifCategory {
	string name;
	vector<string> columns; // item names, in first-seen order
	bool is_loop = false;
	vector<idx_t> loop_col_map;      // loop position -> full column index (loop categories)
	vector<bool> is_loop_col;        // parallel to columns: true if the column comes from the loop
	idx_t data_start = 0;            // byte offset of loop data start (loop categories)
	idx_t data_end = 0;              // byte offset past loop data end (exclusive)
	vector<MmcifSingleCell> singles; // single-tag cells, keyed by full column index

	std::atomic<idx_t> row_count = {idx_t(-1)}; // -1 == unknown (computed lazily)
};

// ---------------------------------------------------------------------------
// MmcifIndex: owns the decompressed content buffer + the pass-1 index.
// Load() is process-level cached by path (re-attach reuses content + index).
// ---------------------------------------------------------------------------

class MmcifIndex {
public:
	// Load (or fetch from the process-level cache) the index for a file.
	static shared_ptr<MmcifIndex> Load(const string &path, optional_ptr<ClientContext> context);

	const string &GetDataBlockName() const {
		return data_block_name;
	}

	// Find a category by name (case-insensitive). Returns nullptr if absent.
	MmcifCategory *FindCategory(const string &name);

	// Enumerate all category names.
	void GetCategoryNames(vector<string> &names);

	// Exact row count for a category, computed lazily once (thread-safe) by
	// value-scanning the loop range without materializing strings.
	idx_t GetRowCount(MmcifCategory &cat);

	const char *GetData() const {
		return content_data;
	}
	idx_t GetDataSize() const {
		return content_size;
	}

private:
	MmcifIndex(string raw_p, string text_p)
	    : raw(std::move(raw_p)), text(std::move(text_p)), content_data(text.data()), content_size(text.size()) {
	}
	void Build();

	string raw;  // compressed bytes (kept for cache invalidation checks)
	string text; // decompressed mmCIF text (the flat string arena)
	const char *content_data;
	idx_t content_size;

	string data_block_name;
	vector<unique_ptr<MmcifCategory>> categories;
	mutex row_count_lock;

	friend class MmcifWriteStore;
};

// ---------------------------------------------------------------------------
// MmcifWriteCategory / MmcifWriteStore: the no-deps mutable write model.
//
// Write mode keeps one persistent MmcifWriteStore per attached catalog instead
// of the RCSB CifFile/ISTable core. Cells are materialized (row-major
// vector<string>) so DML can mutate in place and a plain-text writer can emit
// the file back. Null cells are stored as "." / "?" (the RCSB parser's stored
// forms), so the writer re-emits them unchanged; "" also maps to "?" on emit.
// ---------------------------------------------------------------------------

struct MmcifWriteCategory {
	string name;
	bool is_loop = false;
	std::vector<std::string> columns;           // item names, in first-seen order
	std::vector<std::vector<std::string>> rows; // row-major cell strings, one per column
};

class MmcifWriteStore {
public:
	MmcifWriteStore() = default;

	string data_block_name;
	std::vector<MmcifWriteCategory> categories;

	// Materialize a mutable store from a (read-only) MmcifIndex.
	static shared_ptr<MmcifWriteStore> FromIndex(const MmcifIndex &index);

	MmcifWriteCategory *FindCategory(const string &name);
	std::vector<std::string> GetCategoryNames() const;
	idx_t GetNumRows(MmcifWriteCategory &cat) const;
	const std::vector<std::string> &GetRow(MmcifWriteCategory &cat, idx_t row) const;
	void AddRow(MmcifWriteCategory &cat, const std::vector<std::string> &row);
	void DeleteRows(MmcifWriteCategory &cat, const std::vector<unsigned int> &rows);
	void UpdateCell(MmcifWriteCategory &cat, idx_t row, const string &col, const string &value);
};

} // namespace duckdb

#endif
