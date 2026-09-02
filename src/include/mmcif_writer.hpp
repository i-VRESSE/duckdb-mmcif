// mmcif CIF writer: port of the RCSB CifFile::Write non-smart-print path.
//
// Emulates the RCSB output byte-for-byte: 80-column wrap, '?' for null cells,
// single/double quote selection, and ';...;' text-quotes for multi-line /
// long values. The write store keeps cells in the RCSB parser's stored forms
// (".", "?", unquoted), so emit is byte-compatible with the old write-back.
//
// The ostream is the test seam: unit tests can emit to a string stream and
// assert on the byte format.

#pragma once

#include <iosfwd>

#include "mmcif_write_store.hpp"

namespace duckdb {

// Emits one data block (the write store keeps only the first data block),
// skipping empty categories (writeEmptyTables=false).
void MmcifWriteCif(std::ostream &cifo, const MmcifWriteStore &store);

} // namespace duckdb
