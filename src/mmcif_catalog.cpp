// mmcif catalog module implementation: schema/table entries, the custom
// Catalog, the write-mode DML operators, the transaction manager, and the
// storage-extension registration.
//
// Write-mode DML operators: custom physical sinks that read the input
// chunk and apply row-level mutations to the transaction's private write store.
// row_id == physical store row index (scans emit row_id = row index).

#include "mmcif_catalog.hpp"

#include "duckdb/common/file_system.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/operator/numeric_cast.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/physical_operator_states.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/operator/logical_delete.hpp"
#include "duckdb/planner/operator/logical_insert.hpp"
#include "duckdb/planner/operator/logical_update.hpp"
#include "duckdb/storage/storage_extension.hpp"

#include "mmcif_dictionary.hpp"
#include "mmcif_file.hpp"
#include "mmcif_patch.hpp"
#include "mmcif_table_functions.hpp"

namespace duckdb {

// Resolve a category in the write store; throws if absent or column-less.
static MmcifWriteCategory *MmcifGetWriteCategory(MmcifWriteStore &store, const string &table_name) {
	auto cat = store.FindCategory(table_name);
	if (!cat || cat->columns.empty()) {
		throw BinderException("mmcif: category '%s' not present in block '%s'", table_name.c_str(),
		                      store.data_block_name.c_str());
	}
	return cat;
}

// NULL -> empty cell -> written back as "?"
static string MmcifCellToString(const Vector &vec, idx_t row, const MmcifWriteCategory &cat, idx_t col) {
	auto val = vec.GetValue(row);
	if (val.IsNull()) {
		return "";
	}
	auto str = val.ToString();
	// Only a value holding ';' can be unwritable; fail the statement, not COMMIT.
	if (str.find(';') != string::npos) {
		MmcifPatch::CheckValue(str, cat.name + "." + cat.columns[col]);
	}
	return str;
}

// ---------------------------------------------------------------------------
// Write-mode DML operators
// ---------------------------------------------------------------------------

struct MmcifWriteGlobalState : public GlobalSinkState {
	idx_t count = 0;
	// DELETE row ids are accumulated across sink chunks and applied once in Combine:
	// row ids refer to positions in the table as it was when the scan started, so
	// applying them chunk-by-chunk would use stale indices after the first shrink.
	std::vector<idx_t> delete_indices;
};

// Shared sink/source plumbing: a single-threaded, order-dependent sink that
// returns the affected row count as one BIGINT.
class MmcifWriteOperator : public PhysicalOperator {
public:
	MmcifWriteOperator(PhysicalPlan &physical_plan, PhysicalOperatorType type, vector<LogicalType> types,
	                   idx_t estimated_cardinality, MmcifCatalog &catalog, string table_name)
	    : PhysicalOperator(physical_plan, type, std::move(types), estimated_cardinality), catalog(catalog),
	      table_name(std::move(table_name)) {
	}

	MmcifCatalog &catalog;
	string table_name;

	bool IsSink() const override {
		return true;
	}
	bool ParallelSink() const override {
		return false;
	}
	bool SinkOrderDependent() const override {
		return true;
	}
	bool IsSource() const override {
		return true;
	}

	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override {
		return make_uniq<MmcifWriteGlobalState>();
	}
	unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) const override {
		return make_uniq<LocalSinkState>();
	}
	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override {
		return make_uniq<GlobalSourceState>();
	}
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override {
		auto &g = sink_state->Cast<MmcifWriteGlobalState>();
		chunk.SetCardinality(1);
		chunk.SetValue(0, 0, Value::BIGINT(NumericCast<int64_t>(g.count)));
		return SourceResultType::FINISHED;
	}
};

class MmcifInsertOperator : public MmcifWriteOperator {
public:
	MmcifInsertOperator(PhysicalPlan &physical_plan, vector<LogicalType> types, idx_t estimated_cardinality,
	                    MmcifCatalog &catalog, string table_name, vector<idx_t> column_index_map)
	    : MmcifWriteOperator(physical_plan, PhysicalOperatorType::INSERT, std::move(types), estimated_cardinality,
	                         catalog, std::move(table_name)),
	      column_index_map(std::move(column_index_map)) {
	}

	vector<idx_t> column_index_map; // empty => positional insert into all columns

	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override {
		auto &gstate = input.global_state.Cast<MmcifWriteGlobalState>();
		auto &store = catalog.WriteStore(context.client);
		auto cat = MmcifGetWriteCategory(store, table_name);
		idx_t num_cols = cat->columns.size();
		chunk.Flatten();
		for (idx_t r = 0; r < chunk.size(); r++) {
			std::vector<string> row(num_cols, "");
			for (idx_t c = 0; c < num_cols; c++) {
				if (column_index_map.empty()) {
					row[c] = MmcifCellToString(chunk.data[c], r, *cat, c);
					continue;
				}
				auto mapped = column_index_map[c];
				if (mapped == DConstants::INVALID_INDEX) {
					continue; // unspecified column -> NULL
				}
				row[c] = MmcifCellToString(chunk.data[mapped], r, *cat, c);
			}
			store.AddRow(*cat, row);
			gstate.count++;
		}
		return SinkResultType::NEED_MORE_INPUT;
	}
};

class MmcifDeleteOperator : public MmcifWriteOperator {
public:
	MmcifDeleteOperator(PhysicalPlan &physical_plan, vector<LogicalType> types, idx_t estimated_cardinality,
	                    MmcifCatalog &catalog, string table_name, idx_t row_id_index)
	    : MmcifWriteOperator(physical_plan, PhysicalOperatorType::DELETE_OPERATOR, std::move(types),
	                         estimated_cardinality, catalog, std::move(table_name)),
	      row_id_index(row_id_index) {
	}

	idx_t row_id_index;

	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override {
		auto &gstate = input.global_state.Cast<MmcifWriteGlobalState>();
		chunk.Flatten();
		auto &row_ids = chunk.data[row_id_index];
		auto row_data = FlatVector::GetData<int64_t>(row_ids);
		for (idx_t r = 0; r < chunk.size(); r++) {
			gstate.delete_indices.push_back(NumericCast<idx_t>(row_data[r]));
		}
		return SinkResultType::NEED_MORE_INPUT;
	}
	SinkCombineResultType Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const override {
		auto &gstate = input.global_state.Cast<MmcifWriteGlobalState>();
		std::vector<idx_t> indices;
		indices.swap(gstate.delete_indices);
		sort(indices.begin(), indices.end());
		indices.erase(unique(indices.begin(), indices.end()), indices.end());
		if (!indices.empty()) {
			auto &store = catalog.WriteStore(context.client);
			auto cat = MmcifGetWriteCategory(store, table_name);
			store.DeleteRows(*cat, indices);
			gstate.count += indices.size();
		}
		return SinkCombineResultType::FINISHED;
	}
};

class MmcifUpdateOperator : public MmcifWriteOperator {
public:
	MmcifUpdateOperator(PhysicalPlan &physical_plan, vector<LogicalType> types, idx_t estimated_cardinality,
	                    MmcifCatalog &catalog, string table_name, vector<idx_t> columns, vector<idx_t> expr_indices)
	    : MmcifWriteOperator(physical_plan, PhysicalOperatorType::UPDATE, std::move(types), estimated_cardinality,
	                         catalog, std::move(table_name)),
	      columns(std::move(columns)), expr_indices(std::move(expr_indices)) {
	}

	vector<idx_t> columns;      // physical column index to update
	vector<idx_t> expr_indices; // chunk index holding the new value

	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override {
		auto &gstate = input.global_state.Cast<MmcifWriteGlobalState>();
		auto &store = catalog.WriteStore(context.client);
		auto cat = MmcifGetWriteCategory(store, table_name);
		chunk.Flatten();
		auto &row_ids = chunk.data[chunk.ColumnCount() - 1];
		auto row_data = FlatVector::GetData<int64_t>(row_ids);
		for (idx_t r = 0; r < chunk.size(); r++) {
			for (idx_t i = 0; i < columns.size(); i++) {
				store.UpdateCell(*cat, NumericCast<idx_t>(row_data[r]), columns[i],
				                 MmcifCellToString(chunk.data[expr_indices[i]], r, *cat, columns[i]));
			}
		}
		gstate.count += chunk.size();
		return SinkResultType::NEED_MORE_INPUT;
	}
};

// ---------------------------------------------------------------------------
// MmcifTableEntry
// ---------------------------------------------------------------------------

MmcifTableEntry::MmcifTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info,
                                 string table_name_p, MmcifCatalog *catalog_p)
    : TableCatalogEntry(catalog, schema, info), table_name(std::move(table_name_p)), catalog(catalog_p) {
}

unique_ptr<BaseStatistics> MmcifTableEntry::GetStatistics(ClientContext &context, column_t column_id) {
	// Real min/max stats require a full category scan, which we deliberately
	// avoid to keep LIMIT/schema queries cheap. Stats are skipped; the planner
	// still gets exact cardinality from GetStorageInfo.
	return nullptr;
}

TableFunction MmcifTableEntry::GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) {
	auto result = make_uniq<MmcifBindData>();
	result->table_entry = this;

	if (catalog->write_mode) {
		// Write mode: the transaction's materialized store rows.
		auto store = catalog->ReadStore(context);
		auto cat = MmcifGetWriteCategory(*store, table_name);
		result->column_names = cat->columns;
		result->store = store;
		result->write_category = cat;
		for (auto &col : result->column_names) {
			result->column_types.push_back(DictionaryIndex::Get().LookupType(table_name, col));
		}
	} else {
		// Read-only: shared lazy index, streamed scan. No materialization.
		MmcifBindIndex(*result, catalog->GetIndex(&context), table_name);
	}

	bind_data = std::move(result);
	auto scan_function = MmcifScanFunction();
	// Expose the table entry so DELETE/UPDATE binder checks pass in write mode.
	scan_function.get_bind_info = [](const optional_ptr<FunctionData> bind_data) -> BindInfo {
		auto &bind = bind_data->Cast<MmcifBindData>();
		if (!bind.table_entry) {
			return BindInfo(ScanType::EXTERNAL);
		}
		return BindInfo(const_cast<TableCatalogEntry &>(*bind.table_entry));
	};
	return scan_function;
}

TableStorageInfo MmcifTableEntry::GetStorageInfo(ClientContext &context) {
	TableStorageInfo result;
	if (catalog->write_mode) {
		auto cat = catalog->ReadStore(context)->FindCategory(table_name);
		result.cardinality = cat ? cat->rows.size() : 0;
	} else {
		auto index = catalog->GetIndex(&context);
		auto cat = index->FindCategory(table_name);
		// Exact cardinality, computed lazily once per category (cached).
		result.cardinality = cat ? index->GetRowCount(*cat) : 0;
	}
	return result;
}

// ---------------------------------------------------------------------------
// MmcifSchemaEntry
// ---------------------------------------------------------------------------

MmcifSchemaEntry::MmcifSchemaEntry(Catalog &catalog, CreateSchemaInfo &info, MmcifCatalog *catalog_p)
    : SchemaCatalogEntry(catalog, info), catalog(catalog_p) {
}

optional_ptr<CatalogEntry> MmcifSchemaEntry::CreateTable(CatalogTransaction transaction, BoundCreateTableInfo &info) {
	throw BinderException("mmcif databases are read-only - cannot CREATE TABLE");
}
optional_ptr<CatalogEntry> MmcifSchemaEntry::CreateFunction(CatalogTransaction transaction, CreateFunctionInfo &info) {
	throw BinderException("mmcif databases are read-only - cannot CREATE FUNCTION");
}
optional_ptr<CatalogEntry> MmcifSchemaEntry::CreateIndex(CatalogTransaction transaction, CreateIndexInfo &info,
                                                         TableCatalogEntry &table) {
	throw BinderException("mmcif databases are read-only - cannot CREATE INDEX");
}
optional_ptr<CatalogEntry> MmcifSchemaEntry::CreateView(CatalogTransaction transaction, CreateViewInfo &info) {
	throw BinderException("mmcif databases are read-only - cannot CREATE VIEW");
}
optional_ptr<CatalogEntry> MmcifSchemaEntry::CreateSequence(CatalogTransaction transaction, CreateSequenceInfo &info) {
	throw BinderException("mmcif databases are read-only - cannot CREATE SEQUENCE");
}
optional_ptr<CatalogEntry> MmcifSchemaEntry::CreateTableFunction(CatalogTransaction transaction,
                                                                 CreateTableFunctionInfo &info) {
	throw BinderException("mmcif databases are read-only - cannot CREATE FUNCTION");
}
optional_ptr<CatalogEntry> MmcifSchemaEntry::CreateCopyFunction(CatalogTransaction transaction,
                                                                CreateCopyFunctionInfo &info) {
	throw BinderException("mmcif databases are read-only - cannot CREATE COPY FUNCTION");
}
optional_ptr<CatalogEntry> MmcifSchemaEntry::CreatePragmaFunction(CatalogTransaction transaction,
                                                                  CreatePragmaFunctionInfo &info) {
	throw BinderException("mmcif databases are read-only - cannot CREATE PRAGMA FUNCTION");
}
optional_ptr<CatalogEntry> MmcifSchemaEntry::CreateCollation(CatalogTransaction transaction,
                                                             CreateCollationInfo &info) {
	throw BinderException("mmcif databases are read-only - cannot CREATE COLLATION");
}
optional_ptr<CatalogEntry> MmcifSchemaEntry::CreateType(CatalogTransaction transaction, CreateTypeInfo &info) {
	throw BinderException("mmcif databases are read-only - cannot CREATE TYPE");
}

void MmcifSchemaEntry::Alter(CatalogTransaction transaction, AlterInfo &info) {
	throw BinderException("mmcif databases are read-only - cannot ALTER");
}

static ColumnDefinition MmcifColumnDefinition(DictionaryIndex &dictionary, const string &table_name,
                                              const string &column_name) {
	ColumnDefinition result(column_name, dictionary.LookupType(table_name, column_name));
	result.SetComment(Value(dictionary.GetItemUrl(table_name, column_name)));
	return result;
}

MmcifTableEntry &MmcifSchemaEntry::GetTableEntry(CatalogTransaction transaction, const string &entry_name) {
	// Cache: reuse an existing entry instead of replacing it. Scans keep a raw
	// pointer to the returned MmcifTableEntry in their bind data; replacing the
	// map entry would free that object while still referenced (e.g. a DELETE
	// whose WHERE subquery scans the same table twice), leaving a dangling
	// table_entry that segfaults the DELETE/UPDATE binder.
	auto existing = tables.find(entry_name);
	if (existing != tables.end()) {
		return *existing->second;
	}
	auto *columns = this->catalog->FindColumns(transaction.context, entry_name);
	if (!columns) {
		throw BinderException("mmcif: category '%s' not present in file '%s'", entry_name.c_str(),
		                      this->catalog->path.c_str());
	}
	CreateTableInfo info(*this, entry_name);
	auto &dict = DictionaryIndex::Get();
	info.comment = Value(dict.GetCategoryUrl(entry_name));
	for (auto &col : *columns) {
		info.columns.AddColumn(MmcifColumnDefinition(dict, entry_name, col));
	}
	auto entry = make_uniq<MmcifTableEntry>(ParentCatalog(), *this, info, entry_name, this->catalog);
	auto *result = entry.get();
	tables[entry_name] = std::move(entry);
	return *result;
}

void MmcifSchemaEntry::Scan(ClientContext &context, CatalogType type,
                            const std::function<void(CatalogEntry &)> &callback) {
	if (type != CatalogType::TABLE_ENTRY) {
		return; // mmcif exposes only tables
	}
	auto categories = catalog->GetIndex(&context)->GetCategoryNames();
	auto transaction = GetCatalogTransaction(context);
	for (auto &category : categories) {
		callback(GetTableEntry(transaction, category));
	}
}
void MmcifSchemaEntry::Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) {
	throw InternalException("MmcifSchemaEntry::Scan without context");
}

void MmcifSchemaEntry::DropEntry(ClientContext &context, DropInfo &info) {
	throw BinderException("mmcif databases are read-only - cannot DROP");
}

optional_ptr<CatalogEntry> MmcifSchemaEntry::LookupEntry(CatalogTransaction transaction,
                                                         const EntryLookupInfo &lookup_info) {
	if (lookup_info.GetCatalogType() != CatalogType::TABLE_ENTRY) {
		return nullptr;
	}
	auto entry_name = lookup_info.GetEntryName();
	if (!catalog->FindColumns(transaction.context, entry_name)) {
		return nullptr;
	}
	return &GetTableEntry(transaction, entry_name);
}

// ---------------------------------------------------------------------------
// MmcifCatalog
// ---------------------------------------------------------------------------

MmcifCatalog::MmcifCatalog(AttachedDatabase &db_p, string path_p, bool write_mode_p, ClientContext &context,
                           std::optional<string> data_block_p)
    : Catalog(db_p), path(std::move(path_p)), data_block(std::move(data_block_p)), write_mode(write_mode_p) {
	if (write_mode || data_block) {
		index = MmcifIndex::Load(path, &context, data_block);
		if (write_mode) {
			write_store = index->Materialize();
		}
	}
}

const std::vector<string> *MmcifCatalog::FindColumns(optional_ptr<ClientContext> context, const string &table_name) {
	auto cat = GetIndex(context)->FindCategory(table_name);
	return cat ? &cat->columns : nullptr;
}

shared_ptr<MmcifIndex> MmcifCatalog::GetIndex(optional_ptr<ClientContext> context) {
	lock_guard<mutex> l(index_lock);
	if (!index) {
		index = MmcifIndex::Load(path, context, data_block);
	}
	return index;
}

shared_ptr<MmcifWriteStore> MmcifCatalog::ReadStore(ClientContext &context) {
	return Transaction::Get(context, *this).Cast<MmcifTransaction>().store;
}

MmcifWriteStore &MmcifCatalog::WriteStore(ClientContext &context) {
	auto &transaction = Transaction::Get(context, *this).Cast<MmcifTransaction>();
	if (transaction.store == transaction.snapshot) {
		{
			lock_guard<mutex> l(write_lock);
			if (write_store != transaction.snapshot) {
				throw TransactionException("mmcif: write-write conflict on '%s' - another transaction committed first",
				                           path);
			}
		}
		// ponytail: copies the whole store once per writing transaction (same O(file)
		// as the write-back); copy only touched categories if this shows in profiles.
		transaction.store = make_shared_ptr<MmcifWriteStore>(*transaction.snapshot);
	}
	return *transaction.store;
}

void MmcifCatalog::Initialize(bool load_builtin) {
	CreateSchemaInfo info;
	main_schema = make_uniq<MmcifSchemaEntry>(*this, info, this);
}

string MmcifCatalog::GetCatalogType() {
	return "mmcif";
}

optional_ptr<CatalogEntry> MmcifCatalog::CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) {
	if (info.schema == DEFAULT_SCHEMA) {
		return main_schema.get();
	}
	throw BinderException("mmcif databases only have a single schema");
}

void MmcifCatalog::ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) {
	callback(*main_schema);
}

optional_ptr<SchemaCatalogEntry> MmcifCatalog::LookupSchema(CatalogTransaction transaction,
                                                            const EntryLookupInfo &schema_lookup,
                                                            OnEntryNotFound if_not_found) {
	auto &schema_name = schema_lookup.GetEntryName();
	if (schema_name == DEFAULT_SCHEMA || schema_name == INVALID_SCHEMA) {
		return main_schema.get();
	}
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		return nullptr;
	}
	throw BinderException("mmcif databases only have a single schema - \"%s\"", std::string(DEFAULT_SCHEMA));
}

PhysicalOperator &MmcifCatalog::PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner,
                                                  LogicalCreateTable &op, PhysicalOperator &plan) {
	throw NotImplementedException("mmcif databases are read-only - cannot CREATE TABLE AS");
}
PhysicalOperator &MmcifCatalog::PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
                                           optional_ptr<PhysicalOperator> plan) {
	if (!write_mode) {
		throw BinderException("mmcif databases are read-only - cannot INSERT");
	}
	if (op.return_chunk) {
		throw NotImplementedException("mmcif write mode does not support INSERT RETURNING");
	}
	vector<idx_t> col_map;
	for (auto &mapped : op.column_index_map) {
		col_map.push_back(mapped);
	}
	auto &insert =
	    planner.Make<MmcifInsertOperator>(op.types, op.estimated_cardinality, *this, op.table.name, std::move(col_map));
	if (plan) {
		insert.children.push_back(*plan);
	}
	return insert;
}
PhysicalOperator &MmcifCatalog::PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
                                           PhysicalOperator &plan) {
	if (!write_mode) {
		throw BinderException("mmcif databases are read-only - cannot DELETE");
	}
	if (op.return_chunk) {
		throw NotImplementedException("mmcif write mode does not support DELETE RETURNING");
	}
	auto &bound_ref = op.expressions[0]->Cast<BoundReferenceExpression>();
	auto &del =
	    planner.Make<MmcifDeleteOperator>(op.types, op.estimated_cardinality, *this, op.table.name, bound_ref.index);
	del.children.push_back(plan);
	return del;
}
PhysicalOperator &MmcifCatalog::PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
                                           PhysicalOperator &plan) {
	if (!write_mode) {
		throw BinderException("mmcif databases are read-only - cannot UPDATE");
	}
	if (op.return_chunk) {
		throw NotImplementedException("mmcif write mode does not support UPDATE RETURNING");
	}
	vector<idx_t> columns;
	vector<idx_t> expr_indices;
	for (idx_t i = 0; i < op.columns.size(); i++) {
		if (op.expressions[i]->GetExpressionType() != ExpressionType::BOUND_REF) {
			throw NotImplementedException("mmcif write mode supports only direct SET expressions");
		}
		columns.push_back(op.columns[i].index);
		auto &ref = op.expressions[i]->Cast<BoundReferenceExpression>();
		expr_indices.push_back(ref.index);
	}
	auto &update = planner.Make<MmcifUpdateOperator>(op.types, op.estimated_cardinality, *this, op.table.name,
	                                                 std::move(columns), std::move(expr_indices));
	update.children.push_back(plan);
	return update;
}

DatabaseSize MmcifCatalog::GetDatabaseSize(ClientContext &context) {
	DatabaseSize result;
	result.total_blocks = 0;
	result.block_size = 0;
	result.free_blocks = 0;
	result.used_blocks = 0;
	result.bytes = 0;
	result.wal_size = idx_t(-1);
	return result;
}

bool MmcifCatalog::InMemory() {
	// The catalog metadata is materialized in memory, but the database itself is
	// backed by the attached mmCIF file. DuckDB uses this return value to decide
	// whether duckdb_databases().path should be NULL.
	return false;
}

string MmcifCatalog::GetDBPath() {
	return path;
}

void MmcifCatalog::DropSchema(ClientContext &context, DropInfo &info) {
	throw BinderException("mmcif databases do not support dropping schemas");
}

// ---------------------------------------------------------------------------
// MmcifTransactionManager
// ---------------------------------------------------------------------------

MmcifTransactionManager::MmcifTransactionManager(AttachedDatabase &db, MmcifCatalog &catalog_p)
    : TransactionManager(db), catalog(catalog_p) {
}

Transaction &MmcifTransactionManager::StartTransaction(ClientContext &context) {
	shared_ptr<MmcifWriteStore> snapshot;
	{
		lock_guard<mutex> l(catalog.write_lock);
		snapshot = catalog.write_store;
	}
	auto transaction = make_uniq<MmcifTransaction>(*this, context, std::move(snapshot));
	auto &result = *transaction;
	lock_guard<mutex> l(lock);
	transactions[result] = std::move(transaction);
	return result;
}

ErrorData MmcifTransactionManager::CommitTransaction(ClientContext &context, Transaction &transaction) {
	auto &mmcif_transaction = transaction.Cast<MmcifTransaction>();
	ErrorData error;
	auto &store = mmcif_transaction.store;
	// Read-only / no-op transactions never rewrite the file.
	if (store != mmcif_transaction.snapshot && store->IsDirty()) {
		lock_guard<mutex> l(catalog.write_lock);
		try {
			if (catalog.write_store != mmcif_transaction.snapshot) {
				throw TransactionException("mmcif: write-write conflict on '%s' - another transaction committed first",
				                           catalog.path);
			}
			MmcifFile::Persist(*store, catalog.path, context);
			store->ClearDirty();
			catalog.write_store = store;
		} catch (std::exception &ex) {
			error = ErrorData(ex);
		}
	}
	lock_guard<mutex> l(lock);
	transactions.erase(transaction);
	return error;
}

void MmcifTransactionManager::RollbackTransaction(Transaction &transaction) {
	// The private store dies with the transaction; the committed store is untouched.
	lock_guard<mutex> l(lock);
	transactions.erase(transaction);
}

void MmcifTransactionManager::Checkpoint(ClientContext &context, bool force) {
	if (!catalog.write_mode) {
		throw NotImplementedException("Cannot CHECKPOINT a read-only mmcif database");
	}
	// Nothing to do: COMMIT already wrote every committed change to disk.
}

// ---------------------------------------------------------------------------
// Attach + storage-extension registration
// ---------------------------------------------------------------------------

// Enter write mode only when the user explicitly passes a read-write access
// key (READ_WRITE TRUE). Read-only is the default even though DuckDB core's
// own default access_mode is READ_WRITE. info.options is the raw pre-consumption
// map, so the extension can see the explicit keys DuckDB core already consumed.
static bool MmcifAttachWriteMode(const AttachInfo &info) {
	bool has_readwrite = false;
	bool has_readonly = false;
	bool readwrite_value = false;
	bool readonly_value = false;
	for (auto &entry : info.options) {
		if (entry.first == "readwrite" || entry.first == "read_write") {
			has_readwrite = true;
			readwrite_value = BooleanValue::Get(entry.second.DefaultCastAs(LogicalType::BOOLEAN));
		} else if (entry.first == "readonly" || entry.first == "read_only") {
			has_readonly = true;
			readonly_value = BooleanValue::Get(entry.second.DefaultCastAs(LogicalType::BOOLEAN));
		}
	}
	if (has_readwrite) {
		return readwrite_value;
	}
	if (has_readonly) {
		return !readonly_value;
	}
	return false; // no explicit access key -> read-only
}

static unique_ptr<Catalog> MmcifAttach(optional_ptr<StorageExtensionInfo> storage_info, ClientContext &context,
                                       AttachedDatabase &db, const string &name, AttachInfo &info,
                                       AttachOptions &attach_options) {
	bool write_mode = MmcifAttachWriteMode(info);
	// Remote files (http/https/s3/...) are served through a streaming file
	// system and cannot be rewritten in place, so they are always read-only.
	if (write_mode && MmcifFile::IsRemotePath(info.path)) {
		throw InvalidInputException(
		    "mmcif: remote file '%s' cannot be attached with READ_WRITE - remote mmcif files are read-only", info.path);
	}
	// Pipes (e.g. /dev/stdin) are one-shot streams: there is nothing to
	// write back to on COMMIT, so write mode is rejected instead of
	// silently losing mutations.
	if (write_mode && FileSystem::GetFileSystem(context).IsPipe(info.path)) {
		throw InvalidInputException("mmcif: '%s' cannot be attached with READ_WRITE - pipes are read-only", info.path);
	}
	std::optional<string> data_block;
	for (auto &entry : attach_options.options) {
		if (StringUtil::CIEquals(entry.first, "data_block")) {
			if (entry.second.IsNull() || entry.second.type() != LogicalType::VARCHAR) {
				throw InvalidInputException("mmcif: DATA_BLOCK must be a non-NULL string");
			}
			data_block = entry.second.GetValue<string>();
		}
	}
	attach_options.options.erase("data_block");
	return make_uniq<MmcifCatalog>(db, info.path, write_mode, context, std::move(data_block));
}

static unique_ptr<TransactionManager> MmcifCreateTransactionManager(optional_ptr<StorageExtensionInfo> storage_info,
                                                                    AttachedDatabase &db, Catalog &catalog) {
	auto &mmcif_catalog = catalog.Cast<MmcifCatalog>();
	return make_uniq<MmcifTransactionManager>(db, mmcif_catalog);
}

void MmcifRegisterStorageExtension(DBConfig &config) {
	auto storage_extension = make_shared_ptr<StorageExtension>();
	storage_extension->attach = MmcifAttach;
	storage_extension->create_transaction_manager = MmcifCreateTransactionManager;
	StorageExtension::Register(config, "mmcif", std::move(storage_extension));
}

} // namespace duckdb
