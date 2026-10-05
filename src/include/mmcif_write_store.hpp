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

#include "mmcif_comments.hpp"

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
	// The comments table: every '#' line read from the source file, in file
	// order, each carrying the anchor the writer needs to re-emit it where it
	// belongs. DML never touches it, so comments survive INSERT/UPDATE/DELETE.
	std::vector<MmcifComment> comments;

	MmcifWriteCategory *FindCategory(const string &name);
	std::vector<string> GetCategoryNames() const;
	idx_t GetNumRows(MmcifWriteCategory &cat) const;
	const std::vector<string> &GetRow(MmcifWriteCategory &cat, idx_t row) const;
	void AddRow(MmcifWriteCategory &cat, const std::vector<string> &row);
	void DeleteRows(MmcifWriteCategory &cat, const std::vector<unsigned int> &rows);
	void UpdateCell(MmcifWriteCategory &cat, idx_t row, const string &col, const string &value);

	// COMMIT/detach/checkpoint write the store back to disk only when it is
	// dirty (some DML mutated it since materialization). Read-only transactions
	// therefore never rewrite the file.
	bool IsDirty() const {
		return dirty;
	}
	void ClearDirty() {
		dirty = false;
	}

	// True when the source file carries content (extra data blocks / save
	// frames) that the regenerating writer cannot preserve. Write-mode attach
	// and write-back are refused for such files instead of silently dropping
	// whole blocks.
	bool HasUnrepresentableContent() const {
		return has_unrepresentable_content;
	}
	void MarkUnrepresentableContent() {
		has_unrepresentable_content = true;
	}

private:
	bool dirty = false;
	bool has_unrepresentable_content = false;
};

} // namespace duckdb

#endif
