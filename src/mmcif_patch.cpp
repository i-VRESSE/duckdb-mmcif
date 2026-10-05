// Surgical mmcif write-back. See mmcif_patch.hpp for the model.

#include "mmcif_patch.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

#include <algorithm>
#include <cctype>
#include <vector>

#include "mmcif_index.hpp"
#include "mmcif_write_store.hpp"

namespace duckdb {
namespace {

struct PatchEdit {
	idx_t start;
	idx_t end;
	string text;
};

// Accumulates spliced text while tracking the current column, so a ';...;' text
// field can always be opened in column 1 the way the mmCIF spec requires.
// start_col is the column the spliced text lands at in the source line; `first`
// tracks whether anything has been emitted yet, because a value spliced over an
// existing value must not add a separator - the whitespace it replaces is
// already outside the patched span.
class PatchLine {
public:
	explicit PatchLine(idx_t start_col = 0, string eol_p = "\n")
	    : out(), col(start_col), first(true), eol(std::move(eol_p)) {
	}

	void Token(const string &token) {
		if (!first) {
			out += " ";
			col += 1;
		}
		out += token;
		col += token.size();
		first = false;
	}

	void TextBlock(const string &value) {
		// A text field has to open in column 1, so break the line first.
		if (col != 0) {
			Newline();
		}
		string body = value;
		if (!body.empty() && body.back() == '\n') {
			body.pop_back(); // the closing ';' line supplies it
		}
		// End on the closing ';' without consuming the line: the source's own
		// newline (or the row's terminating one) finishes it.
		out += ";" + body + eol;
		out += ";";
		col = 1;
		first = false;
	}

	void Newline() {
		out += eol;
		col = 0;
		first = true;
	}

	bool AtLineStart() const {
		return col == 0;
	}

	const string &Str() const {
		return out;
	}

private:
	string out;
	idx_t col;
	bool first;
	string eol;
};

// Column that `pos` sits at in its source line: the column a replacement value
// is spliced in at, which decides whether a text field has to break first.
static idx_t MmcifPatchColumnOf(const char *src, idx_t pos) {
	idx_t line = pos;
	while (line > 0 && src[line - 1] != '\n') {
		line--;
	}
	return pos - line;
}

// Line terminator the source uses on the physical line containing `pos`, so a
// patch that has to break a line keeps the file's CRLF/LF convention.
static string MmcifPatchEolAt(const char *src, idx_t size, idx_t pos) {
	idx_t p = pos;
	while (p < size && src[p] != '\n') {
		p++;
	}
	if (p > 0 && p < size && src[p - 1] == '\r') {
		return "\r\n";
	}
	return "\n";
}

// Line terminator of the line just before an insertion point.
static string MmcifPatchEolBefore(const char *src, idx_t pos) {
	if (pos >= 2 && src[pos - 1] == '\n' && src[pos - 2] == '\r') {
		return "\r\n";
	}
	return "\n";
}

static bool MmcifPatchSpecialFirstChar(char c) {
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

static bool MmcifPatchReservedWord(const string &value) {
	return StringUtil::CIStartsWith(value, "data_") || StringUtil::CIStartsWith(value, "loop_") ||
	       StringUtil::CIStartsWith(value, "save_") || StringUtil::CIStartsWith(value, "stop_") ||
	       StringUtil::CIStartsWith(value, "global_");
}

// Emit one value with the least quoting that keeps it unambiguous: a bare token
// when it can be one, otherwise single/double quotes, falling back to a text
// block when the value cannot survive either quote style.
void EmitValue(PatchLine &line, const string &value, const string &item) {
	if (value.empty()) {
		line.Token("?"); // empty cell -> missing marker
		return;
	}
	bool multiple_word = false;
	bool multiple_line = false;
	bool has_single = false;
	bool has_double = false;
	for (idx_t i = 0; i < value.size(); i++) {
		char c = value[i];
		if (c == ' ' || c == '\t') {
			multiple_word = true;
		} else if (c == '\n') {
			multiple_line = true;
		} else if (c == '\'') {
			has_single = true;
		} else if (c == '"') {
			has_double = true;
		}
	}
	if (MmcifPatchSpecialFirstChar(value[0]) || MmcifPatchReservedWord(value)) {
		multiple_word = true;
	}
	if (has_single && has_double) {
		multiple_line = true; // neither quote style can hold it
	}
	if (multiple_line) {
		// A text field cannot contain a line that starts with ';'.
		if (value[0] == ';' || value.find("\n;") != string::npos) {
			throw IOException(
			    "mmcif: cannot write value of '%s' - a text field cannot contain a line starting with ';'",
			    item.c_str());
		}
		line.TextBlock(value);
		return;
	}
	if (!multiple_word) {
		line.Token(value);
		return;
	}
	string quote = has_single ? "\"" : "'";
	line.Token(quote + value + quote);
}

// Where a category's new rows go: right after its last original row (deleted
// rows count, so re-inserting after a delete lands in the same place), falling
// back to the category's source data region.
idx_t InsertionPoint(const MmcifWriteCategory &cat, MmcifCategory &source) {
	idx_t at = 0;
	for (auto &rs : cat.row_spans) {
		if (rs.start != MMCIF_NO_SPAN && rs.end > at) {
			at = rs.end;
		}
	}
	for (auto &d : cat.deleted_rows) {
		if (d.end > at) {
			at = d.end;
		}
	}
	if (at == 0) {
		at = source.data_end != 0 ? source.data_end : source.data_start;
	}
	if (at == 0) {
		throw IOException("mmcif: cannot find where to insert rows into category '%s'", cat.name.c_str());
	}
	return at;
}

bool IsInsertedRow(const MmcifWriteCategory &cat, idx_t row) {
	return row >= cat.row_spans.size() || cat.row_spans[row].start == MMCIF_NO_SPAN;
}

// One generated row of a loop category: the values, whitespace separated.
string FormatLoopRow(const MmcifWriteCategory &cat, const std::vector<string> &row, const string &eol) {
	PatchLine line(0, eol);
	for (idx_t c = 0; c < row.size(); c++) {
		EmitValue(line, row[c], cat.name + "." + (c < cat.columns.size() ? cat.columns[c] : to_string(c)));
	}
	if (!line.AtLineStart()) {
		line.Newline();
	}
	return line.Str();
}

// One generated row of a key-value category: `_category.item value` per column.
string FormatItemRow(const MmcifWriteCategory &cat, const std::vector<string> &row, const string &eol) {
	string out;
	for (idx_t c = 0; c < row.size(); c++) {
		string item = c < cat.columns.size() ? cat.columns[c] : to_string(c);
		PatchLine line(0, eol);
		line.Token("_" + cat.name + "." + item);
		// Same line, so a value that needs a text field breaks to column 1.
		EmitValue(line, row[c], cat.name + "." + item);
		if (!line.AtLineStart()) {
			line.Newline();
		}
		out += line.Str();
	}
	return out;
}

} // namespace

string MmcifPatch::Apply(const MmcifWriteStore &store) {
	const char *src = store.SourceData();
	idx_t size = store.SourceSize();
	if (!src) {
		throw InternalException("mmcif: write-back has no source text to patch");
	}

	std::vector<PatchEdit> edits;
	for (auto &cat : store.categories) {
		MmcifCategory *source = store.SourceCategory(cat.name);
		if (!source) {
			throw IOException("mmcif: cannot write back category '%s' - it is not in the source file",
			                  cat.name.c_str());
		}

		// Deleted rows: cut their whole lines out of the source.
		for (auto &d : cat.deleted_rows) {
			if (d.end > d.start) {
				edits.push_back(PatchEdit {d.start, d.end, string()});
			}
		}

		// Updated cells: replace exactly the bytes of the old value.
		for (idx_t r = 0; r < cat.rows.size(); r++) {
			if (IsInsertedRow(cat, r)) {
				continue; // generated below, from the current values
			}
			if (r >= cat.cell_spans.size() || cat.cell_spans[r].size() != cat.rows[r].size()) {
				throw InternalException("mmcif: write-back spans are out of sync for category '%s'", cat.name.c_str());
			}
			for (idx_t c = 0; c < cat.rows[r].size(); c++) {
				const MmcifCellSpan &span = cat.cell_spans[r][c];
				if (!span.edited) {
					continue;
				}
				if (span.off == MMCIF_NO_SPAN) {
					throw IOException("mmcif: cannot write back %s.%s - the item has no position in the source file",
					                  cat.name.c_str(), c < cat.columns.size() ? cat.columns[c].c_str() : "?");
				}
				PatchLine line(MmcifPatchColumnOf(src, span.off), MmcifPatchEolAt(src, size, span.off));
				EmitValue(line, cat.rows[r][c],
				          cat.name + "." + (c < cat.columns.size() ? cat.columns[c] : to_string(c)));
				edits.push_back(PatchEdit {span.off, span.off + span.len, line.Str()});
			}
		}

		// Inserted rows: generate them and splice them in as one block, using
		// the line ending of the line they are appended after.
		bool has_inserts = false;
		for (idx_t r = 0; r < cat.rows.size(); r++) {
			if (IsInsertedRow(cat, r)) {
				has_inserts = true;
				break;
			}
		}
		if (has_inserts) {
			idx_t insert_at = InsertionPoint(cat, *source);
			string eol = MmcifPatchEolBefore(src, insert_at);
			string inserted;
			for (idx_t r = 0; r < cat.rows.size(); r++) {
				if (!IsInsertedRow(cat, r)) {
					continue;
				}
				inserted += cat.is_loop ? FormatLoopRow(cat, cat.rows[r], eol) : FormatItemRow(cat, cat.rows[r], eol);
			}
			if (!inserted.empty()) {
				edits.push_back(PatchEdit {insert_at, insert_at, inserted});
			}
		}
	}

	std::stable_sort(edits.begin(), edits.end(),
	                 [](const PatchEdit &a, const PatchEdit &b) { return a.start < b.start; });

	idx_t extra = 0;
	idx_t prev_end = 0;
	for (auto &e : edits) {
		if (e.start < prev_end || e.end > size) {
			throw InternalException("mmcif: write-back patches overlap or run past the end of the file");
		}
		prev_end = e.end;
		extra += e.text.size();
	}

	string out;
	out.reserve(size + extra);
	idx_t cursor = 0;
	for (auto &e : edits) {
		out.append(src + cursor, e.start - cursor);
		out += e.text;
		cursor = e.end;
	}
	out.append(src + cursor, size - cursor);
	return out;
}

} // namespace duckdb
