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

// A bare token that is a tag or reserved word, not a value: the item before it
// has no value.
static bool MmcifIsKeyword(const char *base, idx_t start, idx_t len) {
	idx_t end = start + len;
	return base[start] == '_' || MmcifStartsWith(base, start, end, "loop_") ||
	       MmcifStartsWith(base, start, end, "data_") || MmcifStartsWith(base, start, end, "save_") ||
	       MmcifStartsWith(base, start, end, "global_") || MmcifStartsWith(base, start, end, "stop_");
}

// Split the "_category.item" tag that starts at `start` into (category, item).
// Returns the offset just past the tag, or 0 when it has no '.'.
static idx_t MmcifSplitTag(const char *base, idx_t start, idx_t end, string &category, string &item) {
	idx_t tag_end = start;
	while (tag_end < end && !isspace(static_cast<unsigned char>(base[tag_end]))) {
		tag_end++;
	}
	idx_t dot = start + 1;
	while (dot < tag_end && base[dot] != '.') {
		dot++;
	}
	if (dot >= tag_end) {
		return 0;
	}
	category.assign(base + start + 1, dot - start - 1);
	item.assign(base + dot + 1, tag_end - dot - 1);
	return tag_end;
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
				idx_t tag_end = MmcifSplitTag(base, s, line_end, cat, item);
				if (tag_end == 0) {
					if (state == LOOP_DATA) {
						end_loop(line_start);
					}
				} else if (state == LOOP_HEADER) {
					// Loop header: this line is a column tag.
					if (!cur || cur->name != cat) {
						finalize();
						cur = make_uniq<MmcifCategory>();
						cur->name = cat;
						cur->is_loop = true;
					}
					cur->loop_col_map.push_back(column_index(item));
				} else {
					// Single-tag item "_cat.item value". A running loop ends here:
					// close it while it is still `cur`, because the finalize() below
					// moves it out of data_end's reach.
					end_loop(line_start);
					if (!cur || cur->name != cat) {
						finalize();
						cur = make_uniq<MmcifCategory>();
						cur->name = cat;
					}
					MmcifSingleCell cell;
					cell.col = column_index(item);
					cell.tag_off = s;
					cell.tag_end = tag_end;
					// The value follows the tag on its line or on a later one (wwPDB
					// writes long values, quoted or as a ';' text field, below the tag).
					const char *out;
					idx_t len;
					bool is_null;
					if (MmcifValueCursor(base, tag_end, size).Next(&out, &len, &is_null) &&
					    !MmcifIsKeyword(base, out - base, len)) {
						cell.off = out - base;
						cell.len = len;
						cell.is_null = is_null;
						// Resume after the value, so a value on later lines is not
						// re-read as tags or stray lines.
						skip_to = MmcifLineEnd(base, size, cell.off + len - 1);
					} else {
						// No value before the next tag or keyword: NULL, anchored
						// right after the tag so an UPDATE can write one there.
						cell.off = tag_end;
						cell.len = 0;
						cell.is_null = true;
					}
					cur->singles.push_back(cell);
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
		if (state == LOOP_DATA && base[line_start] == ';') {
			// A text field in loop data: skip it whole, so its lines (blank,
			// or starting with '_' / "loop_") are not read as structure.
			idx_t p = line_end + 1;
			while (p < size && base[p] != ';') {
				p = MmcifLineEnd(base, size, p);
			}
			skip_to = MmcifLineEnd(base, size, p);
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
