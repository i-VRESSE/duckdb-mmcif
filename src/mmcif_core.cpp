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
#include "duckdb/common/file_system.hpp"
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
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/execution/physical_operator_states.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/operator/logical_insert.hpp"
#include "duckdb/planner/operator/logical_delete.hpp"
#include "duckdb/planner/operator/logical_update.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/common/operator/numeric_cast.hpp"
#include "duckdb/common/numeric_utils.hpp"

#include <fstream>
#include <sstream>

// RCSB mmcif core (vendored submodules under modules/)
#include "CifFile.h"
#include "CifParserBase.h"
#include "ISTable.h"
#include "TableFile.h"
#include "CifString.h"

#include "mmcif_index.hpp"

#include "mmcif_dict_data.hpp" // embedded gzip'd dictionary artifacts (CMake)

#include "duckdb/common/vector_operations/vector_operations.hpp"

namespace duckdb {

// ---------------------------------------------------------------------------
// Dictionary type index (issue 03): lazy singleton loaded from the embedded
// gzip'd TSV artifacts. Type keys are "_category.item"; relationships are
// (parent_item, child_item) pairs where each item is a "_category.item" key
// carrying both the category (table) and the data item (column).
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

// Returns true for paths that reference a remote resource (http/https URL,
// S3, etc.) rather than a local file. Remote paths are handled by DuckDB's
// virtual file system (httpfs is autoloaded) and are always read-only here.
static bool MmcifIsRemotePath(const string &path) {
	auto lower = StringUtil::Lower(path);
	return lower.find("://") != string::npos;
}

// Read the raw bytes of a file. When a client context is available the read
// goes through DuckDB's virtual file system, so http/https URLs (e.g.
// https://files.rcsb.org/download/1AMB.cif.gz) and s3:// paths work and the
// httpfs extension is autoloaded as needed. Falls back to std::ifstream for
// callers without a context (always local paths).
static string MmcifReadFileContents(const string &file_name, optional_ptr<ClientContext> context) {
	if (context) {
		auto &fs = FileSystem::GetFileSystem(*context);
		if (!MmcifIsRemotePath(file_name) && !fs.FileExists(file_name)) {
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
	std::stringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

static unique_ptr<CifFile> MmcifParseFile(const string &file_name, string &diags,
                                          optional_ptr<ClientContext> context = nullptr) {
	auto cif_file = make_uniq<CifFile>(true); // virtual-mode ctor, no sdb backing file
	string content = MmcifReadFileContents(file_name, context);
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
	// Read-only path (recommendation 3/5): a shared lazy index + category. Rows
	// are streamed from the byte cursor in MmcifScan; nothing is materialized at
	// bind, so LIMIT 10 never copies 2.44M rows.
	shared_ptr<MmcifIndex> index;
	MmcifCategory *category = nullptr;
	// Write-mode (legacy) path: materialized RCSB rows.
	std::vector<std::vector<std::string>> rows;
	optional_ptr<TableCatalogEntry> table_entry; // set only for attached-table scans

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<MmcifBindData>();
		result->file_name = file_name;
		result->table_name = table_name;
		result->column_names = column_names;
		result->column_types = column_types;
		result->index = index;
		result->category = category;
		result->rows = rows;
		result->table_entry = table_entry;
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
		if (bind.index && bind.category) {
			ncols = bind.category->columns.size();
			full_to_out.assign(ncols, DConstants::INVALID_INDEX);
			for (idx_t c = 0; c < column_ids.size(); c++) {
				auto col_id = column_ids[c];
				if (col_id != COLUMN_IDENTIFIER_ROW_ID && col_id != COLUMN_IDENTIFIER_EMPTY && col_id < ncols) {
					full_to_out[col_id] = c;
				}
			}
			single_by_col.assign(ncols, nullptr);
			for (auto &s : bind.category->singles) {
				if (s.col < ncols) {
					single_by_col[s.col] = &s;
				}
			}
			if (bind.category->is_loop) {
				cursor = make_uniq<MmcifValueCursor>(bind.index->GetData(), bind.category->data_start,
				                                     bind.category->data_end);
			}
		}
	}
	const MmcifBindData &bind;
	vector<column_t> column_ids;
	idx_t position;
	idx_t ncols = 0;
	vector<idx_t> full_to_out;                     // full column index -> output position
	vector<const MmcifSingleCell *> single_by_col; // full column index -> single cell (broadcast)
	unique_ptr<MmcifValueCursor> cursor;
	bool done = false;
	bool single_done = false;

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

static void MmcifLoadIndex(MmcifBindData &result, shared_ptr<MmcifIndex> index, const string &table_name,
                           const string &file_name) {
	auto cat = index->FindCategory(table_name);
	if (!cat || cat->columns.empty()) {
		throw BinderException("mmcif: category '%s' not present in block '%s'", table_name.c_str(),
		                      index->GetDataBlockName().c_str());
	}
	result.index = std::move(index);
	result.category = cat;
	result.column_names = cat->columns;
}

static unique_ptr<FunctionData> MmcifBind(ClientContext &context, TableFunctionBindInput &input,
                                          vector<LogicalType> &return_types, vector<string> &names) {
	auto file_name = input.inputs[0].GetValue<string>();
	auto table_name = input.inputs[1].GetValue<string>();
	auto result = make_uniq<MmcifBindData>();
	result->file_name = file_name;
	result->table_name = table_name;

	auto index = MmcifIndex::Load(file_name, &context);
	MmcifLoadIndex(*result, std::move(index), table_name, file_name);

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

// Index-backed scan (recommendation 3/5/6): parse the category's loop range
// incrementally from the byte cursor, row-major, into per-column VARCHAR
// vectors, then vectorized-cast each column to its dictionary type. LIMIT
// pushdown falls out naturally: we stop after the requested rows are filled.
static void MmcifScanIndex(ClientContext &context, TableFunctionInput &data, DataChunk &output,
                           MmcifGlobalState &gstate) {
	auto &cat = *gstate.bind.category;
	idx_t out_cols = output.ColumnCount();
	vector<unique_ptr<Vector>> tmp(out_cols);
	vector<string_t *> ptrs(out_cols);
	for (idx_t c = 0; c < out_cols; c++) {
		tmp[c] = make_uniq<Vector>(LogicalType::VARCHAR);
		ptrs[c] = FlatVector::GetData<string_t>(*tmp[c]);
	}

	idx_t count = 0;
	if (cat.is_loop) {
		idx_t loop_ncols = cat.loop_col_map.size();
		while (count < STANDARD_VECTOR_SIZE && !gstate.done) {
			bool ok = true;
			for (idx_t li = 0; li < loop_ncols; li++) {
				idx_t full_col = cat.loop_col_map[li];
				idx_t out_pos = gstate.full_to_out[full_col];
				const char *out;
				idx_t len;
				bool is_null;
				if (!gstate.cursor->Next(&out, &len, &is_null)) {
					ok = false;
					break;
				}
				if (out_pos != DConstants::INVALID_INDEX) {
					if (is_null) {
						FlatVector::SetNull(*tmp[out_pos], count, true);
					} else {
						ptrs[out_pos][count] = string_t(out, UnsafeNumericCast<uint32_t>(len));
					}
				}
			}
			if (!ok) {
				gstate.done = true;
				break;
			}
			for (idx_t c = 0; c < out_cols; c++) {
				auto col_id = gstate.column_ids[c];
				if (col_id == COLUMN_IDENTIFIER_ROW_ID) {
					output.data[c].SetValue(count, Value::Numeric(LogicalType::BIGINT, gstate.position));
				} else if (col_id == COLUMN_IDENTIFIER_EMPTY) {
					output.data[c].SetValue(count, Value(true));
				}
			}
			count++;
			gstate.position++;
		}
	} else {
		// Single-tag category: exactly one row.
		if (gstate.single_done) {
			output.SetCardinality(0);
			return;
		}
		for (idx_t c = 0; c < out_cols; c++) {
			auto col_id = gstate.column_ids[c];
			if (col_id == COLUMN_IDENTIFIER_ROW_ID) {
				output.data[c].SetValue(0, Value::Numeric(LogicalType::BIGINT, 0));
			} else if (col_id == COLUMN_IDENTIFIER_EMPTY) {
				output.data[c].SetValue(0, Value(true));
			} else {
				auto sc = (col_id < gstate.ncols) ? gstate.single_by_col[col_id] : nullptr;
				if (sc && !sc->is_null) {
					ptrs[c][0] = string_t(gstate.bind.index->GetData() + sc->off, UnsafeNumericCast<uint32_t>(sc->len));
				} else {
					FlatVector::SetNull(*tmp[c], 0, true);
				}
			}
		}
		count = 1;
		gstate.single_done = true;
	}

	// Vectorized cast VARCHAR -> dictionary type for each real column.
	for (idx_t c = 0; c < out_cols; c++) {
		auto col_id = gstate.column_ids[c];
		if (col_id == COLUMN_IDENTIFIER_ROW_ID || col_id == COLUMN_IDENTIFIER_EMPTY) {
			continue;
		}
		VectorOperations::Cast(context, *tmp[c], output.data[c], count);
	}
	output.SetCardinality(count);
}

static void MmcifScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &gstate = data.global_state->Cast<MmcifGlobalState>();
	auto &bind = gstate.bind;
	if (bind.index && bind.category) {
		MmcifScanIndex(context, data, output, gstate);
		return;
	}
	// Legacy write-mode scan over materialized rows.
	idx_t row = gstate.position;
	idx_t count = 0;
	while (row < bind.rows.size() && count < STANDARD_VECTOR_SIZE) {
		const auto &r = bind.rows[row];
		for (idx_t c = 0; c < output.ColumnCount(); c++) {
			auto col_id = gstate.column_ids[c];
			auto &vec = output.data[c];
			if (col_id == COLUMN_IDENTIFIER_ROW_ID || col_id == COLUMN_IDENTIFIER_EMPTY) {
				if (col_id == COLUMN_IDENTIFIER_ROW_ID) {
					vec.SetValue(count, Value::Numeric(LogicalType::BIGINT, row));
				} else {
					vec.SetValue(count, Value(true));
				}
				continue;
			}
			if (MmcifBindData::IsNullCell(r[col_id])) {
				vec.SetValue(count, Value());
			} else {
				vec.SetValue(count, Value(r[col_id]));
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

static vector<string> MmcifFileCategories(const string &file_name, optional_ptr<ClientContext> context) {
	auto index = MmcifIndex::Load(file_name, context);
	vector<string> categories;
	index->GetCategoryNames(categories);
	return categories;
}

// mmcif_tables(file): table_name, column_name, column_type
static unique_ptr<FunctionData> MmcifTablesBind(ClientContext &context, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<string> &names) {
	auto file_name = input.inputs[0].GetValue<string>();
	auto result = make_uniq<MmcifMetaBindData>();
	auto index = MmcifIndex::Load(file_name, &context);
	vector<string> categories;
	index->GetCategoryNames(categories);
	for (auto &category : categories) {
		auto cat = index->FindCategory(category);
		if (!cat) {
			continue;
		}
		for (auto &col : cat->columns) {
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

// Split a "_category.item" key into (category, column), dropping the leading '_'.
static std::pair<std::string, std::string> MmcifSplitItem(const string &item) {
	auto dot = item.find('.');
	auto category = item.substr(1, dot - 1);
	auto column = item.substr(dot + 1);
	return {std::move(category), std::move(column)};
}

// mmcif_relationships(file): parent_table, parent_column, child_table, child_column
static unique_ptr<FunctionData> MmcifRelationshipsBind(ClientContext &context, TableFunctionBindInput &input,
                                                       vector<LogicalType> &return_types, vector<string> &names) {
	auto file_name = input.inputs[0].GetValue<string>();
	auto result = make_uniq<MmcifMetaBindData>();
	auto index = MmcifIndex::Load(file_name, &context);
	vector<string> categories;
	index->GetCategoryNames(categories);
	case_insensitive_set_t present(categories.begin(), categories.end());
	for (auto &rel : DictionaryIndex::Get().GetRelationships()) {
		auto parent_item = MmcifSplitItem(rel.first);
		auto child_item = MmcifSplitItem(rel.second);
		if (present.find(parent_item.first) != present.end() && present.find(child_item.first) != present.end()) {
			vector<string> row = {parent_item.first, parent_item.second, child_item.first, child_item.second};
			result->rows.push_back(std::move(row));
		}
	}
	names.emplace_back("parent_table");
	names.emplace_back("parent_column");
	names.emplace_back("child_table");
	names.emplace_back("child_column");
	return_types.push_back(LogicalType::VARCHAR);
	return_types.push_back(LogicalType::VARCHAR);
	return_types.push_back(LogicalType::VARCHAR);
	return_types.push_back(LogicalType::VARCHAR);
	return std::move(result);
}

// ---------------------------------------------------------------------------
// MmcifCatalog is defined later in this file; schema/table entries hold a
// pointer to it so write mode can reach the single persistent CifFile.
// ---------------------------------------------------------------------------
class MmcifCatalog;

// Resolve the CifFile to operate on (defined after MmcifCatalog is complete).
static CifFile *MmcifResolveCifFile(MmcifCatalog *catalog, const string &file_name, unique_ptr<CifFile> &local,
                                    optional_ptr<ClientContext> context = nullptr);

// Read-only lazy-index helpers (defined after MmcifCatalog is complete).
static bool MmcifCatalogIsWrite(MmcifCatalog *catalog);
static shared_ptr<MmcifIndex> MmcifCatalogGetIndex(MmcifCatalog *catalog, optional_ptr<ClientContext> context);

// ---------------------------------------------------------------------------
// MmcifTableEntry: a real TableCatalogEntry whose ColumnList carries dictionary
// types (so DESCRIBE shows DOUBLE/BIGINT/VARCHAR) and whose GetScanFunction
// returns a per-category scan with pre-filled bind data.
// ---------------------------------------------------------------------------

class MmcifTableEntry : public TableCatalogEntry {
public:
	MmcifTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info, string file_name_p,
	                string table_name_p, MmcifCatalog *catalog_p)
	    : TableCatalogEntry(catalog, schema, info), file_name(std::move(file_name_p)),
	      table_name(std::move(table_name_p)), catalog(catalog_p) {
	}

	string file_name;
	string table_name;
	MmcifCatalog *catalog;

	unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, column_t column_id) override {
		// Real min/max stats require a full category scan, which we deliberately
		// avoid to keep LIMIT/schema queries cheap. Stats are skipped; the planner
		// still gets exact cardinality from GetStorageInfo (recommendation 7).
		return nullptr;
	}

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
	MmcifSchemaEntry(Catalog &catalog, CreateSchemaInfo &info, string file_name_p, MmcifCatalog *catalog_p)
	    : SchemaCatalogEntry(catalog, info), file_name(std::move(file_name_p)), catalog(catalog_p) {
	}

	string file_name;
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
	if (MmcifCatalogIsWrite(this->catalog)) {
		unique_ptr<CifFile> local;
		auto cif_p = MmcifResolveCifFile(this->catalog, file_name, local, transaction.context);
		string first_block;
		auto &table = MmcifGetTable(*cif_p, entry_name, first_block);
		for (auto &col : table.GetColumnNames()) {
			info.columns.AddColumn(ColumnDefinition(col, dict.LookupType(entry_name, col)));
		}
	} else {
		auto index = MmcifCatalogGetIndex(this->catalog, transaction.context);
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
	if (MmcifCatalogIsWrite(catalog)) {
		unique_ptr<CifFile> local;
		auto cif_p = MmcifResolveCifFile(catalog, file_name, local, &context);
		auto first_block = cif_p->GetFirstBlockName();
		auto &block = cif_p->GetBlock(first_block);
		block.GetTableNames(categories);
	} else {
		auto index = MmcifCatalogGetIndex(catalog, &context);
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
	if (MmcifCatalogIsWrite(catalog)) {
		unique_ptr<CifFile> local;
		auto cif_p = MmcifResolveCifFile(catalog, file_name, local, transaction.context);
		auto first_block = cif_p->GetFirstBlockName();
		auto &block = cif_p->GetBlock(first_block);
		if (!block.IsTablePresent(entry_name)) {
			return nullptr;
		}
		return &GetTableEntry(transaction, entry_name);
	}
	auto index = MmcifCatalogGetIndex(catalog, transaction.context);
	if (!index->FindCategory(entry_name)) {
		return nullptr;
	}
	return &GetTableEntry(transaction, entry_name);
}

// ---------------------------------------------------------------------------
// MmcifCatalog: SQLite-style custom Catalog. Single "main" schema. Read-only
// by default; opened with READ_WRITE TRUE it owns one persistent CifFile that
// DML operators mutate and COMMIT/detach write back.
//
// The write-mode DML operators are defined after this class; the catalog's
// PlanInsert/PlanDelete/PlanUpdate member bodies instantiate them lazily, so
// only forward declarations are needed here.
// ---------------------------------------------------------------------------

class MmcifInsertOperator;
class MmcifDeleteOperator;
class MmcifUpdateOperator;

class MmcifCatalog : public Catalog {
public:
	MmcifCatalog(AttachedDatabase &db_p, string path_p, bool write_mode_p)
	    : Catalog(db_p), path(std::move(path_p)), write_mode(write_mode_p) {
		if (write_mode) {
			string diags;
			cif_file = MmcifParseFile(path, diags);
		}
	}

	string path;
	bool write_mode;
	unique_ptr<CifFile> cif_file;
	// Read-only lazy index (recommendation 1): built on first resolve, then
	// reused for every schema lookup, scan, and metadata query in this catalog.
	// The process-level content cache (recommendation 2) lives in MmcifIndex::Load.
	shared_ptr<MmcifIndex> index;
	mutex index_lock;

	bool IsWriteMode() const {
		return write_mode;
	}
	CifFile *GetCifFile() {
		return cif_file.get();
	}
	shared_ptr<MmcifIndex> GetIndex(optional_ptr<ClientContext> context) {
		if (write_mode) {
			return nullptr;
		}
		lock_guard<mutex> l(index_lock);
		if (!index) {
			index = MmcifIndex::Load(path, context);
		}
		return index;
	}
	// ROLLBACK: discard in-memory mutations by re-parsing the on-disk file.
	void ReloadFromDisk() {
		if (!write_mode) {
			return;
		}
		string diags;
		cif_file = MmcifParseFile(path, diags);
	}
	// COMMIT / detach / checkpoint: write the in-memory CifFile back to disk.
	// Paths ending in .gz are written back gzip-compressed (the read path
	// auto-decompresses them, so writing plain text would break the round-trip).
	void Persist(ClientContext &context) {
		if (!write_mode || !cif_file) {
			return;
		}
		if (MmcifIsRemotePath(path)) {
			throw IOException("mmcif: cannot write back to remote path %s - remote files are read-only", path);
		}
		if (StringUtil::EndsWith(StringUtil::Lower(path), ".gz")) {
			// CifFile::Write always emits plain text; run it through DuckDB's
			// gzip compression stream so the .cif.gz round-trips correctly.
			std::ostringstream ss;
			cif_file->Write(ss);
			auto content = ss.str();
			auto &fs = FileSystem::GetFileSystem(context);
			FileOpenFlags flags = FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW;
			flags.SetCompression(FileCompressionType::GZIP);
			auto handle = fs.OpenFile(path, flags);
			if (!content.empty()) {
				fs.Write(*handle, data_ptr_cast(&content[0]), content.size());
			}
			// Closing the handle flushes the gzip footer (deflate stream end).
			handle->Close();
		} else {
			cif_file->Write(path);
		}
	}

	void Initialize(bool load_builtin) override {
		CreateSchemaInfo info;
		main_schema = make_uniq<MmcifSchemaEntry>(*this, info, path, this);
	}

	void OnDetach(ClientContext &context) override {
		Persist(context);
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
		auto &insert = planner.Make<MmcifInsertOperator>(op.types, op.estimated_cardinality, *this, op.table.name,
		                                                 std::move(col_map));
		if (plan) {
			insert.children.push_back(*plan);
		}
		return insert;
	}
	PhysicalOperator &PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
	                             PhysicalOperator &plan) override {
		if (!write_mode) {
			throw BinderException("mmcif databases are read-only - cannot DELETE");
		}
		if (op.return_chunk) {
			throw NotImplementedException("mmcif write mode does not support DELETE RETURNING");
		}
		auto &bound_ref = op.expressions[0]->Cast<BoundReferenceExpression>();
		auto &del = planner.Make<MmcifDeleteOperator>(op.types, op.estimated_cardinality, *this, op.table.name,
		                                              bound_ref.index);
		del.children.push_back(plan);
		return del;
	}
	PhysicalOperator &PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
	                             PhysicalOperator &plan) override {
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

TableFunction MmcifTableEntry::GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) {
	auto result = make_uniq<MmcifBindData>();
	result->file_name = file_name;
	result->table_name = table_name;
	result->table_entry = this;

	if (catalog->IsWriteMode()) {
		// Write mode: materialized RCSB rows (DML mutates the persistent CifFile).
		unique_ptr<CifFile> local;
		auto cif_p = MmcifResolveCifFile(catalog, file_name, local, &context);
		string first_block;
		auto &table = MmcifGetTable(*cif_p, table_name, first_block);
		MmcifLoadRows(table, *result);
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
// Resolve the CifFile to operate on. Write mode returns the catalog's single
// persistent CifFile; read-only mode parses a fresh copy into `local` (kept
// alive by the caller).
// ---------------------------------------------------------------------------
static CifFile *MmcifResolveCifFile(MmcifCatalog *catalog, const string &file_name, unique_ptr<CifFile> &local,
                                    optional_ptr<ClientContext> context) {
	auto persistent = catalog->GetCifFile();
	if (persistent) {
		return persistent;
	}
	string diags;
	local = MmcifParseFile(file_name, diags, context);
	return local.get();
}

static bool MmcifCatalogIsWrite(MmcifCatalog *catalog) {
	return catalog->IsWriteMode();
}

static shared_ptr<MmcifIndex> MmcifCatalogGetIndex(MmcifCatalog *catalog, optional_ptr<ClientContext> context) {
	return catalog->GetIndex(context);
}

// ---------------------------------------------------------------------------
// Write-mode DML operators (D3): custom physical sinks that read the input
// chunk and apply row-level mutations to the catalog's persistent CifFile.
// row_id == physical ISTable row index (scans emit row_id = row index).
// ---------------------------------------------------------------------------

struct MmcifWriteGlobalState : public GlobalSinkState {
	idx_t count = 0;
	mutex lock;
	// DELETE row ids are accumulated across sink chunks and applied once in Combine:
	// row ids refer to positions in the table as it was when the scan started, so
	// applying them chunk-by-chunk would use stale indices after the first shrink.
	vector<unsigned int> delete_indices;
};

static string MmcifCellToString(const Vector &vec, idx_t row) {
	auto val = vec.GetValue(row);
	if (val.IsNull()) {
		return ""; // NULL -> empty cell -> written back as "?"
	}
	return val.ToString();
}

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
		auto &cif = *catalog.GetCifFile();
		string first_block;
		auto &table = MmcifGetTable(cif, table_name, first_block);
		auto col_names = table.GetColumnNames();
		idx_t num_cols = col_names.size();
		chunk.Flatten();
		lock_guard<mutex> l(gstate.lock);
		for (idx_t r = 0; r < chunk.size(); r++) {
			std::vector<std::string> row(num_cols, "");
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
			table.AddRow(row);
			gstate.count++;
		}
		return SinkResultType::NEED_MORE_INPUT;
	}
	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override {
		return make_uniq<GlobalSourceState>();
	}
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk, OperatorSourceInput &input) const override {
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
		vector<unsigned int> indices;
		indices.swap(gstate.delete_indices);
		sort(indices.begin(), indices.end());
		indices.erase(unique(indices.begin(), indices.end()), indices.end());
		if (!indices.empty()) {
			auto &cif = *catalog.GetCifFile();
			string first_block;
			auto &table = MmcifGetTable(cif, table_name, first_block);
			table.DeleteRows(indices);
			gstate.count += indices.size();
		}
		return SinkCombineResultType::FINISHED;
	}
	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override {
		return make_uniq<GlobalSourceState>();
	}
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk, OperatorSourceInput &input) const override {
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
		auto &cif = *catalog.GetCifFile();
		string first_block;
		auto &table = MmcifGetTable(cif, table_name, first_block);
		auto col_names = table.GetColumnNames();
		chunk.Flatten();
		auto &row_ids = chunk.data[chunk.ColumnCount() - 1];
		auto row_data = FlatVector::GetData<int64_t>(row_ids);
		lock_guard<mutex> l(gstate.lock);
		for (idx_t r = 0; r < chunk.size(); r++) {
			for (idx_t i = 0; i < columns.size(); i++) {
				table.UpdateCell(NumericCast<unsigned int>(row_data[r]), col_names[columns[i]],
				                 MmcifCellToString(chunk.data[expr_indices[i]], r));
			}
		}
		gstate.count += chunk.size();
		return SinkResultType::NEED_MORE_INPUT;
	}
	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override {
		return make_uniq<GlobalSourceState>();
	}
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk, OperatorSourceInput &input) const override {
		auto &g = sink_state->Cast<MmcifWriteGlobalState>();
		chunk.SetCardinality(1);
		chunk.SetValue(0, 0, Value::BIGINT(NumericCast<int64_t>(g.count)));
		return SourceResultType::FINISHED;
	}
};

// ---------------------------------------------------------------------------
// Read-only transaction manager (DuckTransactionManager requires a DuckCatalog).
// ---------------------------------------------------------------------------

class MmcifTransactionManager : public TransactionManager {
public:
	MmcifTransactionManager(AttachedDatabase &db, MmcifCatalog &catalog_p) : TransactionManager(db), catalog(catalog_p) {
	}

	MmcifCatalog &catalog;

	Transaction &StartTransaction(ClientContext &context) override {
		auto transaction = make_uniq<Transaction>(*this, context);
		auto &result = *transaction;
		lock_guard<mutex> l(lock);
		transactions[result] = std::move(transaction);
		return result;
	}

	ErrorData CommitTransaction(ClientContext &context, Transaction &transaction) override {
		// D5: write the mutated in-memory CifFile back to the attached .cif on COMMIT.
		catalog.Persist(context);
		lock_guard<mutex> l(lock);
		transactions.erase(transaction);
		return ErrorData();
	}

	void RollbackTransaction(Transaction &transaction) override {
		// D6: ROLLBACK discards in-memory mutations by re-parsing from disk.
		catalog.ReloadFromDisk();
		lock_guard<mutex> l(lock);
		transactions.erase(transaction);
	}

	void Checkpoint(ClientContext &context, bool force = false) override {
		if (!catalog.IsWriteMode()) {
			throw NotImplementedException("Cannot CHECKPOINT a read-only mmcif database");
		}
		catalog.Persist(context);
	}

private:
	mutex lock;
	reference_map_t<Transaction, unique_ptr<Transaction>> transactions;
};

// ---------------------------------------------------------------------------
// Attach + Load registration
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
	if (write_mode && MmcifIsRemotePath(info.path)) {
		throw InvalidInputException(
		    "mmcif: remote file '%s' cannot be attached with READ_WRITE - remote mmcif files are read-only",
		    info.path);
	}
	return make_uniq<MmcifCatalog>(db, info.path, write_mode);
}

static unique_ptr<TransactionManager> MmcifCreateTransactionManager(optional_ptr<StorageExtensionInfo> storage_info,
                                                                    AttachedDatabase &db, Catalog &catalog) {
	auto &mmcif_catalog = catalog.Cast<MmcifCatalog>();
	return make_uniq<MmcifTransactionManager>(db, mmcif_catalog);
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
