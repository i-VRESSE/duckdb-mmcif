// File-format policy: remote/gzip/plain reads and the gzip-aware
// write-back.

#pragma once

#include <memory>
#include <string>

#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/typedefs.hpp"
#include "mmcif_write_store.hpp"

namespace duckdb {

class ClientContext;

class MmcifFile {
public:
	// True for paths that reference a remote resource (http/https URL, S3,
	// etc.) rather than a local file. Remote paths are handled by DuckDB's
	// virtual file system (httpfs is autoloaded) and are always read-only.
	static bool IsRemotePath(const string &path);

	// Read the raw bytes of a file. When a client context is available the
	// read goes through DuckDB's virtual file system, so http/https URLs
	// (e.g. https://files.rcsb.org/download/1AMB.cif.gz) and s3:// paths work
	// and the httpfs extension is autoloaded as needed. Falls back to
	// std::ifstream for callers without a context (always local paths). A
	// missing local file throws either way.
	static string Read(const string &path, optional_ptr<ClientContext> context);

	// COMMIT: write the transaction's store back to disk.
	// Paths ending in .gz are written back gzip-compressed (the read path
	// auto-decompresses them, so writing plain text would break the
	// round-trip). Remote paths are rejected (read-only).
	static void Persist(const MmcifWriteStore &store, const string &path, ClientContext &context);
};

} // namespace duckdb
