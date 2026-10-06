#include "mmcif_index.hpp"

#include "duckdb/common/file_system.hpp"
#include "duckdb/common/gzip_file_system.hpp"
#include "duckdb/common/string_util.hpp"

#include <string>

#include "mmcif_file.hpp"

#ifdef _WIN32
#include "duckdb/common/windows.hpp"
#include "duckdb/common/windows_util.hpp"
#else
#include <sys/stat.h>
#endif

namespace duckdb {

// ---------------------------------------------------------------------------
// Process-level cache (recommendation 2): keyed by path, re-attaching the same
// file in one DuckDB session reuses the decompressed content and the pass-1
// index. Local files are invalidated when their stamp (see MmcifFileStampKey)
// changes; remote paths are cached by path only (may be stale).
// ---------------------------------------------------------------------------

static mutex g_index_cache_lock;
static unordered_map<string, weak_ptr<MmcifIndex>> g_index_cache;

// Per-path file stamps used to skip stat on unchanged files. Hoisted out of
// MmcifFileChanged so InvalidateCache can drop a stamp after a write-back.
static mutex g_stamp_lock;
static case_insensitive_map_t<string> g_stamps;

// Context-free stamp of a local file: identity (device/inode or volume/file
// index), size, and sub-second modification and change times. The change time
// also moves when mtime is restored, so a same-size rewrite within the mtime
// granularity (or an atomic rename over the path) still changes the stamp.
// Returns "" when the file cannot be stat'd.
static string MmcifLocalFileStamp(const string &path) {
#ifdef _WIN32
	auto wpath = WindowsUtil::UTF8ToUnicode(path.c_str());
	HANDLE handle =
	    CreateFileW(wpath.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
	                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (handle == INVALID_HANDLE_VALUE) {
		return "";
	}
	BY_HANDLE_FILE_INFORMATION info;
	FILE_BASIC_INFO basic;
	bool ok = GetFileInformationByHandle(handle, &info) &&
	          GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic));
	CloseHandle(handle);
	if (!ok) {
		return "";
	}
	return StringUtil::Format("%s|win|%llu|%llu|%llu|%lld|%lld", path, (unsigned long long)info.dwVolumeSerialNumber,
	                          ((unsigned long long)info.nFileIndexHigh << 32) | info.nFileIndexLow,
	                          ((unsigned long long)info.nFileSizeHigh << 32) | info.nFileSizeLow,
	                          (long long)basic.LastWriteTime.QuadPart, (long long)basic.ChangeTime.QuadPart);
#else
	struct stat st;
	if (stat(path.c_str(), &st) != 0) {
		return "";
	}
#ifdef __APPLE__
	auto mtime_ns = (long long)st.st_mtimespec.tv_nsec;
	auto ctime_ns = (long long)st.st_ctimespec.tv_nsec;
#else
	auto mtime_ns = (long long)st.st_mtim.tv_nsec;
	auto ctime_ns = (long long)st.st_ctim.tv_nsec;
#endif
	return StringUtil::Format("%s|posix|%llu|%llu|%lld|%lld.%lld|%lld.%lld", path, (unsigned long long)st.st_dev,
	                          (unsigned long long)st.st_ino, (long long)st.st_size, (long long)st.st_mtime, mtime_ns,
	                          (long long)st.st_ctime, ctime_ns);
#endif
}

// Stamp a local file. Returns "" when the file is missing (treated as
// "changed"). With a context the stamp comes from DuckDB's VFS (mtime+size);
// without one (write-mode loads) a direct OS stat is used.
static string MmcifFileStampKey(const string &path, optional_ptr<ClientContext> context) {
	if (context) {
		auto &fs = FileSystem::GetFileSystem(*context);
		if (!fs.FileExists(path)) {
			return "";
		}
		auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
		auto mtime = fs.GetLastModifiedTime(*handle);
		auto size = fs.GetFileSize(*handle);
		return StringUtil::Format("%s|%ld|%lld", path, (long)mtime.value, (long long)size);
	}
	return MmcifLocalFileStamp(path);
}

// Local-file staleness check: stamp differs from the one recorded when the
// cached copy was loaded. Remote paths cannot be cheaply stat'd and are cached
// by path only (may be stale).
static bool MmcifFileChanged(const string &path, optional_ptr<ClientContext> context) {
	if (MmcifFile::IsRemotePath(path)) {
		return false;
	}
	auto key = MmcifFileStampKey(path, context);
	if (key.empty()) {
		return true; // file missing -> treat as changed
	}
	lock_guard<mutex> l(g_stamp_lock);
	auto it = g_stamps.find(path);
	return it == g_stamps.end() || it->second != key;
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

	// Stamp before reading: a write racing the read then leaves a stamp that no
	// longer matches, so the next Load re-reads instead of trusting this copy.
	string stamp = MmcifFile::IsRemotePath(path) ? string() : MmcifFileStampKey(path, context);
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
	idx_t original_text_size = text.size();
	text += "\ndata_zzz_prototype\n#\n";

	auto index = shared_ptr<MmcifIndex>(new MmcifIndex(std::move(raw), std::move(text), original_text_size));
	index->Build();

	lock_guard<mutex> l(g_index_cache_lock);
	g_index_cache[path] = weak_ptr<MmcifIndex>(index);
	lock_guard<mutex> sl(g_stamp_lock);
	if (stamp.empty()) {
		g_stamps.erase(path);
	} else {
		g_stamps[path] = stamp;
	}
	return index;
}

void MmcifIndex::InvalidateCache(const string &path) {
	lock_guard<mutex> l(g_index_cache_lock);
	g_index_cache.erase(path);
	lock_guard<mutex> sl(g_stamp_lock);
	g_stamps.erase(path);
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

	// Comment lines seen since the previous structural element. A comment is
	// anchored to the element that follows it, which is only known once that
	// line is read, so they pile up here until attach_comments() can bind them.
	vector<string> pending;

	auto attach_comments = [&](MmcifCommentAnchor anchor, const string &category, const string &item, idx_t row) {
		if (pending.empty()) {
			return;
		}
		for (auto &text : pending) {
			MmcifComment comment;
			comment.anchor = anchor;
			comment.category = category;
			comment.item = item;
			comment.row = row;
			comment.text = std::move(text);
			comments.push_back(std::move(comment));
		}
		pending.clear();
	};

	// Row index of the loop row that starts at `offset`: value-count the loop
	// range in front of it. The count resumes where the previous call stopped,
	// so a loop with many comments is still scanned once, not once per comment.
	// Nothing is counted until a comment actually needs it, keeping the pass-1
	// scan of the (common) comment-free loop free of any value parsing.
	idx_t counted_data_start = 0; // the loop range the counters below belong to
	idx_t counted_offset = 0;
	idx_t counted_values = 0;
	auto loop_row_at = [&](const MmcifCategory &cat, idx_t offset) -> idx_t {
		idx_t loop_ncols = cat.loop_col_map.size();
		if (loop_ncols == 0) {
			return 0;
		}
		if (counted_data_start != cat.data_start || offset < counted_offset) {
			counted_data_start = cat.data_start; // a different loop: restart
			counted_offset = cat.data_start;
			counted_values = 0;
		}
		if (offset > counted_offset) {
			MmcifValueCursor counter(content_data, counted_offset, offset);
			const char *out;
			idx_t len;
			bool is_null;
			while (counter.Next(&out, &len, &is_null)) {
				counted_values++;
			}
			counted_offset = offset;
		}
		return counted_values / loop_ncols;
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
				// Comment line: kept verbatim and anchored to whatever
				// structural element follows it. It does NOT end the loop it
				// sits in - mmCIF comments sit between values, so the rows
				// after a comment still belong to that loop.
				pending.push_back(string(base + line_start, line_end - line_start));
			} else if (c == '_') {
				// Tag line.
				string cat, item;
				bool starts_category = false;
				if (MmcifSplitTag(base, s, line_end - s, cat, item)) {
					if (state == LOOP_HEADER) {
						// Loop header: this line is a column tag.
						starts_category = !cur || cur->name != cat;
						if (starts_category) {
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
						// Single-tag line "_cat.item value". A running loop ends
						// here: close it while it is still `cur`, because the
						// finalize() below moves it out of data_end's reach.
						if (state == LOOP_DATA && cur) {
							cur->data_end = line_start;
							state = TOP;
						}
						starts_category = !cur || cur->name != cat;
						if (starts_category) {
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
							// "." / "?" missing markers -> NULL, matching the loop
							// cursor (quoted values have len > 1 and stay literal).
							if (cell.len == 1 && (base[val_start] == '.' || base[val_start] == '?')) {
								cell.is_null = true;
							}
						}
						cur->singles.push_back(cell);
						skip_to = value_consumed_until;
					}
					// The comments in front of this tag line sit on it: the
					// first line of a category, or this item's own line.
					if (starts_category) {
						attach_comments(MmcifCommentAnchor::CATEGORY, cat, string(), 0);
					} else {
						attach_comments(MmcifCommentAnchor::ITEM, cat, item, 0);
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
					// Comments above the first data_ line are the block preamble.
					attach_comments(MmcifCommentAnchor::BLOCK_HEADER, string(), string(), 0);
				} else {
					// Later data blocks are ignored (keep-first-block behavior),
					// but record them so write-mode attach refuses to write
					// back a file whose extra blocks would be silently dropped.
					// The synthetic "data_zzz_prototype" flush block appended by
					// Load is a parser artifact, not file content.
					if (line_start < original_text_size) {
						has_multiple_blocks = true;
					}
					// This break ends the first data block: anything still
					// pending is the block's trailing comments.
					attach_comments(MmcifCommentAnchor::TRAILER, string(), string(), 0);
					break;
				}
				state = TOP;
			} else if (MmcifStartsWith(base, s, line_end, "save_")) {
				// save_ frame (save_xxx ... save_): not representable in the
				// write store, so flag it and stop the current loop.
				has_save_frames = true;
				if (state == LOOP_DATA && cur) {
					cur->data_end = line_start;
				}
				finalize();
				state = TOP;
			} else if (state == LOOP_HEADER) {
				// First data line of a loop: data begins. Comments held back
				// since the loop header sit in front of the first row.
				if (!cur) {
					cur = make_uniq<MmcifCategory>();
					cur->is_loop = true;
				}
				attach_comments(MmcifCommentAnchor::LOOP_ROW, cur->name, string(), 0);
				cur->data_start = line_start;
				state = LOOP_DATA;
			} else if (state == LOOP_DATA) {
				// Continuation data line. A comment held back in front of it
				// sits inside the loop, anchored to the row this line starts.
				// The row count is only computed when comments are actually
				// pending, so a plain loop scan stays a single pass.
				if (cur && !pending.empty()) {
					attach_comments(MmcifCommentAnchor::LOOP_ROW, cur->name, string(), loop_row_at(*cur, line_start));
				}
			} else {
				// TOP with a stray non-tag line: ignore.
			}
		} else {
			// Blank line: terminates loop data. Pending comments stay pending -
			// they are emitted in front of the next element, which keeps them
			// visually where they were: between the loop and what follows it.
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
	// Out of file: whatever is still pending trails the data block.
	attach_comments(MmcifCommentAnchor::TRAILER, string(), string(), 0);
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
