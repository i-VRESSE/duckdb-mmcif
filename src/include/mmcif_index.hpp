// mmcif lazy index + streaming scanner: a hand-written streaming mmCIF
// scanner and a two-pass lazy index:
//
//   Pass 1 (index): scan the whole decompressed buffer once, line by line,
//   recording each category's columns and the byte ranges of its loop data.
//   No cell strings are materialized, so schema enumeration, mmcif_tables(),
//   mmcif_columns(),
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
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace duckdb {

// Start of the line holding byte `pos`.
inline idx_t MmcifLineStart(const char *base, idx_t pos) {
	while (pos > 0 && base[pos - 1] != '\n') {
		pos--;
	}
	return pos;
}

// Start of the line after the one holding byte `pos` (or `size`).
inline idx_t MmcifLineEnd(const char *base, idx_t size, idx_t pos) {
	while (pos < size && base[pos] != '\n') {
		pos++;
	}
	return pos < size ? pos + 1 : size;
}

// ---------------------------------------------------------------------------
// MmcifValueCursor: reads mmCIF data values sequentially from a byte range of
// the decompressed buffer (row-major loop data). Handles plain tokens, single
// and double quotes (closed only by a quote followed by whitespace), triple
// quotes, and ;...; multi-line values opened in column 1. A bare '.' or '?' is
// NULL. Returns the raw token as (offset, len) into the buffer; MmcifUnquote
// strips its delimiters.
//
// A '#' at a token boundary starts a comment that runs to the end of the line;
// comment lines are skipped so loop data containing a comment still reads its
// values (a comment is not a value).
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
				// A quote closes the value only when whitespace (or the end)
				// follows it, so 'it's' is one value.
				while (pos < end) {
					if (base[pos] == q && (pos + 1 >= end || isspace(static_cast<unsigned char>(base[pos + 1])))) {
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
		if (c == ';' && (pos == 0 || base[pos - 1] == '\n')) {
			// Text field: only a ';' in column 1 opens one.
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
		// A bare "." or "?" is NULL; ".5" or "?x" is an ordinary value.
		*is_null = *len == 1 && (c == '.' || c == '?');
		return true;
	}

private:
	void SkipWs() {
		while (pos < end) {
			if (isspace(static_cast<unsigned char>(base[pos]))) {
				pos++;
			} else if (base[pos] == '#') {
				// Comment: skip to the end of the line and keep looking for a
				// value. Only at a token boundary, so '#' inside a quoted or
				// text-quoted value is never treated as a comment.
				while (pos < end && base[pos] != '\n') {
					pos++;
				}
			} else {
				break;
			}
		}
	}
	const char *base;
	idx_t end;
	idx_t pos;
};

// Strip the delimiters of a raw token from MmcifValueCursor, in place:
//   'x' / "x" / '''x''' -> x (interior kept verbatim)
//   ;x\n;               -> x (the closing "\n;" and its '\r' removed)
//   anything else       -> unchanged
inline void MmcifUnquote(const char *&p, idx_t &len) {
	if (len == 0) {
		return;
	}
	char c = p[0];
	if (c == '\'' || c == '"') {
		idx_t q = len >= 6 && p[1] == c && p[2] == c && p[len - 1] == c && p[len - 2] == c && p[len - 3] == c ? 3 : 1;
		if (len >= 2 * q && p[len - 1] == c) {
			p += q;
			len -= 2 * q;
		} else {
			p += 1; // unterminated: drop the opening quote only
			len -= 1;
		}
		return;
	}
	if (c == ';' && memchr(p, '\n', len)) {
		// A text field always spans lines; a plain ";x" token does not.
		p += 1;
		len -= 1;
		if (len >= 2 && p[len - 1] == ';' && p[len - 2] == '\n') {
			len -= 2;
			if (len > 0 && p[len - 1] == '\r') {
				len--;
			}
		}
	}
}

// ---------------------------------------------------------------------------
// MmcifCategory: pass-1 index record for one mmCIF category.
// ---------------------------------------------------------------------------

struct MmcifSingleCell {
	idx_t col;     // full column index into columns
	idx_t tag_off; // start of the item's tag
	idx_t tag_end; // end of the item's tag (exclusive)
	idx_t off;     // offset of the value in the decompressed buffer
	idx_t len;     // length of the value
	bool is_null;
};

struct MmcifCategory {
	string name;
	vector<string> columns; // item names, in first-seen order
	bool is_loop = false;
	vector<idx_t> loop_col_map;      // loop position -> full column index (loop categories)
	idx_t data_start = 0;            // byte offset of loop data start (loop categories)
	idx_t data_end = 0;              // byte offset past loop data end (exclusive)
	vector<MmcifSingleCell> singles; // single-tag cells, keyed by full column index

	std::atomic<idx_t> row_count = {idx_t(-1)}; // -1 == unknown (computed lazily)
};

// ---------------------------------------------------------------------------
// MmcifIndex: owns the decompressed content buffer + the pass-1 index.
// Load() is process-level cached by path and block selection.
// ---------------------------------------------------------------------------

// The write model lives in mmcif_write_store.hpp; the seam between the index
// and the store is MmcifIndex::Materialize() below.
class MmcifWriteStore;

class MmcifIndex : public enable_shared_from_this<MmcifIndex> {
public:
	// Load (or fetch from the process-level cache) the index for a file.
	static shared_ptr<MmcifIndex> Load(const string &path, optional_ptr<ClientContext> context,
	                                   optional_ptr<const string> data_block = nullptr);

	// Drop the process-level cache entry for a path after the file on disk
	// changes (e.g. after a write-mode COMMIT persists). The next Load re-reads.
	static void InvalidateCache(const string &path);

	// The seam between the read index and the write model: materialize the
	// selected data block as a mutable MmcifWriteStore (write mode).
	shared_ptr<MmcifWriteStore> Materialize();

	const string &GetDataBlockName() const {
		return data_block_name;
	}

	const vector<string> &GetDataBlockNames() const {
		return data_block_names;
	}

	// Find a category by name (case-insensitive). Returns nullptr if absent.
	MmcifCategory *FindCategory(const string &name);

	// Category names, in file order.
	vector<string> GetCategoryNames() const;
	const vector<unique_ptr<MmcifCategory>> &GetCategories() const {
		return categories;
	}

	// Exact row count for a category, computed lazily once by
	// value-scanning the loop range without materializing strings.
	idx_t GetRowCount(MmcifCategory &cat);

	const char *GetData() const {
		return text.data();
	}
	idx_t GetSize() const {
		return text.size();
	}

private:
	explicit MmcifIndex(string text_p) : text(std::move(text_p)) {
	}
	void Build(optional_ptr<const string> data_block);

	string text; // decompressed mmCIF text (the flat string arena)

	vector<string> data_block_names;
	string data_block_name;
	vector<unique_ptr<MmcifCategory>> categories;
};

} // namespace duckdb

#endif
