// mmcif DuckDB extension: ATTACH a cif data file and expose each mmcif category
// present in the file as a normal DuckDB table (read-only v1).
//
// Route per .scratch/mmcif-extension/map.md + issues/02-attach-type.md:
//   - StorageExtension registered on Load under "mmcif"; attach returns a real
//     custom Catalog (SQLite-style MmcifCatalog/MmcifSchemaEntry/MmcifTableEntry),
//     not a DuckCatalog (DuckCatalog storage fails on non-DuckDB files).
//   - Column types come from the precomputed dictionary type index (issue 03),
//     embedded as gzip'd byte arrays generated at build time from dict/*.tsv.gz.
//   - mmcif_tables(file) / mmcif_relationships(file) are global table functions.

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/gzip_file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/column_list.hpp"
#include "duckdb/parser/column_definition.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/storage/database_size.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/transaction/transaction.hpp"
#include "duckdb/transaction/transaction_manager.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/attached_database.hpp"

#include <fstream>
#include <sstream>

// RCSB mmcif core (vendored submodules under modules/)
#include "CifFile.h"
#include "CifParserBase.h"
#include "ISTable.h"
#include "TableFile.h"
#include "CifString.h"

#include "mmcif_dict_data.hpp" // embedded gzip'd dictionary artifacts (CMake)

namespace duckdb {

// ---------------------------------------------------------------------------
// Dictionary type index (issue 03): lazy singleton loaded from the embedded
// gzip'd TSV artifacts. Type keys are "_category.item"; relationships are
// (parent_category_id, child_category_id) pairs.
// ---------------------------------------------------------------------------

class DictionaryIndex {
public:
	static DictionaryIndex &Get() {
		static DictionaryIndex instance;
		return instance;
	}

	// "_category.item" -> DuckDB type; unknown -> VARCHAR
	LogicalType LookupType(const string &category, const string &column) const {
		auto key = "_" + category + "." + column;
		auto entry = types.find(key);
		if (entry != types.end()) {
			return entry->second;
		}
		return LogicalType::VARCHAR;
	}

	const std::vector<std::pair<std::string, std::string>> &GetRelationships() const {
		return relationships;
	}

private:
	DictionaryIndex() {
		LoadTypes();
		LoadRelationships();
	}

	void LoadTypes() {
		auto content = GZipFileSystem::UncompressGZIPString(
		    string(reinterpret_cast<const char *>(MMCIFF_TYPE_INDEX_GZ), MMCIFF_TYPE_INDEX_GZ_SIZE));
		auto lines = StringUtil::Split(content, '\n');
		for (auto &line : lines) {
			if (line.empty() || line[0] == '#') {
				continue;
			}
			auto tab = line.find('\t');
			if (tab == string::npos) {
				continue;
			}
			auto item = line.substr(0, tab);
			auto type_str = line.substr(tab + 1);
			LogicalType type;
			if (type_str == "DOUBLE") {
				type = LogicalType::DOUBLE;
			} else if (type_str == "BIGINT") {
				type = LogicalType::BIGINT;
			} else {
				type = LogicalType::VARCHAR;
			}
			types[item] = std::move(type);
		}
	}

	void LoadRelationships() {
		auto content = GZipFileSystem::UncompressGZIPString(
		    string(reinterpret_cast<const char *>(MMCIFF_RELATIONSHIPS_GZ), MMCIFF_RELATIONSHIPS_GZ_SIZE));
		auto lines = StringUtil::Split(content, '\n');
		for (auto &line : lines) {
			if (line.empty() || line[0] == '#') {
				continue;
			}
			auto tab = line.find('\t');
			if (tab == string::npos) {
				continue;
			}
			relationships.emplace_back(line.substr(0, tab), line.substr(tab + 1));
		}
	}

	case_insensitive_map_t<LogicalType> types;
	std::vector<std::pair<std::string, std::string>> relationships;
};

// ---------------------------------------------------------------------------
// CifFile parsing helpers (prototype quirks, issue 05): append a dummy trailing
// data block so the RCSB parser flushes any pending "last loop" table, then
// keep only the FIRST data block (issue 04).
// ---------------------------------------------------------------------------

static unique_ptr<CifFile> MmcifParseFile(const string &file_name, string &diags) {
	auto cif_file = make_uniq<CifFile>(true); // virtual-mode ctor, no sdb backing file
	std::ifstream in(file_name.c_str(), std::ios::binary);
	std::stringstream ss;
	ss << in.rdbuf();
	string content = ss.str();
	// gzip'd mmcif files (e.g. https://files.rcsb.org/download/1AMB.cif.gz) are
	// detected by magic bytes and decompressed before parsing.
	if (GZipFileSystem::CheckIsZip(content.data(), content.size())) {
		content = GZipFileSystem::UncompressGZIPString(content);
	}
	content += "\ndata_zzz_prototype\n#\n";
	CifParser parser(cif_file.get(), CifFileReadDef(), cif_file->GetVerbose());
	parser.ParseString(content, diags);
	return cif_file;
}

static ISTable &MmcifGetTable(CifFile &cif_file, const string &table_name, string &first_block) {
	first_block = cif_file.GetFirstBlockName();
	auto &block = cif_file.GetBlock(first_block);
	if (!block.IsTablePresent(table_name)) {
		throw BinderException("mmcif: category '%s' not present in block '%s'", table_name.c_str(),
		                      first_block.c_str());
	}
	auto &table = block.GetTable(table_name);
	if (table.GetColumnNames().empty()) {
		throw BinderException("mmcif: category '%s' has no columns", table_name.c_str());
	}
	return table;
}

// ---------------------------------------------------------------------------
// Per-category scan: MmcifBindData carries the parsed rows + dictionary types.
// Used both as the global mmcif_scan(file, table) table function (bind reads the
// two VARCHAR args) and as the attached-table scan (GetScanFunction pre-fills
// bind_data, so bind is never called).
// ---------------------------------------------------------------------------

struct MmcifBindData : public FunctionData {
	string file_name;
	string table_name;
	std::vector<std::string> column_names;
	std::vector<LogicalType> column_types;
	std::vector<std::vector<std::string>> rows; // row-major, values in column order

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<MmcifBindData>();
		result->file_name = file_name;
		result->table_name = table_name;
		result->column_names = column_names;
		result->column_types = column_types;
		result->rows = rows;
		return std::move(result);
	}
	bool Equals(const FunctionData &other) const override {
		return false;
	}

	static bool IsNullCell(const string &v) {
		// CifString::IsEmptyValue is true for "", ".", "?"
		return CifString::IsEmptyValue(v);
	}
};

struct MmcifGlobalState : public GlobalTableFunctionState {
	MmcifGlobalState(const MmcifBindData &bind_p, const vector<column_t> &column_ids_p)
	    : bind(bind_p), column_ids(column_ids_p), position(0) {
	}
	const MmcifBindData &bind;
	vector<column_t> column_ids;
	idx_t position;

	idx_t MaxThreads() const override {
		return 1;
	}
};

static void MmcifLoadRows(ISTable &table, MmcifBindData &result) {
	result.column_names = table.GetColumnNames();
	idx_t num_rows = table.GetNumRows();
	for (idx_t r = 0; r < num_rows; r++) {
		result.rows.push_back(table.GetRow(r));
	}
}

static unique_ptr<FunctionData> MmcifBind(ClientContext &context, TableFunctionBindInput &input,
                                          vector<LogicalType> &return_types, vector<string> &names) {
	auto file_name = input.inputs[0].GetValue<string>();
	auto table_name = input.inputs[1].GetValue<string>();
	auto result = make_uniq<MmcifBindData>();
	result->file_name = file_name;
	result->table_name = table_name;

	string diags;
	auto cif_file = MmcifParseFile(file_name, diags);
	string first_block;
	auto &table = MmcifGetTable(*cif_file, table_name, first_block);
	MmcifLoadRows(table, *result);

	for (auto &col : result->column_names) {
		auto type = DictionaryIndex::Get().LookupType(table_name, col);
		result->column_types.push_back(type);
		names.push_back(col);
		return_types.push_back(std::move(type));
	}
	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> MmcifInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<MmcifBindData>();
	return make_uniq<MmcifGlobalState>(bind, input.column_ids);
}

static void MmcifScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &gstate = data.global_state->Cast<MmcifGlobalState>();
	auto &bind = gstate.bind;
	idx_t row = gstate.position;
	idx_t count = 0;
	while (row < bind.rows.size() && count < STANDARD_VECTOR_SIZE) {
		const auto &r = bind.rows[row];
		for (idx_t c = 0; c < output.ColumnCount(); c++) {
			auto col_id = gstate.column_ids[c];
			auto &vec = output.data[c];
			if (col_id == COLUMN_IDENTIFIER_ROW_ID || col_id == COLUMN_IDENTIFIER_EMPTY) {
				// Virtual column requested for e.g. COUNT(*): emit a non-null value so rows are counted.
				if (col_id == COLUMN_IDENTIFIER_ROW_ID) {
					vec.SetValue(count, Value::Numeric(LogicalType::UBIGINT, row));
				} else {
					vec.SetValue(count, Value(true));
				}
				continue;
			}
			if (MmcifBindData::IsNullCell(r[col_id])) {
				vec.SetValue(count, Value());
			} else {
				vec.SetValue(count, Value(r[col_id])); // SetValue casts VARCHAR -> column type
			}
		}
		row++;
		count++;
	}
	gstate.position = row;
	output.SetCardinality(count);
}

static TableFunction MmcifScanFunction() {
	TableFunction result("mmcif_scan", {LogicalType::VARCHAR, LogicalType::VARCHAR}, MmcifScan, MmcifBind,
	                     MmcifInitGlobal);
	result.projection_pushdown = true;
	return result;
}

// ---------------------------------------------------------------------------
// Metadata table functions (global, issue 02): mmcif_tables(file) and
// mmcif_relationships(file), filtered to categories present in the file.
// ---------------------------------------------------------------------------

struct MmcifMetaBindData : public FunctionData {
	std::vector<std::vector<std::string>> rows; // one row of strings per output row

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<MmcifMetaBindData>();
		result->rows = rows;
		return std::move(result);
	}
	bool Equals(const FunctionData &other) const override {
		return false;
	}
};

struct MmcifMetaGlobalState : public GlobalTableFunctionState {
	MmcifMetaGlobalState(const MmcifMetaBindData &bind_p, const vector<column_t> &column_ids_p)
	    : bind(bind_p), column_ids(column_ids_p), position(0) {
	}
	const MmcifMetaBindData &bind;
	vector<column_t> column_ids;
	idx_t position;

	idx_t MaxThreads() const override {
		return 1;
	}
};

static unique_ptr<GlobalTableFunctionState> MmcifMetaInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<MmcifMetaBindData>();
	return make_uniq<MmcifMetaGlobalState>(bind, input.column_ids);
}

static void MmcifMetaScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &gstate = data.global_state->Cast<MmcifMetaGlobalState>();
	auto &bind = gstate.bind;
	idx_t row = gstate.position;
	idx_t count = 0;
	while (row < bind.rows.size() && count < STANDARD_VECTOR_SIZE) {
		const auto &r = bind.rows[row];
		for (idx_t c = 0; c < output.ColumnCount(); c++) {
			auto col_id = gstate.column_ids[c];
			output.data[c].SetValue(count, Value(r[col_id]));
		}
		row++;
		count++;
	}
	gstate.position = row;
	output.SetCardinality(count);
}

static vector<string> MmcifFileCategories(const string &file_name) {
	string diags;
	auto cif_file = MmcifParseFile(file_name, diags);
	auto first_block = cif_file->GetFirstBlockName();
	auto &block = cif_file->GetBlock(first_block);
	vector<string> categories;
	block.GetTableNames(categories);
	return categories;
}

// mmcif_tables(file): table_name, column_name, column_type
static unique_ptr<FunctionData> MmcifTablesBind(ClientContext &context, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<string> &names) {
	auto file_name = input.inputs[0].GetValue<string>();
	auto result = make_uniq<MmcifMetaBindData>();
	auto categories = MmcifFileCategories(file_name);
	string diags;
	auto cif_file = MmcifParseFile(file_name, diags);
	auto first_block = cif_file->GetFirstBlockName();
	auto &block = cif_file->GetBlock(first_block);
	for (auto &category : categories) {
		auto &table = block.GetTable(category);
		auto col_names = table.GetColumnNames();
		for (auto &col : col_names) {
			auto type = DictionaryIndex::Get().LookupType(category, col);
			vector<string> row = {category, col, type.ToString()};
			result->rows.push_back(std::move(row));
		}
	}
	names.emplace_back("table_name");
	names.emplace_back("column_name");
	names.emplace_back("column_type");
	return_types.push_back(LogicalType::VARCHAR);
	return_types.push_back(LogicalType::VARCHAR);
	return_types.push_back(LogicalType::VARCHAR);
	return std::move(result);
}

// mmcif_relationships(file): parent_category, child_category
static unique_ptr<FunctionData> MmcifRelationshipsBind(ClientContext &context, TableFunctionBindInput &input,
                                                       vector<LogicalType> &return_types, vector<string> &names) {
	auto file_name = input.inputs[0].GetValue<string>();
	auto result = make_uniq<MmcifMetaBindData>();
	auto categories = MmcifFileCategories(file_name);
	case_insensitive_set_t present(categories.begin(), categories.end());
	for (auto &rel : DictionaryIndex::Get().GetRelationships()) {
		if (present.find(rel.first) != present.end() && present.find(rel.second) != present.end()) {
			vector<string> row = {rel.first, rel.second};
			result->rows.push_back(std::move(row));
		}
	}
	names.emplace_back("parent_category");
	names.emplace_back("child_category");
	return_types.push_back(LogicalType::VARCHAR);
	return_types.push_back(LogicalType::VARCHAR);
	return std::move(result);
}

// ---------------------------------------------------------------------------
// MmcifTableEntry: a real TableCatalogEntry whose ColumnList carries dictionary
// types (so DESCRIBE shows DOUBLE/BIGINT/VARCHAR) and whose GetScanFunction
// returns a per-category scan with pre-filled bind data.
// ---------------------------------------------------------------------------

class MmcifTableEntry : public TableCatalogEntry {
public:
	MmcifTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info, string file_name_p,
	                string table_name_p)
	    : TableCatalogEntry(catalog, schema, info), file_name(std::move(file_name_p)),
	      table_name(std::move(table_name_p)) {
	}

	string file_name;
	string table_name;

	unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, column_t column_id) override {
		return nullptr;
	}

	TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) override {
		auto result = make_uniq<MmcifBindData>();
		result->file_name = file_name;
		result->table_name = table_name;

		string diags;
		auto cif_file = MmcifParseFile(file_name, diags);
		string first_block;
		auto &table = MmcifGetTable(*cif_file, table_name, first_block);
		MmcifLoadRows(table, *result);
		for (auto &col : result->column_names) {
			result->column_types.push_back(DictionaryIndex::Get().LookupType(table_name, col));
		}

		bind_data = std::move(result);
		return MmcifScanFunction();
	}

	TableStorageInfo GetStorageInfo(ClientContext &context) override {
		TableStorageInfo result;
		result.cardinality = 10000;
		return result;
	}
};

// ---------------------------------------------------------------------------
// MmcifSchemaEntry: single schema; Scan(TABLE_ENTRY) enumerates the categories
// present in the file; LookupEntry materializes a MmcifTableEntry. Read-only:
// every DDL operation throws.
// ---------------------------------------------------------------------------

class MmcifSchemaEntry : public SchemaCatalogEntry {
public:
	MmcifSchemaEntry(Catalog &catalog, CreateSchemaInfo &info, string file_name_p)
	    : SchemaCatalogEntry(catalog, info), file_name(std::move(file_name_p)) {
	}

	string file_name;
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
	auto &catalog = ParentCatalog();
	CreateTableInfo info(*this, entry_name);
	auto &dict = DictionaryIndex::Get();
	string diags;
	auto cif_file = MmcifParseFile(file_name, diags);
	string first_block;
	auto &table = MmcifGetTable(*cif_file, entry_name, first_block);
	auto col_names = table.GetColumnNames();
	for (auto &col : col_names) {
		info.columns.AddColumn(ColumnDefinition(col, dict.LookupType(entry_name, col)));
	}
	auto entry = make_uniq<MmcifTableEntry>(catalog, *this, info, file_name, entry_name);
	auto *result = entry.get();
	tables[entry_name] = std::move(entry);
	return *result;
}

void MmcifSchemaEntry::Scan(ClientContext &context, CatalogType type,
                            const std::function<void(CatalogEntry &)> &callback) {
	if (type != CatalogType::TABLE_ENTRY) {
		return; // mmcif exposes only tables
	}
	auto categories = MmcifFileCategories(file_name);
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
	string diags;
	auto cif_file = MmcifParseFile(file_name, diags);
	auto first_block = cif_file->GetFirstBlockName();
	auto &block = cif_file->GetBlock(first_block);
	if (!block.IsTablePresent(entry_name)) {
		return nullptr;
	}
	return &GetTableEntry(transaction, entry_name);
}

// ---------------------------------------------------------------------------
// MmcifCatalog: SQLite-style custom Catalog. Read-only; single "main" schema.
// ---------------------------------------------------------------------------

class MmcifCatalog : public Catalog {
public:
	MmcifCatalog(AttachedDatabase &db_p, string path_p) : Catalog(db_p), path(std::move(path_p)) {
	}

	string path;

	void Initialize(bool load_builtin) override {
		CreateSchemaInfo info;
		main_schema = make_uniq<MmcifSchemaEntry>(*this, info, path);
	}

	string GetCatalogType() override {
		return "mmcif";
	}

	optional_ptr<CatalogEntry> CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) override {
		if (info.schema == DEFAULT_SCHEMA) {
			return main_schema.get();
		}
		throw BinderException("mmcif databases only have a single schema");
	}

	void ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) override {
		callback(*main_schema);
	}

	optional_ptr<SchemaCatalogEntry> LookupSchema(CatalogTransaction transaction, const EntryLookupInfo &schema_lookup,
	                                              OnEntryNotFound if_not_found) override {
		auto &schema_name = schema_lookup.GetEntryName();
		if (schema_name == DEFAULT_SCHEMA || schema_name == INVALID_SCHEMA) {
			return main_schema.get();
		}
		if (if_not_found == OnEntryNotFound::RETURN_NULL) {
			return nullptr;
		}
		throw BinderException("mmcif databases only have a single schema - \"%s\"", std::string(DEFAULT_SCHEMA));
	}

	PhysicalOperator &PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner, LogicalCreateTable &op,
	                                    PhysicalOperator &plan) override {
		throw NotImplementedException("mmcif databases are read-only - cannot CREATE TABLE AS");
	}
	PhysicalOperator &PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
	                             optional_ptr<PhysicalOperator> plan) override {
		throw NotImplementedException("mmcif databases are read-only - cannot INSERT");
	}
	PhysicalOperator &PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
	                             PhysicalOperator &plan) override {
		throw NotImplementedException("mmcif databases are read-only - cannot DELETE");
	}
	PhysicalOperator &PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
	                             PhysicalOperator &plan) override {
		throw NotImplementedException("mmcif databases are read-only - cannot UPDATE");
	}

	DatabaseSize GetDatabaseSize(ClientContext &context) override {
		DatabaseSize result;
		result.total_blocks = 0;
		result.block_size = 0;
		result.free_blocks = 0;
		result.used_blocks = 0;
		result.bytes = 0;
		result.wal_size = idx_t(-1);
		return result;
	}

	bool InMemory() override {
		return true;
	}

	string GetDBPath() override {
		return path;
	}

private:
	void DropSchema(ClientContext &context, DropInfo &info) override {
		throw BinderException("mmcif databases do not support dropping schemas");
	}

private:
	unique_ptr<MmcifSchemaEntry> main_schema;
};

// ---------------------------------------------------------------------------
// Read-only transaction manager (DuckTransactionManager requires a DuckCatalog).
// ---------------------------------------------------------------------------

class MmcifTransactionManager : public TransactionManager {
public:
	explicit MmcifTransactionManager(AttachedDatabase &db) : TransactionManager(db) {
	}

	Transaction &StartTransaction(ClientContext &context) override {
		auto transaction = make_uniq<Transaction>(*this, context);
		auto &result = *transaction;
		lock_guard<mutex> l(lock);
		transactions[result] = std::move(transaction);
		return result;
	}

	ErrorData CommitTransaction(ClientContext &context, Transaction &transaction) override {
		lock_guard<mutex> l(lock);
		transactions.erase(transaction);
		return ErrorData();
	}

	void RollbackTransaction(Transaction &transaction) override {
		lock_guard<mutex> l(lock);
		transactions.erase(transaction);
	}

	void Checkpoint(ClientContext &context, bool force = false) override {
		throw NotImplementedException("Cannot CHECKPOINT an mmcif database");
	}

private:
	mutex lock;
	reference_map_t<Transaction, unique_ptr<Transaction>> transactions;
};

// ---------------------------------------------------------------------------
// Attach + Load registration
// ---------------------------------------------------------------------------

static unique_ptr<Catalog> MmcifAttach(optional_ptr<StorageExtensionInfo> storage_info, ClientContext &context,
                                       AttachedDatabase &db, const string &name, AttachInfo &info,
                                       AttachOptions &attach_options) {
	return make_uniq<MmcifCatalog>(db, info.path);
}

static unique_ptr<TransactionManager> MmcifCreateTransactionManager(optional_ptr<StorageExtensionInfo> storage_info,
                                                                    AttachedDatabase &db, Catalog &catalog) {
	return make_uniq<MmcifTransactionManager>(db);
}

void MmcifCoreLoad(ExtensionLoader &loader) {
	loader.RegisterFunction(MmcifScanFunction());

	TableFunction mmcif_tables("mmcif_tables", {LogicalType::VARCHAR}, MmcifMetaScan, MmcifTablesBind,
	                           MmcifMetaInitGlobal);
	mmcif_tables.projection_pushdown = true;
	loader.RegisterFunction(mmcif_tables);

	TableFunction mmcif_relationships("mmcif_relationships", {LogicalType::VARCHAR}, MmcifMetaScan,
	                                  MmcifRelationshipsBind, MmcifMetaInitGlobal);
	mmcif_relationships.projection_pushdown = true;
	loader.RegisterFunction(mmcif_relationships);

	auto &db = loader.GetDatabaseInstance();
	auto &config = DBConfig::GetConfig(db);
	auto storage_extension = make_shared_ptr<StorageExtension>();
	storage_extension->attach = MmcifAttach;
	storage_extension->create_transaction_manager = MmcifCreateTransactionManager;
	StorageExtension::Register(config, "mmcif", std::move(storage_extension));
}

} // namespace duckdb
