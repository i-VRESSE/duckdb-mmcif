// Per-category scan + metadata table functions module.
//
// MmcifBindData carries dictionary types and shared read indexes or a write
// store. Global scans bind paths/globs/lists; attached-table scans pre-fill
// bind_data in GetScanFunction, so their bind callback is never called.

#pragma once

#include "duckdb.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"

#include <memory>
#include <string>
#include <vector>

#include "mmcif_index.hpp"
#include "mmcif_write_store.hpp"

namespace duckdb {

class ExtensionLoader;

struct MmcifScanFile {
	string path;
	shared_ptr<MmcifIndex> index;
	MmcifCategory *category = nullptr;
	vector<idx_t> local_to_union;
};

struct MmcifBindData : public FunctionData {
	// Multi-file/global read path: file-local columns map to the union schema.
	vector<MmcifScanFile> files;
	// Dictionary mode binds only paths and a fixed schema, never file contents.
	bool dictionary_schema = false;
	vector<string> paths;
	string table_name;
	bool has_data_block = false;
	string data_block;
	idx_t filename_column = DConstants::INVALID_INDEX;
	std::vector<string> column_names;
	std::vector<LogicalType> column_types;
	// Read-only path: a shared lazy index + category. Rows
	// are streamed from the byte cursor in MmcifScan; nothing is materialized at
	// bind, so LIMIT 10 never copies 2.44M rows.
	shared_ptr<MmcifIndex> index;
	MmcifCategory *category = nullptr;
	// Write-mode path: the catalog's store and the scanned category. The store
	// is shared, not copied; the scan reads the rows present when it starts.
	shared_ptr<MmcifWriteStore> store;
	MmcifWriteCategory *write_category = nullptr;
	optional_ptr<TableCatalogEntry> table_entry; // set only for attached-table scans

	unique_ptr<FunctionData> Copy() const override;
	bool Equals(const FunctionData &other) const override;

	static bool IsNullCell(const string &v) {
		// A cell is NULL when it is empty, ".", or "?" (the stored null forms).
		return v.empty() || v == "." || v == "?";
	}
};

// The per-category scan table function; also returned by MmcifTableEntry as
// the attached-table scan.
TableFunction MmcifScanFunction();

// Read-only bind: point result at the index category and set its column
// names and dictionary types. Throws if the category is absent.
void MmcifBindIndex(MmcifBindData &result, shared_ptr<MmcifIndex> index, const string &table_name);

// Union matching categories, retaining indexes and concrete source paths.
void MmcifBindFiles(MmcifBindData &result, const vector<string> &paths, const vector<shared_ptr<MmcifIndex>> &indexes,
                    const string &table_name);
vector<string> MmcifExpandFiles(ClientContext &context, const Value &input);

// Registers mmcif_scan, mmcif_tables, mmcif_columns, and mmcif_relationships on the loader.
void MmcifRegisterTableFunctions(ExtensionLoader &loader);

} // namespace duckdb
