// mmcif catalog module implementation: schema/table entries, the custom
// Catalog, the write-mode DML operators, the transaction manager, and the
// storage-extension registration.
//
// Write-mode DML operators (D3): custom physical sinks that read the input
// chunk and apply row-level mutations to the catalog's persistent write store.
// row_id == physical store row index (scans emit row_id = row index).

#include "mmcif_catalog.hpp"

#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/operator/numeric_cast.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/physical_operator_states.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/storage/storage_extension.hpp"

#include "mmcif_dictionary.hpp"
#include "mmcif_file.hpp"
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

// Copy the store rows into the write-mode bind data (row-major snapshot; DML
// mutates the store, not this copy).
static void MmcifLoadRows(MmcifWriteCategory *cat, MmcifBindData &result) {
	result.column_names = cat->columns;
	result.rows = cat->rows;
}

// NULL -> empty cell -> written back as "?"
static string MmcifCellToString(const Vector &vec, idx_t row) {
	auto val = vec.GetValue(row);
	if (val.IsNull()) {
		return ""; // NULL -> empty cell -> written back as "?"
	}
	return val.ToString();
}

// ---------------------------------------------------------------------------
// Write-mode DML operators (D3)
// ---------------------------------------------------------------------------

struct MmcifWriteGlobalState : public GlobalSinkState {
	idx_t count = 0;
	mutex lock;
	// DELETE row ids are accumulated across sink chunks and applied once in Combine:
	// row ids refer to positions in the table as it was when the scan started, so
	// applying them chunk-by-chunk would use stale indices after the first shrink.
	std::vector<unsigned int> delete_indices;
};

class MmcifInsertOperator : public PhysicalOperator {
public:
	MmcifInsertOperator(PhysicalPlan &physical_plan, vector<LogicalType> types, idx_t estimated_cardinality,
	                    MmcifCatalog &catalog, string table_name, vector<idx_t> column_index_map)
	    : PhysicalOperator(physical_plan, PhysicalOperatorType::INSERT, std::move(types), estimated_cardinality),
	      catalog(catalog), table_name(std::move(table_name)), column_index_map(std::move(column_index_map)) {
	}

	MmcifCatalog &catalog;
	string table_name;
	vector<idx_t> column_index_map; // empty => positional insert into all columns

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
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override {
		auto &gstate = input.global_state.Cast<MmcifWriteGlobalState>();
		auto store = catalog.GetWriteStore();
		auto cat = MmcifGetWriteCategory(*store, table_name);
		idx_t num_cols = cat->columns.size();
		chunk.Flatten();
		lock_guard<mutex> l(gstate.lock);
		for (idx_t r = 0; r < chunk.size(); r++) {
			std::vector<string> row(num_cols, "");
			for (idx_t c = 0; c < num_cols; c++) {
				if (column_index_map.empty()) {
					row[c] = MmcifCellToString(chunk.data[c], r);
					continue;
				}
				auto mapped = column_index_map[c];
				if (mapped == DConstants::INVALID_INDEX) {
					continue; // unspecified column -> NULL
				}
				row[c] = MmcifCellToString(chunk.data[mapped], r);
			}
			store->AddRow(*cat, row);
			gstate.count++;
		}
		return SinkResultType::NEED_MORE_INPUT;
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

class MmcifDeleteOperator : public PhysicalOperator {
public:
	MmcifDeleteOperator(PhysicalPlan &physical_plan, vector<LogicalType> types, idx_t estimated_cardinality,
	                    MmcifCatalog &catalog, string table_name, idx_t row_id_index)
	    : PhysicalOperator(physical_plan, PhysicalOperatorType::DELETE_OPERATOR, std::move(types),
	                       estimated_cardinality),
	      catalog(catalog), table_name(std::move(table_name)), row_id_index(row_id_index) {
	}

	MmcifCatalog &catalog;
	string table_name;
	idx_t row_id_index;

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
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override {
		auto &gstate = input.global_state.Cast<MmcifWriteGlobalState>();
		chunk.Flatten();
		auto &row_ids = chunk.data[row_id_index];
		auto row_data = FlatVector::GetData<int64_t>(row_ids);
		lock_guard<mutex> l(gstate.lock);
		for (idx_t r = 0; r < chunk.size(); r++) {
			gstate.delete_indices.push_back(NumericCast<unsigned int>(row_data[r]));
		}
		return SinkResultType::NEED_MORE_INPUT;
	}
	SinkCombineResultType Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const override {
		auto &gstate = input.global_state.Cast<MmcifWriteGlobalState>();
		lock_guard<mutex> l(gstate.lock);
		std::vector<unsigned int> indices;
		indices.swap(gstate.delete_indices);
		sort(indices.begin(), indices.end());
		indices.erase(unique(indices.begin(), indices.end()), indices.end());
		if (!indices.empty()) {
			auto store = catalog.GetWriteStore();
			auto cat = MmcifGetWriteCategory(*store, table_name);
			store->DeleteRows(*cat, indices);
			gstate.count += indices.size();
		}
		return SinkCombineResultType::FINISHED;
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

class MmcifUpdateOperator : public PhysicalOperator {
public:
	MmcifUpdateOperator(PhysicalPlan &physical_plan, vector<LogicalType> types, idx_t estimated_cardinality,
	                    MmcifCatalog &catalog, string table_name, vector<idx_t> columns, vector<idx_t> expr_indices)
	    : PhysicalOperator(physical_plan, PhysicalOperatorType::UPDATE, std::move(types), estimated_cardinality),
	      catalog(catalog), table_name(std::move(table_name)), columns(std::move(columns)),
	      expr_indices(std::move(expr_indices)) {
	}

	MmcifCatalog &catalog;
	string table_name;
	vector<idx_t> columns;      // physical column index to update
	vector<idx_t> expr_indices; // chunk index holding the new value

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
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override {
		auto &gstate = input.global_state.Cast<MmcifWriteGlobalState>();
		auto store = catalog.GetWriteStore();
		auto cat = MmcifGetWriteCategory(*store, table_name);
		auto col_names = cat->columns;
		chunk.Flatten();
		auto &row_ids = chunk.data[chunk.ColumnCount() - 1];
		auto row_data = FlatVector::GetData<int64_t>(row_ids);
		lock_guard<mutex> l(gstate.lock);
		for (idx_t r = 0; r < chunk.size(); r++) {
			for (idx_t i = 0; i < columns.size(); i++) {
				store->UpdateCell(*cat, NumericCast<idx_t>(row_data[r]), col_names[columns[i]],
				                  MmcifCellToString(chunk.data[expr_indices[i]], r));
			}
		}
		gstate.count += chunk.size();
		return SinkResultType::NEED_MORE_INPUT;
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

// ---------------------------------------------------------------------------
// MmcifTableEntry
// ---------------------------------------------------------------------------

MmcifTableEntry::MmcifTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info,
                                 string file_name_p, string table_name_p, MmcifCatalog *catalog_p)
    : TableCatalogEntry(catalog, schema, info), file_name(std::move(file_name_p)), table_name(std::move(table_name_p)),
      catalog(catalog_p) {
}

unique_ptr<BaseStatistics> MmcifTableEntry::GetStatistics(ClientContext &context, column_t column_id) {
	// Real min/max stats require a full category scan, which we deliberately
	// avoid to keep LIMIT/schema queries cheap. Stats are skipped; the planner
	// still gets exact cardinality from GetStorageInfo (recommendation 7).
	return nullptr;
}

TableFunction MmcifTableEntry::GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) {
	auto result = make_uniq<MmcifBindData>();
	result->file_name = file_name;
	result->table_name = table_name;
	result->table_entry = this;

	if (catalog->IsWriteMode()) {
		// Write mode: materialized store rows (DML mutates the persistent store).
		auto store = catalog->GetWriteStore();
		auto cat = MmcifGetWriteCategory(*store, table_name);
		MmcifLoadRows(cat, *result);
		for (auto &col : result->column_names) {
			result->column_types.push_back(DictionaryIndex::Get().LookupType(table_name, col));
		}
	} else {
		// Read-only: shared lazy index, streamed scan. No materialization.
		auto index = catalog->GetIndex(&context);
		auto cat = index->FindCategory(table_name);
		if (!cat || cat->columns.empty()) {
			throw BinderException("mmcif: category '%s' not present in file '%s'", table_name.c_str(),
			                      file_name.c_str());
		}
		result->index = std::move(index);
		result->category = cat;
		result->column_names = cat->columns;
		for (auto &col : result->column_names) {
			result->column_types.push_back(DictionaryIndex::Get().LookupType(table_name, col));
		}
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
	result.cardinality = 10000;
	if (!catalog->IsWriteMode()) {
		auto index = catalog->GetIndex(&context);
		auto cat = index->FindCategory(table_name);
		if (cat) {
			// Exact cardinality, computed lazily once per category (cached).
			result.cardinality = index->GetRowCount(*cat);
		}
	}
	return result;
}

// ---------------------------------------------------------------------------
// MmcifSchemaEntry
// ---------------------------------------------------------------------------

MmcifSchemaEntry::MmcifSchemaEntry(Catalog &catalog, CreateSchemaInfo &info, string file_name_p,
                                   MmcifCatalog *catalog_p)
    : SchemaCatalogEntry(catalog, info), file_name(std::move(file_name_p)), catalog(catalog_p) {
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
	auto &catalog = ParentCatalog();
	CreateTableInfo info(*this, entry_name);
	auto &dict = DictionaryIndex::Get();
	if (this->catalog->IsWriteMode()) {
		auto store = this->catalog->GetWriteStore();
		auto cat = MmcifGetWriteCategory(*store, entry_name);
		for (auto &col : cat->columns) {
			info.columns.AddColumn(ColumnDefinition(col, dict.LookupType(entry_name, col)));
		}
	} else {
		auto index = this->catalog->GetIndex(transaction.context);
		auto cat = index->FindCategory(entry_name);
		if (!cat) {
			throw BinderException("mmcif: category '%s' not present in file '%s'", entry_name.c_str(),
			                      file_name.c_str());
		}
		for (auto &col : cat->columns) {
			info.columns.AddColumn(ColumnDefinition(col, dict.LookupType(entry_name, col)));
		}
	}
	auto entry = make_uniq<MmcifTableEntry>(catalog, *this, info, file_name, entry_name, this->catalog);
	auto *result = entry.get();
	tables[entry_name] = std::move(entry);
	return *result;
}

void MmcifSchemaEntry::Scan(ClientContext &context, CatalogType type,
                            const std::function<void(CatalogEntry &)> &callback) {
	if (type != CatalogType::TABLE_ENTRY) {
		return; // mmcif exposes only tables
	}
	vector<string> categories;
	if (catalog->IsWriteMode()) {
		categories = catalog->GetWriteStore()->GetCategoryNames();
	} else {
		auto index = catalog->GetIndex(&context);
		index->GetCategoryNames(categories);
	}
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
	if (catalog->IsWriteMode()) {
		if (catalog->GetWriteStore()->FindCategory(entry_name) == nullptr) {
			return nullptr;
		}
		return &GetTableEntry(transaction, entry_name);
	}
	auto index = catalog->GetIndex(transaction.context);
	if (!index->FindCategory(entry_name)) {
		return nullptr;
	}
	return &GetTableEntry(transaction, entry_name);
}

// ---------------------------------------------------------------------------
// MmcifCatalog
// ---------------------------------------------------------------------------

MmcifCatalog::MmcifCatalog(AttachedDatabase &db_p, string path_p, bool write_mode_p)
    : Catalog(db_p), path(std::move(path_p)), write_mode(write_mode_p) {
	if (write_mode) {
		write_store = MmcifFile::LoadWriteStore(path, nullptr);
	}
}

bool MmcifCatalog::IsWriteMode() const {
	return write_mode;
}

MmcifWriteStore *MmcifCatalog::GetWriteStore() {
	return write_store.get();
}

shared_ptr<MmcifIndex> MmcifCatalog::GetIndex(optional_ptr<ClientContext> context) {
	if (write_mode) {
		return nullptr;
	}
	lock_guard<mutex> l(index_lock);
	if (!index) {
		index = MmcifIndex::Load(path, context);
	}
	return index;
}

void MmcifCatalog::ReloadFromDisk() {
	if (!write_mode) {
		return;
	}
	write_store = MmcifFile::LoadWriteStore(path, nullptr);
}

void MmcifCatalog::Persist(ClientContext &context) {
	if (!write_mode || !write_store) {
		return;
	}
	MmcifFile::Persist(*write_store, path, context);
}

void MmcifCatalog::Initialize(bool load_builtin) {
	CreateSchemaInfo info;
	main_schema = make_uniq<MmcifSchemaEntry>(*this, info, path, this);
}

void MmcifCatalog::OnDetach(ClientContext &context) {
	Persist(context);
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
	return true;
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
	auto transaction = make_uniq<Transaction>(*this, context);
	auto &result = *transaction;
	lock_guard<mutex> l(lock);
	transactions[result] = std::move(transaction);
	return result;
}

ErrorData MmcifTransactionManager::CommitTransaction(ClientContext &context, Transaction &transaction) {
	// D5: write the mutated in-memory write store back to the attached .cif on COMMIT.
	catalog.Persist(context);
	lock_guard<mutex> l(lock);
	transactions.erase(transaction);
	return ErrorData();
}

void MmcifTransactionManager::RollbackTransaction(Transaction &transaction) {
	// D6: ROLLBACK discards in-memory mutations by re-parsing from disk.
	catalog.ReloadFromDisk();
	lock_guard<mutex> l(lock);
	transactions.erase(transaction);
}

void MmcifTransactionManager::Checkpoint(ClientContext &context, bool force) {
	if (!catalog.IsWriteMode()) {
		throw NotImplementedException("Cannot CHECKPOINT a read-only mmcif database");
	}
	catalog.Persist(context);
}

// ---------------------------------------------------------------------------
// Attach + storage-extension registration
// ---------------------------------------------------------------------------

// D1: enter write mode only when the user explicitly passes a read-write access
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
	return make_uniq<MmcifCatalog>(db, info.path, write_mode);
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
