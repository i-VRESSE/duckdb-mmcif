// Per-category scan + metadata table functions module.
//
// Global scans union category schemas across expanded paths and stream one
// file at a time. Attached scans use the same reader or a transaction's write
// store, with their bind data pre-filled by GetScanFunction.

#include "mmcif_table_functions.hpp"

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/multi_file/multi_file_reader.hpp"
#include "duckdb/common/operator/numeric_cast.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

#include <utility>

#include "mmcif_dictionary.hpp"

namespace duckdb {

// ---------------------------------------------------------------------------
// MmcifBindData
// ---------------------------------------------------------------------------

unique_ptr<FunctionData> MmcifBindData::Copy() const {
	return make_uniq<MmcifBindData>(*this);
}

bool MmcifBindData::Equals(const FunctionData &other) const {
	return false;
}

struct MmcifGlobalState : public GlobalTableFunctionState {
	MmcifGlobalState(const MmcifBindData &bind_p, const vector<column_t> &column_ids_p)
	    : bind(bind_p), column_ids(column_ids_p), position(0) {
		if (bind.dictionary_schema) {
			for (idx_t c = 0; c < bind.column_names.size(); c++) {
				if (c != bind.filename_column) {
					dictionary_columns[bind.column_names[c]] = c;
				}
			}
		} else if (!bind.files.empty()) {
			StartFile(0);
		} else if (bind.index && bind.category) {
			StartCategory(bind.index, bind.category, nullptr);
		}
		if (bind.write_category) {
			write_end = bind.write_category->rows.size();
		}
	}
	void StartCategory(shared_ptr<MmcifIndex> index_p, MmcifCategory *category_p, const vector<idx_t> *mapping) {
		index = std::move(index_p);
		category = category_p;
		position = 0;
		done = single_done = false;
		ncols = category->columns.size();
		full_to_out.assign(ncols, DConstants::INVALID_INDEX);
		out_to_full.assign(column_ids.size(), DConstants::INVALID_INDEX);
		for (idx_t local = 0; local < ncols; local++) {
			auto union_id = mapping ? (*mapping)[local] : local;
			for (idx_t c = 0; c < column_ids.size(); c++) {
				if (column_ids[c] == union_id) {
					full_to_out[local] = c;
					out_to_full[c] = local;
				}
			}
		}
		single_by_col.assign(ncols, nullptr);
		for (auto &cell : category->singles) {
			single_by_col[cell.col] = &cell;
		}
		cursor.reset();
		if (category->is_loop) {
			cursor = make_uniq<MmcifValueCursor>(index->GetData(), category->data_start, category->data_end);
		}
	}

	void StartFile(idx_t file_idx) {
		file_position = file_idx;
		auto &file = bind.files[file_idx];
		StartCategory(file.index, file.category, &file.local_to_union);
	}

	bool NextDictionaryFile(ClientContext &context) {
		// The output owns projected VARCHAR cells, so the previous arena can die.
		cursor.reset();
		category = nullptr;
		single_by_col.clear();
		index.reset();
		while (next_path < bind.paths.size()) {
			file_position = next_path++;
			auto loaded =
			    MmcifIndex::Load(bind.paths[file_position], &context, bind.has_data_block ? &bind.data_block : nullptr);
			auto cat = loaded->FindCategory(bind.table_name);
			if (!cat || cat->columns.empty()) {
				continue;
			}
			vector<idx_t> mapping;
			for (auto &name : cat->columns) {
				auto found = dictionary_columns.find(name);
				if (found == dictionary_columns.end()) {
					throw InvalidInputException("mmcif: item '%s.%s' in '%s' is not in the bundled dictionary; use "
					                            "column_source := 'files' for custom items",
					                            bind.table_name, name, bind.paths[file_position]);
				}
				mapping.push_back(found->second);
			}
			StartCategory(std::move(loaded), cat, &mapping);
			return true;
		}
		return false;
	}

	idx_t next_path = 0;
	case_insensitive_map_t<idx_t> dictionary_columns;

	shared_ptr<MmcifIndex> index;
	MmcifCategory *category = nullptr;
	idx_t file_position = 0;
	vector<idx_t> out_to_full;
	const MmcifBindData &bind;
	vector<column_t> column_ids;
	idx_t position;
	idx_t ncols = 0;
	vector<idx_t> full_to_out;                     // full column index -> output position
	vector<const MmcifSingleCell *> single_by_col; // full column index -> single cell (broadcast)
	unique_ptr<MmcifValueCursor> cursor;
	// Write mode: rows past this one were added after the scan started (an
	// INSERT ... SELECT from the same table) and are not scanned.
	idx_t write_end = 0;
	bool done = false;
	bool single_done = false;

	idx_t MaxThreads() const override {
		return 1;
	}
};

void MmcifBindIndex(MmcifBindData &result, shared_ptr<MmcifIndex> index, const string &table_name) {
	auto cat = index->FindCategory(table_name);
	if (!cat || cat->columns.empty()) {
		throw BinderException("mmcif: category '%s' not present in block '%s'", table_name.c_str(),
		                      index->GetDataBlockName().c_str());
	}
	result.index = std::move(index);
	result.category = cat;
	result.column_names = cat->columns;
	for (auto &col : result.column_names) {
		result.column_types.push_back(DictionaryIndex::Get().LookupType(table_name, col));
	}
}

vector<string> MmcifExpandFiles(ClientContext &context, const Value &input) {
	auto reader = MultiFileReader::CreateDefault("mmcif");
	vector<string> paths;
	for (auto &file : reader->CreateFileList(context, input)->GetAllFiles()) {
		paths.push_back(file.path);
	}
	return paths;
}

void MmcifBindFiles(MmcifBindData &result, const vector<string> &paths, const vector<shared_ptr<MmcifIndex>> &indexes,
                    const string &table_name) {
	case_insensitive_map_t<idx_t> columns;
	for (idx_t i = 0; i < paths.size(); i++) {
		auto category = indexes[i]->FindCategory(table_name);
		if (!category || category->columns.empty()) {
			continue;
		}
		MmcifScanFile file;
		file.path = paths[i];
		file.index = indexes[i];
		file.category = category;
		for (auto &name : category->columns) {
			auto entry = columns.find(name);
			if (entry == columns.end()) {
				auto id = result.column_names.size();
				columns[name] = id;
				result.column_names.push_back(name);
				result.column_types.push_back(DictionaryIndex::Get().LookupType(table_name, name));
				file.local_to_union.push_back(id);
			} else {
				file.local_to_union.push_back(entry->second);
			}
		}
		result.files.push_back(std::move(file));
	}
	if (result.files.empty()) {
		if (indexes.size() == 1) {
			// Preserve the single-file error, including the selected block name.
			MmcifBindIndex(result, indexes[0], table_name);
		}
		throw BinderException("mmcif: category '%s' not present in any matched file", table_name);
	}
	if (paths.size() > 1) {
		if (columns.find("filename") != columns.end()) {
			throw BinderException("mmcif: category '%s' already contains a filename column", table_name);
		}
		result.filename_column = result.column_names.size();
		result.column_names.push_back("filename");
		result.column_types.push_back(LogicalType::VARCHAR);
	}
}

static unique_ptr<string> MmcifDataBlock(TableFunctionBindInput &input) {
	auto it = input.named_parameters.find("data_block");
	if (it == input.named_parameters.end()) {
		return nullptr;
	}
	if (it->second.IsNull()) {
		throw BinderException("mmcif: data_block must be a non-NULL string");
	}
	return make_uniq<string>(it->second.GetValue<string>());
}

static unique_ptr<FunctionData> MmcifBind(ClientContext &context, TableFunctionBindInput &input,
                                          vector<LogicalType> &return_types, vector<string> &names) {
	auto paths = MmcifExpandFiles(context, input.inputs[0]);
	auto table_name = input.inputs[1].GetValue<string>();
	auto result = make_uniq<MmcifBindData>();

	auto data_block = MmcifDataBlock(input);
	auto column_source = input.named_parameters.find("column_source");
	string column_source_mode = "files";
	if (column_source != input.named_parameters.end()) {
		if (column_source->second.IsNull()) {
			throw BinderException("mmcif: column_source must be 'files' or 'dictionary', not NULL");
		}
		column_source_mode = StringUtil::Lower(column_source->second.GetValue<string>());
	}
	if (column_source_mode == "dictionary") {
		auto columns = DictionaryIndex::Get().GetColumns(table_name);
		if (!columns) {
			throw BinderException("mmcif: category '%s' is not in the bundled dictionary; use column_source := 'files' "
			                      "for custom categories",
			                      table_name);
		}
		result->dictionary_schema = true;
		result->paths = std::move(paths);
		result->table_name = table_name;
		result->has_data_block = bool(data_block);
		if (data_block) {
			result->data_block = *data_block;
		}
		result->column_names.assign(columns->begin(), columns->end());
		for (auto &name : *columns) {
			result->column_types.push_back(DictionaryIndex::Get().LookupType(table_name, name));
		}
		if (result->paths.size() > 1) {
			for (auto &name : *columns) {
				if (StringUtil::CIEquals(name, "filename")) {
					throw BinderException("mmcif: category '%s' already contains a filename column", table_name);
				}
			}
			result->filename_column = result->column_names.size();
			result->column_names.push_back("filename");
			result->column_types.push_back(LogicalType::VARCHAR);
		}
	} else if (column_source_mode == "files") {
		vector<shared_ptr<MmcifIndex>> indexes;
		for (auto &path : paths) {
			indexes.push_back(MmcifIndex::Load(path, &context, data_block.get()));
		}
		MmcifBindFiles(*result, paths, indexes, table_name);
	} else {
		throw BinderException("mmcif: column_source must be 'files' or 'dictionary'");
	}
	names.assign(result->column_names.begin(), result->column_names.end());
	return_types.assign(result->column_types.begin(), result->column_types.end());
	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> MmcifInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<MmcifBindData>();
	return make_uniq<MmcifGlobalState>(bind, input.column_ids);
}

// Index-backed scan: parse the category's loop range
// incrementally from the byte cursor, row-major, into per-column VARCHAR
// vectors, then vectorized-cast each column to its dictionary type. LIMIT
// pushdown falls out naturally: we stop after the requested rows are filled.
static idx_t MmcifScanIndex(DataChunk &output, MmcifGlobalState &gstate, vector<unique_ptr<Vector>> &tmp,
                            vector<string_t *> &ptrs) {
	auto &cat = *gstate.category;
	idx_t out_cols = output.ColumnCount();

	idx_t count = 0;
	if (cat.is_loop) {
		idx_t loop_ncols = cat.loop_col_map.size();
		while (count < STANDARD_VECTOR_SIZE && !gstate.done) {
			idx_t li = 0;
			for (; li < loop_ncols; li++) {
				idx_t out_pos = gstate.full_to_out[cat.loop_col_map[li]];
				const char *out;
				idx_t len;
				bool is_null;
				if (!gstate.cursor->Next(&out, &len, &is_null)) {
					gstate.done = true;
					break;
				}
				if (out_pos != DConstants::INVALID_INDEX) {
					if (is_null) {
						FlatVector::SetNull(*tmp[out_pos], count, true);
					} else {
						MmcifUnquote(out, len);
						ptrs[out_pos][count] =
						    gstate.bind.dictionary_schema && output.data[out_pos].GetType() == LogicalType::VARCHAR
						        ? StringVector::AddString(*tmp[out_pos], out, len)
						        : string_t(out, UnsafeNumericCast<uint32_t>(len));
					}
				}
			}
			if (li == 0) {
				break; // cursor exhausted at a row boundary
			}
			// A partial trailing row: its missing values are NULL.
			for (; li < loop_ncols; li++) {
				idx_t out_pos = gstate.full_to_out[cat.loop_col_map[li]];
				if (out_pos != DConstants::INVALID_INDEX) {
					FlatVector::SetNull(*tmp[out_pos], count, true);
				}
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
			return 0;
		}
		for (idx_t c = 0; c < out_cols; c++) {
			auto col_id = gstate.column_ids[c];
			if (col_id == COLUMN_IDENTIFIER_ROW_ID) {
				output.data[c].SetValue(0, Value::Numeric(LogicalType::BIGINT, 0));
			} else if (col_id == COLUMN_IDENTIFIER_EMPTY) {
				output.data[c].SetValue(0, Value(true));
			} else {
				auto local = gstate.out_to_full[c];
				auto sc = local < gstate.ncols ? gstate.single_by_col[local] : nullptr;
				if (sc && !sc->is_null) {
					const char *value = gstate.index->GetData() + sc->off;
					idx_t len = sc->len;
					MmcifUnquote(value, len);
					ptrs[c][0] = gstate.bind.dictionary_schema && output.data[c].GetType() == LogicalType::VARCHAR
					                 ? StringVector::AddString(*tmp[c], value, len)
					                 : string_t(value, UnsafeNumericCast<uint32_t>(len));
				} else {
					FlatVector::SetNull(*tmp[c], 0, true);
				}
			}
		}
		count = 1;
		gstate.single_done = true;
	}

	return count;
}

// Write-mode scan: the store's rows, by index (an INSERT sink may grow the row
// vector between chunks). Cells are copied into the vector because an UPDATE
// sink overwrites store cells while this chunk's values are still in use.
static idx_t MmcifScanStore(DataChunk &output, MmcifGlobalState &gstate, vector<unique_ptr<Vector>> &tmp,
                            vector<string_t *> &ptrs) {
	auto &rows = gstate.bind.write_category->rows;
	idx_t count = 0;
	for (; gstate.position < gstate.write_end && count < STANDARD_VECTOR_SIZE; gstate.position++, count++) {
		auto &row = rows[gstate.position];
		for (idx_t c = 0; c < output.ColumnCount(); c++) {
			auto col_id = gstate.column_ids[c];
			if (col_id == COLUMN_IDENTIFIER_ROW_ID) {
				output.data[c].SetValue(count, Value::Numeric(LogicalType::BIGINT, gstate.position));
			} else if (col_id == COLUMN_IDENTIFIER_EMPTY) {
				output.data[c].SetValue(count, Value(true));
			} else if (MmcifBindData::IsNullCell(row[col_id])) {
				FlatVector::SetNull(*tmp[c], count, true);
			} else {
				ptrs[c][count] = StringVector::AddString(*tmp[c], row[col_id]);
			}
		}
	}
	return count;
}

// Both modes fill per-column VARCHAR vectors, then cast each column to its
// dictionary type in one vectorized pass.
static void MmcifScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &gstate = data.global_state->Cast<MmcifGlobalState>();
	if (gstate.bind.dictionary_schema && !gstate.category && !gstate.NextDictionaryFile(context)) {
		output.SetCardinality(0);
		return;
	}
	idx_t out_cols = output.ColumnCount();
	vector<unique_ptr<Vector>> tmp(out_cols);
	vector<string_t *> ptrs(out_cols);
	for (idx_t c = 0; c < out_cols; c++) {
		tmp[c] = make_uniq<Vector>(LogicalType::VARCHAR);
		ptrs[c] = FlatVector::GetData<string_t>(*tmp[c]);
	}
	idx_t count;
	while (true) {
		// Missing union columns start as NULL for each file/chunk.
		for (idx_t c = 0; c < out_cols; c++) {
			FlatVector::Validity(*tmp[c]).SetAllValid(STANDARD_VECTOR_SIZE);
			if (!gstate.bind.write_category && gstate.out_to_full[c] == DConstants::INVALID_INDEX) {
				FlatVector::Validity(*tmp[c]).SetAllInvalid(STANDARD_VECTOR_SIZE);
			}
		}
		count = gstate.bind.write_category ? MmcifScanStore(output, gstate, tmp, ptrs)
		                                   : MmcifScanIndex(output, gstate, tmp, ptrs);
		if (count) {
			break;
		}
		if (gstate.bind.dictionary_schema) {
			if (!gstate.NextDictionaryFile(context)) {
				break;
			}
		} else {
			if (gstate.bind.files.empty() || gstate.file_position + 1 == gstate.bind.files.size()) {
				break;
			}
			gstate.StartFile(gstate.file_position + 1);
		}
	}
	if (gstate.bind.filename_column != DConstants::INVALID_INDEX) {
		for (idx_t c = 0; c < out_cols; c++) {
			if (gstate.column_ids[c] == gstate.bind.filename_column) {
				tmp[c]->Reference(Value(gstate.bind.dictionary_schema ? gstate.bind.paths[gstate.file_position]
				                                                      : gstate.bind.files[gstate.file_position].path));
			}
		}
	}
	for (idx_t c = 0; c < out_cols; c++) {
		auto col_id = gstate.column_ids[c];
		if (col_id == COLUMN_IDENTIFIER_ROW_ID || col_id == COLUMN_IDENTIFIER_EMPTY) {
			continue;
		}
		VectorOperations::Cast(context, *tmp[c], output.data[c], count);
	}
	output.SetCardinality(count);
}

TableFunction MmcifScanFunction() {
	TableFunction result("mmcif_scan", {LogicalType::VARCHAR, LogicalType::VARCHAR}, MmcifScan, MmcifBind,
	                     MmcifInitGlobal);
	result.projection_pushdown = true;
	result.named_parameters["column_source"] = LogicalType::VARCHAR;
	return result;
}

// ---------------------------------------------------------------------------
// Metadata table functions (global): mmcif_tables(file),
// mmcif_columns(file), and mmcif_relationships(file), filtered to categories
// present in the file.
// ---------------------------------------------------------------------------

struct MmcifMetaBindData : public FunctionData {
	std::vector<std::vector<Value>> rows;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<MmcifMetaBindData>(*this);
	}
	bool Equals(const FunctionData &other) const override {
		return false;
	}
};

struct MmcifMetaGlobalState : public GlobalTableFunctionState {
	explicit MmcifMetaGlobalState(const vector<column_t> &column_ids_p) : column_ids(column_ids_p) {
	}
	vector<column_t> column_ids;
	idx_t position = 0;

	idx_t MaxThreads() const override {
		return 1;
	}
};

static unique_ptr<GlobalTableFunctionState> MmcifMetaInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<MmcifMetaGlobalState>(input.column_ids);
}

static void MmcifMetaScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &gstate = data.global_state->Cast<MmcifMetaGlobalState>();
	auto &rows = data.bind_data->Cast<MmcifMetaBindData>().rows;
	idx_t count = 0;
	for (; gstate.position < rows.size() && count < STANDARD_VECTOR_SIZE; gstate.position++, count++) {
		for (idx_t c = 0; c < output.ColumnCount(); c++) {
			output.data[c].SetValue(count, rows[gstate.position][gstate.column_ids[c]]);
		}
	}
	output.SetCardinality(count);
}

// mmcif_blocks(file): names in file order, without the data_ prefix.
static unique_ptr<FunctionData> MmcifBlocksBind(ClientContext &context, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<MmcifMetaBindData>();
	auto index = MmcifIndex::Load(input.inputs[0].GetValue<string>(), &context);
	for (auto &name : index->GetDataBlockNames()) {
		result->rows.push_back({Value(name)});
	}
	names = {"block_name"};
	return_types = {LogicalType::VARCHAR};
	return std::move(result);
}

// mmcif_tables(file): table_name, comment, column_count
static unique_ptr<FunctionData> MmcifTablesBind(ClientContext &context, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<string> &names) {
	auto file_name = input.inputs[0].GetValue<string>();
	auto result = make_uniq<MmcifMetaBindData>();
	auto index = MmcifIndex::Load(file_name, &context, MmcifDataBlock(input).get());
	auto &dictionary = DictionaryIndex::Get();
	for (auto &cat : index->GetCategories()) {
		result->rows.push_back({Value(cat->name), Value(dictionary.GetCategoryUrl(cat->name)),
		                        Value::BIGINT(NumericCast<int64_t>(cat->columns.size()))});
	}
	names = {"table_name", "comment", "column_count"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BIGINT};
	return std::move(result);
}

// mmcif_columns(file): table_name, column_name, column_index, comment, data_type
static unique_ptr<FunctionData> MmcifColumnsBind(ClientContext &context, TableFunctionBindInput &input,
                                                 vector<LogicalType> &return_types, vector<string> &names) {
	auto file_name = input.inputs[0].GetValue<string>();
	auto result = make_uniq<MmcifMetaBindData>();
	auto index = MmcifIndex::Load(file_name, &context, MmcifDataBlock(input).get());
	auto &dictionary = DictionaryIndex::Get();
	for (auto &cat : index->GetCategories()) {
		auto &category = cat->name;
		for (idx_t column_index = 0; column_index < cat->columns.size(); column_index++) {
			auto &col = cat->columns[column_index];
			auto type = dictionary.LookupType(category, col);
			vector<Value> row = {Value(category), Value(col), Value::INTEGER(NumericCast<int32_t>(column_index + 1)),
			                     Value(dictionary.GetItemUrl(category, col)), Value(type.ToString())};
			result->rows.push_back(std::move(row));
		}
	}
	names = {"table_name", "column_name", "column_index", "comment", "data_type"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::INTEGER, LogicalType::VARCHAR,
	                LogicalType::VARCHAR};
	return std::move(result);
}

// Split a "_category.item" key into (category, column), dropping the leading '_'.
static std::pair<string, string> MmcifSplitItem(const string &item) {
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
	auto index = MmcifIndex::Load(file_name, &context, MmcifDataBlock(input).get());
	auto categories = index->GetCategoryNames();
	case_insensitive_set_t present(categories.begin(), categories.end());
	for (auto &rel : DictionaryIndex::Get().GetRelationships()) {
		auto parent_item = MmcifSplitItem(rel.first);
		auto child_item = MmcifSplitItem(rel.second);
		if (present.find(parent_item.first) != present.end() && present.find(child_item.first) != present.end()) {
			vector<Value> row = {Value(parent_item.first), Value(parent_item.second), Value(child_item.first),
			                     Value(child_item.second)};
			result->rows.push_back(std::move(row));
		}
	}
	names = {"parent_table", "parent_column", "child_table", "child_column"};
	return_types = vector<LogicalType>(4, LogicalType::VARCHAR);
	return std::move(result);
}

// Attach a FunctionDescription to a table function so its purpose, parameter
// names, examples and category are discoverable through duckdb_functions().
static void MmcifRegisterDescribed(ExtensionLoader &loader, TableFunction function, vector<string> parameter_names,
                                   string description, vector<string> examples) {
	if (function.name != "mmcif_blocks") {
		function.named_parameters["data_block"] = LogicalType::VARCHAR;
		parameter_names.push_back("data_block");
		description += " Select a block with data_block := 'name'; defaults to the first block.";
	}
	if (function.name == "mmcif_scan") {
		parameter_names.push_back("column_source");
		description += " Use column_source := 'dictionary' to bind known dictionary columns and open files lazily.";
	}
	FunctionDescription desc;
	desc.parameter_names = std::move(parameter_names);
	desc.description = std::move(description);
	desc.examples = std::move(examples);
	desc.categories = {"mmcif"};
	CreateTableFunctionInfo info(function.name == "mmcif_scan" ? MultiFileReader::CreateFunctionSet(std::move(function))
	                                                           : TableFunctionSet(std::move(function)));
	info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
	for (auto &overload : info.functions.functions) {
		desc.parameter_types = overload.arguments;
		info.descriptions.push_back(desc);
	}
	loader.RegisterFunction(std::move(info));
}

void MmcifRegisterTableFunctions(ExtensionLoader &loader) {
	TableFunction mmcif_blocks("mmcif_blocks", {LogicalType::VARCHAR}, MmcifMetaScan, MmcifBlocksBind,
	                           MmcifMetaInitGlobal);
	mmcif_blocks.projection_pushdown = true;
	MmcifRegisterDescribed(loader, std::move(mmcif_blocks), {"file"},
	                       "List data block names in file order, without the data_ prefix.",
	                       {"SELECT * FROM mmcif_blocks('structures.cif');"});

	// mmcif_scan(file, table): scan one category of an mmCIF file as a table.
	MmcifRegisterDescribed(
	    loader, MmcifScanFunction(), {"file", "table"},
	    "Scan one mmCIF category from paths or globs, unioning columns by name with filename for multiple files.",
	    {"SELECT * FROM mmcif_scan('https://files.rcsb.org/download/1AMB.cif.gz', 'atom_site'); "
	     "-- 438 rows"});

	// mmcif_tables(file): one row per category with its dictionary page and column count.
	TableFunction mmcif_tables("mmcif_tables", {LogicalType::VARCHAR}, MmcifMetaScan, MmcifTablesBind,
	                           MmcifMetaInitGlobal);
	mmcif_tables.projection_pushdown = true;
	MmcifRegisterDescribed(loader, std::move(mmcif_tables), {"file"},
	                       "List the categories in an mmCIF file with their dictionary documentation links and "
	                       "column counts.",
	                       {"SELECT * FROM mmcif_tables('https://files.rcsb.org/download/1AMB.cif.gz');"});

	// mmcif_columns(file): one row per (category, column) with its inferred type.
	TableFunction mmcif_columns("mmcif_columns", {LogicalType::VARCHAR}, MmcifMetaScan, MmcifColumnsBind,
	                            MmcifMetaInitGlobal);
	mmcif_columns.projection_pushdown = true;
	MmcifRegisterDescribed(loader, std::move(mmcif_columns), {"file"},
	                       "List the categories and columns in an mmCIF file, with their dictionary documentation "
	                       "links and inferred types.",
	                       {"SELECT * FROM mmcif_columns('https://files.rcsb.org/download/1AMB.cif.gz'); "
	                        "-- 342 rows"});

	// mmcif_relationships(file): parent/child (table, column) key pairs.
	TableFunction mmcif_relationships("mmcif_relationships", {LogicalType::VARCHAR}, MmcifMetaScan,
	                                  MmcifRelationshipsBind, MmcifMetaInitGlobal);
	mmcif_relationships.projection_pushdown = true;
	MmcifRegisterDescribed(loader, std::move(mmcif_relationships), {"file"},
	                       "List the parent/child key relationships between the categories in an mmCIF file, each side "
	                       "as a (table, column) pair.",
	                       {"SELECT * FROM mmcif_relationships('https://files.rcsb.org/download/1AMB.cif.gz'); "
	                        "-- 86 rows"});
}

} // namespace duckdb
