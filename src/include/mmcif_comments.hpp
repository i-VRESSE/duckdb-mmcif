// The comments table: mmCIF comment lines retained across a write-back.
//
// The pass-1 parser records every '#' line it walks over together with an
// anchor describing where the comment sits in the data model, so the
// write-back writer can put the comment back in the same place instead of
// dropping it (write mode used to lose every comment line).
//
// Anchors are structural, not line numbers: DML changes how many rows a
// category writes back, so "line 412" is meaningless after a rewrite while
// "before row 3 of category atom_site" still resolves. A comment anchored to
// a row that no longer exists is clamped to the end of that loop.

#pragma once

#include "duckdb/common/string.hpp"
#include "duckdb/common/typedefs.hpp"

#include <cstdint>
#include <vector>

namespace duckdb {

// What a comment line sits in front of.
enum class MmcifCommentAnchor : uint8_t {
	BLOCK_HEADER, // before the data_ line (block preamble)
	CATEGORY,     // before a category's first line (its loop_ keyword or first tag)
	ITEM,         // before the line that writes one specific category.item
	LOOP_ROW,     // inside loop data, in front of a specific row
	TRAILER       // after all content, at end of file
};

struct MmcifComment {
	MmcifCommentAnchor anchor = MmcifCommentAnchor::TRAILER;
	string category; // CATEGORY / ITEM / LOOP_ROW: owning category, as written in the file
	string item;     // ITEM: the item name this comment precedes
	idx_t row = 0;   // LOOP_ROW: emit before this row (clamped to the end of the loop)
	string text;     // the comment line verbatim, without its newline
};

} // namespace duckdb
