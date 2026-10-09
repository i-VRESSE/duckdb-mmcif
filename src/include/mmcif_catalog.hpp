// mmcif catalog module: SQLite-style custom Catalog with a single "main"
// schema (MmcifSchemaEntry), per-category table entries (MmcifTableEntry),
// and a write-mode transaction manager (MmcifTransactionManager).
//
// Read-only by default; opened with READ_WRITE TRUE every transaction reads a
// snapshot of the committed MmcifWriteStore and its first DML copies it into a
// private store, which COMMIT writes back (MmcifFile::Persist) and publishes.
// The DML operators themselves live in mmcif_catalog.cpp.

#pragma once

#include "duckdb.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/column_definition.hpp"
#include "duckdb/parser/column_list.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/storage/database_size.hpp"
#include "duckdb/transaction/transaction.hpp"
#include "duckdb/transaction/transaction_manager.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/execution/physical_operator.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "mmcif_index.hpp"
#include "mmcif_write_store.hpp"

namespace duckdb {

class MmcifCatalog;

// ---------------------------------------------------------------------------
// MmcifTableEntry: a real TableCatalogEntry whose ColumnList carries dictionary
// types (so DESCRIBE shows DOUBLE/BIGINT/VARCHAR) and whose GetScanFunction
// returns a per-category scan with pre-filled bind data.
// ---------------------------------------------------------------------------

class MmcifTableEntry : public TableCatalogEntry {
public:
	MmcifTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info, string table_name_p,
	                MmcifCatalog *catalog_p);

	string table_name;
	MmcifCatalog *catalog;

	unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, column_t column_id) override;
	TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) override;
	TableStorageInfo GetStorageInfo(ClientContext &context) override;
};

// ---------------------------------------------------------------------------
// MmcifSchemaEntry: single schema; Scan(TABLE_ENTRY) enumerates the categories
// present in the file; LookupEntry materializes a MmcifTableEntry. Read-only:
// every DDL operation throws.
// ---------------------------------------------------------------------------

class MmcifSchemaEntry : public SchemaCatalogEntry {
public:
	MmcifSchemaEntry(Catalog &catalog, CreateSchemaInfo &info, MmcifCatalog *catalog_p);

	MmcifCatalog *catalog;
	case_insensitive_map_t<unique_ptr<MmcifTableEntry>> tables; // keep entries alive across Scan/LookupEntry

	optional_ptr<CatalogEntry> CreateTable(CatalogTransaction transaction, BoundCreateTableInfo &info) override;
	optional_ptr<CatalogEntry> CreateFunction(CatalogTransaction transaction, CreateFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreateIndex(CatalogTransaction transaction, CreateIndexInfo &info,
	                                       TableCatalogEntry &table) override;
	optional_ptr<CatalogEntry> CreateView(CatalogTransaction transaction, CreateViewInfo &info) override;
	optional_ptr<CatalogEntry> CreateSequence(CatalogTransaction transaction, CreateSequenceInfo &info) override;
	optional_ptr<CatalogEntry> CreateTableFunction(CatalogTransaction transaction,
	                                               CreateTableFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreateCopyFunction(CatalogTransaction transaction,
	                                              CreateCopyFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreatePragmaFunction(CatalogTransaction transaction,
	                                                CreatePragmaFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreateCollation(CatalogTransaction transaction, CreateCollationInfo &info) override;
	optional_ptr<CatalogEntry> CreateType(CatalogTransaction transaction, CreateTypeInfo &info) override;
	void Alter(CatalogTransaction transaction, AlterInfo &info) override;
	void Scan(ClientContext &context, CatalogType type, const std::function<void(CatalogEntry &)> &callback) override;
	void Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) override;
	void DropEntry(ClientContext &context, DropInfo &info) override;
	optional_ptr<CatalogEntry> LookupEntry(CatalogTransaction transaction, const EntryLookupInfo &lookup_info) override;

	MmcifTableEntry &GetTableEntry(CatalogTransaction transaction, const string &entry_name);
};

// ---------------------------------------------------------------------------
// MmcifCatalog: SQLite-style custom Catalog. Single "main" schema. Read-only
// by default; opened with READ_WRITE TRUE it holds the committed
// MmcifWriteStore, which transactions snapshot and replace on COMMIT.
// ---------------------------------------------------------------------------

class MmcifCatalog : public Catalog {
public:
	MmcifCatalog(AttachedDatabase &db_p, string path_p, bool write_mode_p, ClientContext &context,
	             std::optional<string> data_block_p = std::nullopt);

	string path;
	std::optional<string> data_block;
	bool write_mode;
	// Last committed write store (write mode only); guarded by write_lock.
	shared_ptr<MmcifWriteStore> write_store;
	mutex write_lock;
	// Lazy index: built on first resolve (at attach in write mode), then reused
	// for every schema lookup, scan, and metadata query in this catalog. DML
	// never changes columns, so write mode resolves tables through it too.
	// The process-level content cache lives in MmcifIndex::Load.
	shared_ptr<MmcifIndex> index;
	mutex index_lock;

	shared_ptr<MmcifIndex> GetIndex(optional_ptr<ClientContext> context);
	// Columns of a category in the index; nullptr if absent.
	const std::vector<string> *FindColumns(optional_ptr<ClientContext> context, const string &table_name);
	// The calling transaction's store: its snapshot, or its private copy once it wrote.
	shared_ptr<MmcifWriteStore> ReadStore(ClientContext &context);
	// The calling transaction's private store, copied from its snapshot on first use.
	MmcifWriteStore &WriteStore(ClientContext &context);

	void Initialize(bool load_builtin) override;

	string GetCatalogType() override;

	optional_ptr<CatalogEntry> CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) override;

	void ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) override;

	optional_ptr<SchemaCatalogEntry> LookupSchema(CatalogTransaction transaction, const EntryLookupInfo &schema_lookup,
	                                              OnEntryNotFound if_not_found) override;

	PhysicalOperator &PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner, LogicalCreateTable &op,
	                                    PhysicalOperator &plan) override;
	PhysicalOperator &PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
	                             optional_ptr<PhysicalOperator> plan) override;
	PhysicalOperator &PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
	                             PhysicalOperator &plan) override;
	PhysicalOperator &PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
	                             PhysicalOperator &plan) override;

	DatabaseSize GetDatabaseSize(ClientContext &context) override;

	bool InMemory() override;

	string GetDBPath() override;

private:
	void DropSchema(ClientContext &context, DropInfo &info) override;

private:
	unique_ptr<MmcifSchemaEntry> main_schema;
};

// ---------------------------------------------------------------------------
// Transaction manager (DuckTransactionManager requires a DuckCatalog). In write
// mode COMMIT persists the transaction's private store and publishes it;
// ROLLBACK drops it. A transaction that writes after another one committed
// fails with a write-write conflict.
// ---------------------------------------------------------------------------

class MmcifTransaction : public Transaction {
public:
	MmcifTransaction(TransactionManager &manager, ClientContext &context, shared_ptr<MmcifWriteStore> snapshot_p)
	    : Transaction(manager, context), snapshot(snapshot_p), store(std::move(snapshot_p)) {
	}

	// Committed store when the transaction started.
	shared_ptr<MmcifWriteStore> snapshot;
	// == snapshot until the first DML replaces it with a private copy.
	shared_ptr<MmcifWriteStore> store;
};

class MmcifTransactionManager : public TransactionManager {
public:
	MmcifTransactionManager(AttachedDatabase &db, MmcifCatalog &catalog_p);

	MmcifCatalog &catalog;

	Transaction &StartTransaction(ClientContext &context) override;
	ErrorData CommitTransaction(ClientContext &context, Transaction &transaction) override;
	void RollbackTransaction(Transaction &transaction) override;
	void Checkpoint(ClientContext &context, bool force = false) override;

private:
	mutex lock;
	reference_map_t<Transaction, unique_ptr<Transaction>> transactions;
};

// The module's published seam: registers the "mmcif" storage extension (attach
// + transaction-manager factories live in mmcif_catalog.cpp).
void MmcifRegisterStorageExtension(DBConfig &config);

} // namespace duckdb
