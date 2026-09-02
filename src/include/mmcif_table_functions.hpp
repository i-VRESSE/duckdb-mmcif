// Per-category scan + metadata table functions module.
//
// MmcifBindData carries the parsed rows + dictionary types. It is used both as
// the global mmcif_scan(file, table) table function (bind reads the two
// VARCHAR args) and as the attached-table scan (GetScanFunction pre-fills
// bind_data, so bind is never called).

#pragma once

#include "duckdb.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"

#include <memory>
#include <string>
#include <vector>

#include "mmcif_index.hpp"

namespace duckdb {

class ExtensionLoader;

struct MmcifBindData : public FunctionData {
	string file_name;
	string table_name;
	std::vector<string> column_names;
	std::vector<LogicalType> column_types;
	// Read-only path (recommendation 3/5): a shared lazy index + category. Rows
	// are streamed from the byte cursor in MmcifScan; nothing is materialized at
	// bind, so LIMIT 10 never copies 2.44M rows.
	shared_ptr<MmcifIndex> index;
	MmcifCategory *category = nullptr;
	// Write-mode (legacy) path: materialized RCSB rows.
	std::vector<std::vector<string>> rows;
	optional_ptr<TableCatalogEntry> table_entry; // set only for attached-table scans

	unique_ptr<FunctionData> Copy() const override;
	bool Equals(const FunctionData &other) const override;

	static bool IsNullCell(const string &v) {
		// A cell is NULL when it is empty, ".", or "?" (the RCSB stored forms).
		return v.empty() || v == "." || v == "?";
	}
};

// The per-category scan table function; also returned by MmcifTableEntry as
// the attached-table scan.
TableFunction MmcifScanFunction();

// Registers mmcif_scan, mmcif_tables, and mmcif_relationships on the loader.
void MmcifRegisterTableFunctions(ExtensionLoader &loader);

} // namespace duckdb
