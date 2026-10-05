// No-deps CIF writer (replaces RCSB CifFile::Write). Emulates the RCSB
// non-smart-print path exactly: 80-column wrap, '?' for null cells, single/double
// quote selection, and ';...;' text-quotes for multi-line / long values. The
// write store stores cells in the RCSB parser's forms (".", "?", unquoted), so
// emit is byte-compatible with the old write-back.

#include "mmcif_writer.hpp"

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/typedefs.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace duckdb {

static const unsigned int MM_CIF_LINE_LENGTH = 80;
static const unsigned int MM_STD_PRINT_SPACING = 3;

enum MmcifIdentType { MM_NONE = 0, MM_LEFT, MM_RIGHT };

static bool MmcifIsSpecialFirstChar(char c) {
	switch (c) {
	case '$':
	case '#':
	case '_':
	case ';':
	case '(':
	case ')':
	case '[':
	case ']':
	case '{':
	case '}':
		return true;
	default:
		return false;
	}
}

static bool MmcifIsSpecialChar(char c) {
	switch (c) {
	case '(':
	case ')':
	case '[':
	case ']':
	case '{':
	case '}':
		return true;
	default:
		return false;
	}
}

// Case-insensitive prefix compare (replaces String::IsCiEqual on the keywords).
static bool MmcifIsCiPrefix(const string &s, const char *word) {
	size_t n = strlen(word);
	if (s.size() < n) {
		return false;
	}
	for (size_t i = 0; i < n; i++) {
		if (tolower(static_cast<unsigned char>(s[i])) != tolower(static_cast<unsigned char>(word[i]))) {
			return false;
		}
	}
	return true;
}

static bool MmcifIsQuotableText(const string &itemValue) {
	if (itemValue.empty()) {
		return false;
	}
	if (itemValue[0] == '_') {
		return true;
	}
	for (idx_t i = 0; i < itemValue.size(); i++) {
		char c = itemValue[i];
		if (c == ' ' || c == '\t' || c == '\n' || c == '\'' || c == '\"') {
			return true;
		}
		if (i == 0) {
			if (MmcifIsSpecialFirstChar(c)) {
				return true;
			}
		} else {
			if (MmcifIsSpecialChar(c)) {
				return true;
			}
		}
	}
	if (MmcifIsCiPrefix(itemValue, "data_") || MmcifIsCiPrefix(itemValue, "loop_") ||
	    MmcifIsCiPrefix(itemValue, "save_") || MmcifIsCiPrefix(itemValue, "stop_") ||
	    MmcifIsCiPrefix(itemValue, "global_")) {
		return true;
	}
	return false;
}

// Port of CifFile::_PrintItemValue (non-smart path). Writes one cell value to
// the stream, tracking the current line position; returns nothing (the RCSB
// return value is only used by smart-print header logic).
static void MmcifPrintItemValue(std::ostream &cifo, const string &itemValue, idx_t &linePos, MmcifIdentType identType,
                                unsigned int width, const string &nullValue, const string &quotes,
                                bool noWrap = false) {
	string Ident;
	if (identType == MM_NONE && width != 0) {
		Ident = "          ";
	}
	if (linePos == 0) {
		cifo << Ident;
		linePos = Ident.size();
	}
	if (itemValue.empty()) {
		if (MM_CIF_LINE_LENGTH <= linePos + 2) {
			cifo << "\n";
			linePos = 0;
		}
		idx_t N = nullValue.size();
		if (identType == MM_RIGHT) {
			if (linePos + width - N < MM_CIF_LINE_LENGTH) {
				for (idx_t k = 0; k < width - N; k++) {
					cifo << " ";
				}
				linePos += width - N;
			}
		}
		cifo << nullValue;
		linePos += 1;
		if (identType == MM_LEFT) {
			if (linePos != 0 && linePos + width - N < MM_CIF_LINE_LENGTH) {
				for (idx_t k = 0; k < width - N; k++) {
					cifo << " ";
				}
				linePos += width - N;
			}
		}
		cifo << " ";
		linePos += 1;
		return;
	}

	idx_t str_len = itemValue.size();
	bool multipleLine = false;
	bool multipleWord = false;
	bool embeddedQuotes = false;
	bool embeddedSingleQuotes = false;
	bool embeddedDoubleQuotes = false;
	bool specialChars = false;
	string multipleWordQuotes = quotes;

	for (idx_t i = 0; i < str_len; i++) {
		char c = itemValue[i];
		if (c == ' ' || c == '\t') {
			multipleWord = true;
		} else if (c == '\n') {
			multipleLine = true;
		} else if (c == '\'') {
			embeddedSingleQuotes = true;
			embeddedQuotes = true;
			multipleWordQuotes = "\"";
		} else if (c == '\"') {
			embeddedDoubleQuotes = true;
			embeddedQuotes = true;
			multipleWordQuotes = "\'";
		} else if (!specialChars) {
			if (i == 0 && MmcifIsSpecialFirstChar(c)) {
				specialChars = true;
			}
			if (i != 0 && MmcifIsSpecialChar(c)) {
				specialChars = true;
			}
		}
	}
	if (itemValue[0] == '_' || itemValue[0] == ';') {
		multipleWord = true;
	}
	if (MmcifIsCiPrefix(itemValue, "data_") || MmcifIsCiPrefix(itemValue, "loop_") ||
	    MmcifIsCiPrefix(itemValue, "save_") || MmcifIsCiPrefix(itemValue, "stop_") ||
	    MmcifIsCiPrefix(itemValue, "global_")) {
		multipleWord = true;
	}
	if (embeddedQuotes && multipleWord) {
		multipleLine = true;
	}
	if (embeddedQuotes) {
		multipleWord = true;
	}
	if (specialChars) {
		multipleWord = true;
	}
	if (embeddedSingleQuotes && embeddedDoubleQuotes) {
		multipleWordQuotes = quotes;
	}

	if (str_len >= MM_CIF_LINE_LENGTH || multipleLine) {
		if (linePos != 0 && !noWrap) {
			cifo << "\n";
		}
		cifo << ";" << itemValue << "\n;\n";
		linePos = 0;
	} else {
		if (!noWrap && ((!multipleWord && str_len + 2 + linePos > MM_CIF_LINE_LENGTH) ||
		                (multipleWord && str_len + 4 + linePos > MM_CIF_LINE_LENGTH))) {
			cifo << "\n";
			linePos = 0;
			cifo << Ident;
			linePos += Ident.size();
		}
		string fullItemValue;
		if (multipleWord) {
			fullItemValue = multipleWordQuotes + itemValue + multipleWordQuotes;
		} else {
			fullItemValue = itemValue;
		}
		idx_t N = fullItemValue.size();
		if (identType == MM_RIGHT) {
			if (linePos + width - N < MM_CIF_LINE_LENGTH) {
				for (idx_t k = 0; k < width - N; k++) {
					cifo << " ";
				}
				linePos += width - N;
			}
		}
		cifo << fullItemValue;
		linePos += N;
		if (identType == MM_NONE || identType == MM_LEFT) {
			cifo << " ";
			linePos++;
		}
		if (identType == MM_NONE && !Ident.empty()) {
			if (linePos > Ident.size()) {
				linePos = linePos + width - N;
				for (idx_t i = 0; i < width - N; i++) {
					cifo << " ";
				}
			}
		}
		if (identType == MM_LEFT) {
			if (linePos != 0 && linePos + width - N < MM_CIF_LINE_LENGTH) {
				for (idx_t k = 0; k < width - N; k++) {
					cifo << " ";
				}
				linePos += width - N;
			}
		}
		if (identType == MM_RIGHT) {
			cifo << " ";
			linePos++;
		}
	}
}

// ---------------------------------------------------------------------------
// Comments table lookup (see mmcif_comments.hpp)
//
// Groups the store's retained comment lines by the position they are emitted
// in front of, so writing a category is still one straight pass over its data.
// ---------------------------------------------------------------------------

namespace {

using RowComment = std::pair<idx_t, const MmcifComment *>;

void EmitComments(std::ostream &cifo, const std::vector<const MmcifComment *> &comments) {
	for (auto *comment : comments) {
		cifo << comment->text << "\n";
	}
}

// Cursor over one loop's row-anchored comments. Comments anchored to a row that
// no longer exists (deleted) are flushed by EmitRest, so they trail the loop
// instead of disappearing.
struct MmcifRowCommentCursor {
	const std::vector<RowComment> *entries = nullptr;
	idx_t pos = 0;

	void EmitBefore(std::ostream &cifo, idx_t row) {
		while (entries && pos < entries->size() && (*entries)[pos].first <= row) {
			cifo << (*entries)[pos].second->text << "\n";
			pos++;
		}
	}
	void EmitRest(std::ostream &cifo) {
		while (entries && pos < entries->size()) {
			cifo << (*entries)[pos].second->text << "\n";
			pos++;
		}
	}
};

class MmcifCommentLookup {
public:
	explicit MmcifCommentLookup(const MmcifWriteStore &store) {
		for (auto &comment : store.comments) {
			switch (comment.anchor) {
			case MmcifCommentAnchor::BLOCK_HEADER:
				block_header.push_back(&comment);
				break;
			case MmcifCommentAnchor::TRAILER:
				trailer.push_back(&comment);
				break;
			case MmcifCommentAnchor::CATEGORY:
				by_category[comment.category].push_back(&comment);
				break;
			case MmcifCommentAnchor::ITEM:
				by_item[comment.category + "." + comment.item].push_back(&comment);
				break;
			case MmcifCommentAnchor::LOOP_ROW:
				by_row[comment.category].push_back(std::make_pair(comment.row, &comment));
				break;
			}
		}
		for (auto &entry : by_row) {
			std::stable_sort(entry.second.begin(), entry.second.end(),
			                 [](const RowComment &a, const RowComment &b) { return a.first < b.first; });
		}
	}

	const std::vector<const MmcifComment *> &BlockHeader() const {
		return block_header;
	}
	const std::vector<const MmcifComment *> &Trailer() const {
		return trailer;
	}
	// nullptr when the position carries no retained comments.
	const std::vector<const MmcifComment *> *ForCategory(const string &category) const {
		auto it = by_category.find(category);
		return it == by_category.end() ? nullptr : &it->second;
	}
	const std::vector<const MmcifComment *> *ForItem(const string &category, const string &item) const {
		auto it = by_item.find(category + "." + item);
		return it == by_item.end() ? nullptr : &it->second;
	}
	MmcifRowCommentCursor Rows(const string &category) const {
		MmcifRowCommentCursor cursor;
		auto it = by_row.find(category);
		if (it != by_row.end()) {
			cursor.entries = &it->second;
		}
		return cursor;
	}

private:
	std::vector<const MmcifComment *> block_header;
	std::vector<const MmcifComment *> trailer;
	case_insensitive_map_t<std::vector<const MmcifComment *>> by_category;
	case_insensitive_map_t<std::vector<const MmcifComment *>> by_item;
	case_insensitive_map_t<std::vector<RowComment>> by_row;
};

} // namespace

// Port of CifFile::Write(ostream, tables, writeEmptyTables=false) with
// smartPrint disabled. Emits one data block (the write store keeps only the
// first data block), skipping empty categories. Retained comment lines are
// written back at their anchors; where a category has none of its own, the
// generated "# " separator is used, so a comment-free store emits exactly
// what it used to.
void MmcifWriteCif(std::ostream &cifo, const MmcifWriteStore &store) {
	const string nullValue = "?";
	const string quotes = "\'";
	const MmcifCommentLookup comments(store);

	EmitComments(cifo, comments.BlockHeader());
	cifo << "data_" << store.data_block_name << "\n";
	for (auto &cat : store.categories) {
		idx_t numRow = cat.rows.size();
		idx_t numColumn = cat.columns.size();
		if (numRow == 0) {
			continue; // writeEmptyTables=false; the category's comments go with it
		}
		auto *category_comments = comments.ForCategory(cat.name);
		if (category_comments) {
			EmitComments(cifo, *category_comments);
		} else {
			cifo << "# \n";
		}
		if (numRow <= 1 && !cat.is_loop) {
			// Single-row category: item/value pairs, aligned to the longest item.
			idx_t longestNameIndex = 0;
			idx_t cwid = 0;
			for (idx_t i = 0; i < numColumn; i++) {
				if (cat.columns[i].size() > cwid) {
					cwid = cat.columns[i].size();
					longestNameIndex = i;
				}
			}
			string longestCifItem = "_" + cat.name + "." + cat.columns[longestNameIndex];
			const std::vector<string> &rowValues = cat.rows[0];
			for (idx_t i = 0; i < numColumn; i++) {
				auto *item_comments = comments.ForItem(cat.name, cat.columns[i]);
				if (item_comments) {
					EmitComments(cifo, *item_comments);
				}
				idx_t linePos = 0;
				string cifItem = "_" + cat.name + "." + cat.columns[i];
				cifo << cifItem;
				linePos += cifItem.size();
				idx_t numSpaces = MM_STD_PRINT_SPACING + cat.columns[longestNameIndex].size() - cat.columns[i].size();
				for (idx_t k = 0; k < numSpaces; k++) {
					cifo << " ";
				}
				linePos += numSpaces;
				linePos = longestCifItem.size() + MM_STD_PRINT_SPACING - 1;
				MmcifPrintItemValue(cifo, rowValues[i], linePos, MM_NONE, 0, nullValue, quotes, true);
				if (linePos != 0) {
					cifo << "\n";
				}
			}
		} else {
			// Loop category.
			cifo << "loop_\n";
			for (idx_t i = 0; i < numColumn; i++) {
				auto *item_comments = comments.ForItem(cat.name, cat.columns[i]);
				if (item_comments) {
					EmitComments(cifo, *item_comments);
				}
				idx_t linePos = 0;
				string cifItem = "_" + cat.name + "." + cat.columns[i];
				cifo << cifItem;
				linePos += cifItem.size();
				cifo << " ";
				linePos += 1;
				cifo << "\n";
			}
			vector<idx_t> cwidth(numColumn, 1);
			for (idx_t l = 0; l < numRow; l++) {
				const auto &row = cat.rows[l];
				for (idx_t i = 0; i < numColumn; i++) {
					idx_t ilen = row[i].size();
					if (MmcifIsQuotableText(row[i])) {
						ilen += 2;
					}
					if (ilen > cwidth[i]) {
						cwidth[i] = ilen;
					}
				}
			}
			auto row_comments = comments.Rows(cat.name);
			for (idx_t l = 0; l < numRow; l++) {
				row_comments.EmitBefore(cifo, l);
				const auto &row = cat.rows[l];
				idx_t linePos = 0;
				for (idx_t i = 0; i < numColumn; i++) {
					MmcifPrintItemValue(cifo, row[i], linePos, MM_LEFT, NumericCast<unsigned int>(cwidth[i]), nullValue,
					                    quotes);
				}
				if (linePos != 0) {
					cifo << "\n";
				}
			}
			// Comments anchored to rows that no longer exist trail the loop
			// instead of being dropped with those rows.
			row_comments.EmitRest(cifo);
		}
	}
	if (!comments.Trailer().empty()) {
		EmitComments(cifo, comments.Trailer());
	} else {
		cifo << "# \n";
	}
}

} // namespace duckdb
