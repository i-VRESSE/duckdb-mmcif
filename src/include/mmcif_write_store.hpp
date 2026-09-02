// mmcif write model: the no-deps mutable write store.
//
// Evicted from the read-index header (the read path never references it).
// Write mode keeps one persistent MmcifWriteStore per attached catalog instead
// of the RCSB CifFile/ISTable core. Cells are materialized (row-major
// vector<string>) so DML can mutate in place and a plain-text writer can emit
// the file back. Null cells are stored as "." / "?" (the RCSB parser's stored
// forms), so the writer re-emits them unchanged; "" also maps to "?" on emit.
//
// The read seam is MmcifIndex::Materialize(): the store is materialized from
// the (read-only) index there, so MmcifWriteStore holds no index back-pointers
// and the index header carries no write knowledge.

#ifndef DUCKDB_MMCIF_WRITE_STORE_HPP
#define DUCKDB_MMCIF_WRITE_STORE_HPP

#include "duckdb/common/string.hpp"
#include "duckdb/common/typedefs.hpp"

#include <memory>
#include <string>
#include <vector>

namespace duckdb {

class MmcifIndex; // read seam: MmcifIndex::Materialize()

struct MmcifWriteCategory {
	string name;
	bool is_loop = false;
	std::vector<string> columns;           // item names, in first-seen order
	std::vector<std::vector<string>> rows; // row-major cell strings, one per column
};

class MmcifWriteStore {
public:
	MmcifWriteStore() = default;

	string data_block_name;
	std::vector<MmcifWriteCategory> categories;

	MmcifWriteCategory *FindCategory(const string &name);
	std::vector<string> GetCategoryNames() const;
	idx_t GetNumRows(MmcifWriteCategory &cat) const;
	const std::vector<string> &GetRow(MmcifWriteCategory &cat, idx_t row) const;
	void AddRow(MmcifWriteCategory &cat, const std::vector<string> &row);
	void DeleteRows(MmcifWriteCategory &cat, const std::vector<unsigned int> &rows);
	void UpdateCell(MmcifWriteCategory &cat, idx_t row, const string &col, const string &value);
};

} // namespace duckdb

#endif
