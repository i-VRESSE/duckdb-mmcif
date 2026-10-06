// mmcif write model: the no-deps mutable write store.
//
// Evicted from the read-index header (the read path never references it).
// Write mode keeps one persistent MmcifWriteStore per attached catalog instead
// of the RCSB CifFile/ISTable core. Cells are materialized (row-major
// vector<string>) so DML can mutate in place; MmcifPatch splices the changes
// back into the source text. Null cells are stored as "." / "?"; "" maps to
// "?" on write-back.
//
// The read seam is MmcifIndex::Materialize(): the store is materialized from
// the (read-only) index there, so the index header carries no write knowledge.

#ifndef DUCKDB_MMCIF_WRITE_STORE_HPP
#define DUCKDB_MMCIF_WRITE_STORE_HPP

#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/typedefs.hpp"

#include <memory>
#include <string>
#include <vector>

namespace duckdb {

class MmcifIndex;     // read seam: MmcifIndex::Materialize()
struct MmcifCategory; // source record, for the bounds an INSERT needs

// Sentinel for "this row/cell has no bytes in the source file".
static constexpr idx_t MMCIF_NO_SPAN = idx_t(-1);

// Byte span of one cell as it is written in the source file. A NO_SPAN offset
// means the cell has no bytes in the source (it came from an INSERT), so the
// write-back generates it instead of patching it in place.
struct MmcifCellSpan {
	idx_t off;
	idx_t len;
	// DML changed this cell: the write-back replaces its source bytes with the
	// new value and leaves every other byte of the file untouched.
	bool edited;

	MmcifCellSpan() : off(MMCIF_NO_SPAN), len(0), edited(false) {
	}
	MmcifCellSpan(idx_t off_p, idx_t len_p, bool edited_p) : off(off_p), len(len_p), edited(edited_p) {
	}
};

// Byte span of one row as it is written in the source file. A loop row spans
// exactly its own tokens, first value through last (quotes and text fields
// included), because loop rows are a token stream and several may share one
// line; the write-back widens a deleted run of rows to whole lines or a
// separator. A key-value row spans the whole lines its values are written on.
struct MmcifRowSpan {
	idx_t start;
	idx_t end;

	MmcifRowSpan() : start(MMCIF_NO_SPAN), end(0) {
	}
	MmcifRowSpan(idx_t start_p, idx_t end_p) : start(start_p), end(end_p) {
	}
};

struct MmcifWriteCategory {
	string name;
	bool is_loop = false;
	std::vector<string> columns;           // item names, in first-seen order
	std::vector<std::vector<string>> rows; // row-major cell strings, one per column
	// Parallel to rows: where each row and each cell came from in the source.
	std::vector<MmcifRowSpan> row_spans;
	std::vector<std::vector<MmcifCellSpan>> cell_spans;
	// Rows removed by DML; their source bytes are cut out on write-back.
	std::vector<MmcifRowSpan> deleted_rows;
};

class MmcifWriteStore {
public:
	// The source file this store was materialized from. The surgical write-back
	// (MmcifPatch) splices the mutations into its text, so every byte the
	// transaction did not touch survives exactly as it was read.
	explicit MmcifWriteStore(shared_ptr<MmcifIndex> source_index);

	string data_block_name;
	std::vector<MmcifWriteCategory> categories;

	MmcifWriteCategory *FindCategory(const string &name);
	std::vector<string> GetCategoryNames() const;
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

	// The source text, excluding the parser's synthetic flush block.
	const char *SourceData() const;
	idx_t SourceSize() const;
	// The source category record, for the data-region bounds an INSERT needs.
	MmcifCategory *SourceCategory(const string &name) const;

private:
	shared_ptr<MmcifIndex> source_index;
	bool dirty = false;
};

} // namespace duckdb

#endif
