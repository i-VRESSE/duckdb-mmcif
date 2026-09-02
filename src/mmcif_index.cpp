#include "mmcif_index.hpp"

#include "duckdb/common/file_system.hpp"
#include "duckdb/common/gzip_file_system.hpp"
#include "duckdb/common/string_util.hpp"

#include "mmcif_file.hpp"

namespace duckdb {

// ---------------------------------------------------------------------------
// Process-level cache (recommendation 2): keyed by path, re-attaching the same
// file in one DuckDB session reuses the decompressed content and the pass-1
// index. Local files are invalidated when mtime/size change; remote paths are
// cached by path only (may be stale).
// ---------------------------------------------------------------------------

static mutex g_index_cache_lock;
static unordered_map<string, weak_ptr<MmcifIndex>> g_index_cache;

// Local-file staleness check: (mtime, size) changed since the cached copy.
static bool MmcifFileChanged(const string &path, optional_ptr<ClientContext> context) {
	if (MmcifFile::IsRemotePath(path)) {
		return false; // cannot cheaply stat remote paths
	}
	if (!context) {
		return false;
	}
	auto &fs = FileSystem::GetFileSystem(*context);
	if (!fs.FileExists(path)) {
		return true;
	}
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
	auto mtime = fs.GetLastModifiedTime(*handle);
	auto size = fs.GetFileSize(*handle);
	auto key = StringUtil::Format("%s|%ld|%lld", path, (long)mtime.value, (long long)size);
	// Cache the stamp per path so a re-attach with an unchanged file skips stat.
	static mutex stamp_lock;
	static case_insensitive_map_t<string> stamps;
	lock_guard<mutex> l(stamp_lock);
	auto it = stamps.find(path);
	if (it != stamps.end() && it->second == key) {
		return false;
	}
	stamps[path] = key;
	return true;
}

shared_ptr<MmcifIndex> MmcifIndex::Load(const string &path, optional_ptr<ClientContext> context) {
	{
		lock_guard<mutex> l(g_index_cache_lock);
		auto it = g_index_cache.find(path);
		if (it != g_index_cache.end()) {
			auto cached = it->second.lock();
			if (cached && !MmcifFileChanged(path, context)) {
				return cached;
			}
		}
	}

	string raw = MmcifFile::Read(path, context);
	string text;
	bool is_gzip = GZipFileSystem::CheckIsZip(raw.data(), raw.size());
	if (is_gzip) {
		text = GZipFileSystem::UncompressGZIPString(raw);
	} else {
		text = std::move(raw);
	}
	// Append a dummy trailing data block so the parser's "last loop" is flushed
	// (same trick the RCSB path used); the index keeps only the FIRST data block.
	text += "\ndata_zzz_prototype\n#\n";

	auto index = shared_ptr<MmcifIndex>(new MmcifIndex(std::move(raw), std::move(text)));
	index->Build();

	lock_guard<mutex> l(g_index_cache_lock);
	g_index_cache[path] = weak_ptr<MmcifIndex>(index);
	return index;
}

// ---------------------------------------------------------------------------
// Pass 1: line-by-line scan building the category index. No cell strings are
// materialized; only column names, loop byte ranges, and single-tag cell
// offsets are recorded.
// ---------------------------------------------------------------------------

static inline idx_t MmcifTrimStart(const char *base, idx_t line_start, idx_t line_end) {
	while (line_start < line_end && isspace(static_cast<unsigned char>(base[line_start]))) {
		line_start++;
	}
	return line_start;
}

static inline bool MmcifStartsWith(const char *base, idx_t start, idx_t end, const char *word) {
	size_t n = strlen(word);
	if (end - start < (idx_t)n) {
		return false;
	}
	return memcmp(base + start, word, n) == 0;
}

// Split a "_category.item" tag into (category, item). Returns false for a bare "_".
static bool MmcifSplitTag(const char *base, idx_t start, idx_t len, string &category, string &item) {
	if (len < 1 || base[start] != '_') {
		return false;
	}
	idx_t dot = start + 1;
	while (dot < start + len && base[dot] != '.') {
		dot++;
	}
	if (dot == start + len) {
		return false; // "_category" without item
	}
	category.assign(base + start + 1, dot - (start + 1));
	// The item runs up to the first whitespace: loop-header tags and single-tag
	// lines both carry trailing spaces/tabs, and single-tag lines carry a value.
	idx_t item_start = dot + 1;
	idx_t item_end = item_start;
	while (item_end < start + len && !isspace(static_cast<unsigned char>(base[item_end]))) {
		item_end++;
	}
	item.assign(base + item_start, item_end - item_start);
	// Trim trailing whitespace from category (mmCIF tags often carry trailing
	// spaces/tabs before the newline).
	while (!category.empty() && isspace(static_cast<unsigned char>(category.back()))) {
		category.pop_back();
	}
	return true;
}

void MmcifIndex::Build() {
	const char *base = content_data;
	idx_t size = content_size;
	bool indexed = false; // keep only the FIRST data block

	enum State { TOP, LOOP_HEADER, LOOP_DATA };
	State state = TOP;

	unique_ptr<MmcifCategory> cur;

	auto finalize = [&]() {
		if (cur && !cur->name.empty()) {
			categories.push_back(std::move(cur));
		}
		cur = nullptr;
	};

	idx_t line_start = 0;
	idx_t skip_to = 0; // when > line_end, the loop jumps to this line start
	while (line_start < size) {
		idx_t line_end = line_start;
		while (line_end < size && base[line_end] != '\n') {
			line_end++;
		}
		idx_t s = MmcifTrimStart(base, line_start, line_end);
		if (s < line_end) {
			char c = base[s];
			if (c == '#') {
				// Comment line: terminates loop data.
				if (state == LOOP_DATA && cur) {
					cur->data_end = line_start;
					state = TOP;
				}
			} else if (c == '_') {
				// Tag line.
				string cat, item;
				if (MmcifSplitTag(base, s, line_end - s, cat, item)) {
					if (state == LOOP_HEADER) {
						// Loop header: this line is a column tag.
						if (!cur || cur->name != cat) {
							finalize();
							cur = make_uniq<MmcifCategory>();
							cur->name = cat;
							cur->is_loop = true;
						}
						// Add to the loop column list and record the full-column map.
						idx_t full_col = cur->columns.size();
						for (idx_t i = 0; i < full_col; i++) {
							if (cur->columns[i] == item) {
								full_col = i;
								break;
							}
						}
						if (full_col == cur->columns.size()) {
							cur->columns.push_back(item);
							cur->is_loop_col.push_back(true);
						}
						cur->loop_col_map.push_back(full_col);
					} else {
						// Single-tag line "_cat.item value".
						if (!cur || cur->name != cat) {
							finalize();
							cur = make_uniq<MmcifCategory>();
							cur->name = cat;
						}
						idx_t col = cur->columns.size();
						for (idx_t i = 0; i < col; i++) {
							if (cur->columns[i] == item) {
								col = i;
								break;
							}
						}
						if (col == cur->columns.size()) {
							cur->columns.push_back(item);
							cur->is_loop_col.push_back(false);
						}
						// Parse the value after the tag.
						idx_t tag_end = s;
						while (tag_end < line_end && !isspace(static_cast<unsigned char>(base[tag_end]))) {
							tag_end++;
						}
						idx_t val_start = MmcifTrimStart(base, tag_end, line_end);
						// RCSB writes long / multi-line single-tag values on the line
						// AFTER the tag ("_cat.item" then ";...\n;"), so when this line
						// carries no value, look at the next line for a ';' value.
						bool value_from_next = false;
						idx_t value_consumed_until = 0; // resume line start (0 = none)
						if (val_start >= line_end) {
							idx_t nl = line_end + 1;
							if (nl < size) {
								idx_t nel = nl;
								while (nel < size && base[nel] != '\n') {
									nel++;
								}
								idx_t ns = MmcifTrimStart(base, nl, nel);
								if (ns < nel && base[ns] == ';') {
									val_start = ns;
									value_from_next = true;
									value_consumed_until = nel + 1;
								}
							}
						}
						MmcifSingleCell cell;
						cell.col = col;
						cell.is_null = false;
						if (val_start >= line_end && !value_from_next) {
							cell.off = val_start;
							cell.len = 0;
							cell.is_null = true; // empty value -> NULL
						} else {
							idx_t val_end = line_end;
							if (base[val_start] == ';') {
								// Multi-line semicolon value (may span several lines).
								idx_t p = val_start + 1;
								while (p < size && base[p] != '\n') {
									p++;
								}
								if (p < size) {
									p++;
								}
								while (p < size) {
									if (base[p] == ';') {
										p++;
										break;
									}
									while (p < size && base[p] != '\n') {
										p++;
									}
									if (p < size) {
										p++;
									}
								}
								val_end = p;
								// Skip the consumed value lines so their content isn't
								// re-parsed as tags/stray lines.
								idx_t resume = val_end;
								while (resume < size && base[resume] != '\n') {
									resume++;
								}
								if (resume < size) {
									resume++;
								}
								value_consumed_until = resume;
							} else {
								while (val_end < size && base[val_end] != '\n' && base[val_end] != ';') {
									val_end++;
								}
								// Trim trailing whitespace (single-tag lines carry
								// trailing spaces/tabs before the newline).
								while (val_end > val_start && isspace(static_cast<unsigned char>(base[val_end - 1]))) {
									val_end--;
								}
							}
							cell.off = val_start;
							cell.len = val_end - val_start;
						}
						cur->singles.push_back(cell);
						skip_to = value_consumed_until;
					}
				}
				if (state != LOOP_HEADER) {
					if (state == LOOP_DATA && cur) {
						cur->data_end = line_start;
					}
					state = TOP;
				}
			} else if (MmcifStartsWith(base, s, line_end, "loop_")) {
				if (state == LOOP_DATA && cur) {
					cur->data_end = line_start;
				}
				finalize();
				state = LOOP_HEADER;
			} else if (MmcifStartsWith(base, s, line_end, "data_")) {
				if (state == LOOP_DATA && cur) {
					cur->data_end = line_start;
				}
				finalize();
				if (!indexed) {
					data_block_name.assign(base + s + 5, (line_end - s) - 5);
					indexed = true;
				} else {
					// Later data blocks are ignored (keep-first-block behavior).
					break;
				}
				state = TOP;
			} else if (state == LOOP_HEADER) {
				// First data line of a loop: data begins.
				if (!cur) {
					cur = make_uniq<MmcifCategory>();
					cur->is_loop = true;
				}
				cur->data_start = line_start;
				state = LOOP_DATA;
			} else if (state == LOOP_DATA) {
				// Continuation data line: nothing to record.
			} else {
				// TOP with a stray non-tag line: ignore.
			}
		} else {
			// Blank line: terminates loop data.
			if (state == LOOP_DATA && cur) {
				cur->data_end = line_start;
				state = TOP;
			}
		}
		if (skip_to > line_end + 1) {
			line_start = skip_to;
			skip_to = 0;
		} else {
			line_start = line_end + 1; // advance past '\n' (or past EOF)
		}
	}
	finalize();
}

MmcifCategory *MmcifIndex::FindCategory(const string &name) {
	for (auto &cat : categories) {
		if (StringUtil::CIEquals(cat->name, name)) {
			return cat.get();
		}
	}
	return nullptr;
}

void MmcifIndex::GetCategoryNames(vector<string> &names) {
	for (auto &cat : categories) {
		names.push_back(cat->name);
	}
}

idx_t MmcifIndex::GetRowCount(MmcifCategory &cat) {
	auto known = cat.row_count.load();
	if (known != idx_t(-1)) {
		return known;
	}
	lock_guard<mutex> l(row_count_lock);
	known = cat.row_count.load();
	if (known != idx_t(-1)) {
		return known;
	}
	idx_t count = 0;
	if (cat.is_loop) {
		// Value-scan the loop range without materializing strings.
		MmcifValueCursor cursor(content_data, cat.data_start, cat.data_end);
		idx_t loop_ncols = cat.loop_col_map.size();
		const char *out;
		idx_t len;
		bool is_null;
		idx_t consumed = 0;
		while (cursor.Next(&out, &len, &is_null)) {
			consumed++;
			if (consumed == loop_ncols) {
				consumed = 0;
				count++;
			}
		}
		if (consumed != 0) {
			// Partial trailing row: count it.
			count++;
		}
	} else {
		count = 1; // single-tag categories always have exactly one row
	}
	cat.row_count.store(count);
	return count;
}

} // namespace duckdb
