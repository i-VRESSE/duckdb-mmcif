#include "mmcif_index.hpp"

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
// Process-level cache: keyed by path, re-attaching the same
// file in one DuckDB session reuses the decompressed content and the pass-1
// index. Local files are invalidated when their stamp (see MmcifFileStamp)
// changes; remote paths are cached by path only (may be stale).
// ---------------------------------------------------------------------------

struct MmcifCacheEntry {
	weak_ptr<MmcifIndex> index;
	string stamp; // MmcifFileStamp at load time; "" for remote paths
};
static mutex g_cache_lock;
static unordered_map<string, MmcifCacheEntry> g_cache;

// Stamp of a local file: identity (device/inode or volume/file index), size,
// and sub-second modification and change times. The change time also moves
// when mtime is restored, so a same-size rewrite within the mtime granularity
// (or an atomic rename over the path) still changes the stamp.
// Returns "" when the file cannot be stat'd.
static string MmcifFileStamp(const string &path) {
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
	return StringUtil::Format("win|%llu|%llu|%llu|%lld|%lld", (unsigned long long)info.dwVolumeSerialNumber,
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
	return StringUtil::Format("posix|%llu|%llu|%lld|%lld.%lld|%lld.%lld", (unsigned long long)st.st_dev,
	                          (unsigned long long)st.st_ino, (long long)st.st_size, (long long)st.st_mtime, mtime_ns,
	                          (long long)st.st_ctime, ctime_ns);
#endif
}

// Local-file staleness check: stamp differs from the one recorded when the
// cached copy was loaded. Remote paths cannot be cheaply stat'd and are cached
// by path only (may be stale).
static bool MmcifFileChanged(const string &path, const string &loaded_stamp) {
	if (MmcifFile::IsRemotePath(path)) {
		return false;
	}
	auto now = MmcifFileStamp(path);
	return now.empty() || now != loaded_stamp; // missing file -> changed
}

shared_ptr<MmcifIndex> MmcifIndex::Load(const string &path, optional_ptr<ClientContext> context) {
	{
		lock_guard<mutex> l(g_cache_lock);
		auto it = g_cache.find(path);
		if (it != g_cache.end()) {
			auto cached = it->second.index.lock();
			if (cached && !MmcifFileChanged(path, it->second.stamp)) {
				return cached;
			}
		}
	}

	// Stamp before reading: a write racing the read then leaves a stamp that no
	// longer matches, so the next Load re-reads instead of trusting this copy.
	string stamp = MmcifFile::IsRemotePath(path) ? string() : MmcifFileStamp(path);
	string text = MmcifFile::Read(path, context);
	if (GZipFileSystem::CheckIsZip(text.data(), text.size())) {
		text = GZipFileSystem::UncompressGZIPString(text);
	}
	auto index = shared_ptr<MmcifIndex>(new MmcifIndex(std::move(text)));
	index->Build();

	lock_guard<mutex> l(g_cache_lock);
	g_cache[path] = MmcifCacheEntry {weak_ptr<MmcifIndex>(index), std::move(stamp)};
	return index;
}

void MmcifIndex::InvalidateCache(const string &path) {
	lock_guard<mutex> l(g_cache_lock);
	g_cache.erase(path);
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
	const char *base = text.data();
	idx_t size = text.size();
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
	// Close a running loop at line_start (the first line that is not loop data).
	auto end_loop = [&](idx_t line_start) {
		if (state == LOOP_DATA && cur) {
			cur->data_end = line_start;
		}
		state = TOP;
	};
	// Index of item in cur's columns, appending it when first seen.
	auto column_index = [&](const string &item) {
		for (idx_t i = 0; i < cur->columns.size(); i++) {
			if (cur->columns[i] == item) {
				return i;
			}
		}
		cur->columns.push_back(item);
		return cur->columns.size() - 1;
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
				// Comment line: skipped. It does NOT end the loop it sits in -
				// mmCIF comments sit between values, so the rows after a
				// comment still belong to that loop.
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
						cur->loop_col_map.push_back(column_index(item));
					} else {
						// Single-tag line "_cat.item value". A running loop ends
						// here: close it while it is still `cur`, because the
						// finalize() below moves it out of data_end's reach.
						end_loop(line_start);
						if (!cur || cur->name != cat) {
							finalize();
							cur = make_uniq<MmcifCategory>();
							cur->name = cat;
						}
						idx_t col = column_index(item);
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
								const char *out;
								idx_t len;
								bool is_null;
								MmcifValueCursor(base, val_start, size).Next(&out, &len, &is_null);
								val_end = val_start + len;
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
				}
				if (state != LOOP_HEADER) {
					end_loop(line_start);
				}
			} else if (MmcifStartsWith(base, s, line_end, "loop_")) {
				end_loop(line_start);
				finalize();
				state = LOOP_HEADER;
			} else if (MmcifStartsWith(base, s, line_end, "data_")) {
				end_loop(line_start);
				finalize();
				if (indexed) {
					// Later data blocks are ignored (keep-first-block behavior);
					// the write-back patch keeps their bytes verbatim.
					break;
				}
				data_block_name.assign(base + s + 5, (line_end - s) - 5);
				indexed = true;
			} else if (MmcifStartsWith(base, s, line_end, "save_")) {
				// save_ frame (save_xxx ... save_): not indexed; stop the
				// current loop. The write-back patch keeps its bytes verbatim.
				end_loop(line_start);
				finalize();
			} else if (state == LOOP_HEADER) {
				// First data line of a loop: data begins.
				if (!cur) {
					cur = make_uniq<MmcifCategory>();
					cur->is_loop = true;
				}
				cur->data_start = line_start;
				state = LOOP_DATA;
			} else {
				// Continuation loop data line, or a stray non-tag line at TOP: ignore.
			}
		} else {
			// Blank line: terminates loop data.
			if (state == LOOP_DATA) {
				end_loop(line_start);
			}
		}
		if (skip_to > line_end + 1) {
			line_start = skip_to;
			skip_to = 0;
		} else {
			line_start = line_end + 1; // advance past '\n' (or past EOF)
		}
	}
	end_loop(size); // a loop that runs to the end of the file
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

vector<string> MmcifIndex::GetCategoryNames() const {
	vector<string> names;
	for (auto &cat : categories) {
		names.push_back(cat->name);
	}
	return names;
}

idx_t MmcifIndex::GetRowCount(MmcifCategory &cat) {
	// Counting is idempotent: a concurrent first call at worst counts twice.
	auto known = cat.row_count.load();
	if (known != idx_t(-1)) {
		return known;
	}
	idx_t count = 0;
	if (cat.is_loop) {
		// Value-scan the loop range without materializing strings.
		MmcifValueCursor cursor(text.data(), cat.data_start, cat.data_end);
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
