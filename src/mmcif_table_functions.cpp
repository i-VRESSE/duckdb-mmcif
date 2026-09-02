// Per-category scan + metadata table functions module.
//
// MmcifBindData carries the parsed rows + dictionary types. Used both as the
// global mmcif_scan(file, table) table function (bind reads the two VARCHAR
// args) and as the attached-table scan (GetScanFunction pre-fills bind_data,
// so bind is never called).

#include "mmcif_table_functions.hpp"

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/operator/numeric_cast.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <utility>

#include "mmcif_dictionary.hpp"

namespace duckdb {

// ---------------------------------------------------------------------------
// MmcifBindData
// ---------------------------------------------------------------------------

unique_ptr<FunctionData> MmcifBindData::Copy() const {
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

bool MmcifBindData::Equals(const FunctionData &other) const {
	return false;
}

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

TableFunction MmcifScanFunction() {
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
	std::vector<std::vector<string>> rows; // one row of strings per output row

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

void MmcifRegisterTableFunctions(ExtensionLoader &loader) {
	loader.RegisterFunction(MmcifScanFunction());

	TableFunction mmcif_tables("mmcif_tables", {LogicalType::VARCHAR}, MmcifMetaScan, MmcifTablesBind,
	                           MmcifMetaInitGlobal);
	mmcif_tables.projection_pushdown = true;
	loader.RegisterFunction(mmcif_tables);

	TableFunction mmcif_relationships("mmcif_relationships", {LogicalType::VARCHAR}, MmcifMetaScan,
	                                  MmcifRelationshipsBind, MmcifMetaInitGlobal);
	mmcif_relationships.projection_pushdown = true;
	loader.RegisterFunction(mmcif_relationships);
}

} // namespace duckdb
