// Surgical mmcif write-back: splice the write store's mutations into the bytes
// of the file it was read from, so the result differs from the original only
// where the transaction actually changed something.
//
// Every byte the transaction did not modify is kept exactly as it was read -
// comments, blank lines, alignment, the quoting of untouched values, and
// content the data model does not carry (extra data blocks, save frames).
//
// The store records where every row and cell came from (MmcifRowSpan /
// MmcifCellSpan), which is what makes this possible:
//
//   UPDATE  replace exactly the bytes of the old value
//   DELETE  cut out the deleted row's whole lines, or only its own values
//           and one separator when other rows share its line
//   INSERT  generate the new row and splice it in after the category's
//           last original row
//
// Values that are generated (inserted rows, or an updated value that needs a
// ';...;' text field) are formatted minimally, and a text field always opens in
// column 1 as the mmCIF spec requires.

#pragma once

#include "duckdb/common/string.hpp"

namespace duckdb {

class MmcifWriteStore;

class MmcifPatch {
public:
	// Rewrite the store's source text with its mutations applied. Throws when a
	// mutation cannot be placed in the source instead of silently dropping it.
	static string Apply(const MmcifWriteStore &store);
	// Throw when `value` of `item` cannot be written to a file, so DML fails at
	// the statement instead of at COMMIT.
	static void CheckValue(const string &value, const string &item);
};

} // namespace duckdb
