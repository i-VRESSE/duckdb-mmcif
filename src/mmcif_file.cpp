// File-format module: one owner of the remote/gzip/plain policy plus the
// write loader. The duplicated MmcifIsRemotePath/MmcifReadFileContents helpers
// (previously one copy in the index, one in the core) were consolidated here,
// and the catalog's write-back logic moved out of MmcifCatalog::Persist.

#include "mmcif_file.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/gzip_file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"

#include <fstream>
#include <sstream>
#include <string>

#include "mmcif_index.hpp"
#include "mmcif_patch.hpp"

namespace duckdb {

// True for paths that reference a remote resource (http/https URL, S3, etc.)
// rather than a local file.
bool MmcifFile::IsRemotePath(const string &path) {
	return path.find("://") != string::npos;
}

// Read the raw bytes of a file. When a client context is available the read
// goes through DuckDB's virtual file system, so http/https URLs (e.g.
// https://files.rcsb.org/download/1AMB.cif.gz) and s3:// paths work and the
// httpfs extension is autoloaded as needed. Falls back to std::ifstream for
// callers without a context (always local paths). A missing local file throws
// either way.
string MmcifFile::Read(const string &file_name, optional_ptr<ClientContext> context) {
	if (context) {
		auto &fs = FileSystem::GetFileSystem(*context);
		// Pipes (e.g. /dev/stdin, named FIFOs) fail FileExists (not a regular
		// file) but are readable, so exempt them. This enables
		// `cat file.cif | duckdb -c "SELECT * FROM mmcif_scan('/dev/stdin', ...)"`
		// like read_csv supports.
		if (!MmcifFile::IsRemotePath(file_name) && !fs.FileExists(file_name) && !fs.IsPipe(file_name)) {
			throw IOException("mmcif: file not found: %s", file_name);
		}
		auto handle = fs.OpenFile(file_name, FileFlags::FILE_FLAGS_READ);
		string content;
		char buffer[65536];
		while (true) {
			auto n = fs.Read(*handle, buffer, sizeof(buffer));
			if (n <= 0) {
				break;
			}
			content.append(buffer, idx_t(n));
		}
		handle->Close();
		return content;
	}
	std::ifstream in(file_name.c_str(), std::ios::binary);
	if (!in) {
		throw IOException("mmcif: file not found: %s", file_name);
	}
	std::stringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

// Materialize the no-deps mutable write store for a file (gzip'd inputs are
// decompressed by MmcifIndex::Load before the index is built).
shared_ptr<MmcifWriteStore> MmcifFile::LoadWriteStore(const string &file_name, optional_ptr<ClientContext> context) {
	auto index = MmcifIndex::Load(file_name, context);
	return index->Materialize();
}

// COMMIT / detach / checkpoint: write the in-memory store back to disk.
// Paths ending in .gz are written back gzip-compressed (the read path
// auto-decompresses them, so writing plain text would break the round-trip).
void MmcifFile::Persist(const MmcifWriteStore &store, const string &path, ClientContext &context) {
	if (MmcifFile::IsRemotePath(path)) {
		throw IOException("mmcif: cannot write back to remote path %s - remote files are read-only", path);
	}
	auto &fs = FileSystem::GetFileSystem(context);
	// Render first: a failure here happens before anything is touched on disk.
	string content = MmcifPatch::Apply(store);
	// Write to a temp file in the same directory, then rename over the target:
	// each file's write-back is atomic (a crash or failed write leaves the old
	// file intact, never a truncated .cif).
	const string tmp_path = path + ".tmp";
	try {
		FileOpenFlags flags = FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW;
		if (StringUtil::EndsWith(StringUtil::Lower(path), ".gz")) {
			flags.SetCompression(FileCompressionType::GZIP);
		}
		auto handle = fs.OpenFile(tmp_path, flags);
		if (!content.empty()) {
			fs.Write(*handle, data_ptr_cast(&content[0]), content.size());
		}
		// Closing the handle flushes the gzip footer (deflate stream end).
		handle->Close();
		// Atomic move over the target (rename). fs.MoveFile is the ACID rename
		// the storage manager relies on.
		fs.MoveFile(tmp_path, path);
	} catch (...) {
		fs.TryRemoveFile(tmp_path);
		throw;
	}
	// The file on disk changed: drop the process-level index cache entry so a
	// re-attach / reload re-reads instead of materializing a stale store.
	MmcifIndex::InvalidateCache(path);
}

} // namespace duckdb
