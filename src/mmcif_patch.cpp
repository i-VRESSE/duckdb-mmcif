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
	    : out(), col(start_col), first(true), after_block(false), eol(std::move(eol_p)) {
	}

	void Token(const string &token) {
		// Nothing follows a closing ';' on its line: wwPDB files never do it and
		// some readers (e.g. pdbe-mmcif-validator) drop such trailing values.
		if (after_block) {
			Newline();
		}
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
		// End on the closing ';' without consuming the line: the source's own
		// newline (or the row's terminating one) finishes it.
		// The newline before the closing delimiter is not part of the value.
		out += ";" + value + eol;
		out += ";";
		col = 1;
		first = false;
		after_block = true;
	}

	void Newline() {
		out += eol;
		col = 0;
		first = true;
		after_block = false;
	}

	bool AtLineStart() const {
		return col == 0;
	}

	bool AfterTextBlock() const {
		return after_block;
	}

	const string &Str() const {
		return out;
	}

	const string &Eol() const {
		return eol;
	}

private:
	string out;
	idx_t col;
	bool first;
	bool after_block;
	string eol;
};

// Whether the source line continues with more content after `pos`.
static bool MmcifPatchLineContinues(const char *src, idx_t size, idx_t pos) {
	for (idx_t p = pos; p < size && src[p] != '\n'; p++) {
		if (src[p] != ' ' && src[p] != '\t' && src[p] != '\r') {
			return true;
		}
	}
	return false;
}

// Line terminator of the line just before `pos` (a line start), so a patch
// keeps the file's CRLF/LF convention.
static string MmcifPatchEolBefore(const char *src, idx_t pos) {
	if (pos >= 2 && src[pos - 1] == '\n' && src[pos - 2] == '\r') {
		return "\r\n";
	}
	return "\n";
}

// Line terminator of the physical line containing `pos`.
static string MmcifPatchEolAt(const char *src, idx_t size, idx_t pos) {
	return MmcifPatchEolBefore(src, MmcifLineEnd(src, size, pos));
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
	// Embedded quotes are legal in bare CIF tokens, but wwPDB files always quote
	// them and some readers (e.g. pdbe-mmcif-validator) misparse them otherwise.
	if (MmcifPatchSpecialFirstChar(value[0]) || MmcifPatchReservedWord(value) || has_single || has_double) {
		multiple_word = true;
	}
	if (has_single && has_double) {
		multiple_line = true; // neither quote style can hold it
	}
	if (multiple_line) {
		// A text field cannot contain a line that starts with ';'.
		if (value[0] == ';' || value.find("\n;") != string::npos) {
			throw IOException("mmcif: cannot write value of '%s' - a text field "
			                  "cannot contain a line starting with ';'",
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

// Where a category's new rows go: at the start of the line after its last
// original row (deleted rows count, so re-inserting after a delete lands in the
// same place), falling back to the category's source data region.
idx_t InsertionPoint(const MmcifWriteCategory &cat, MmcifCategory &source, const char *src, idx_t size) {
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
	// A loop row span ends at its last value, possibly mid-line: finish the line.
	while (at < size && src[at - 1] != '\n') {
		at++;
	}
	return at;
}

static bool MmcifPatchBlank(char c) {
	return c == ' ' || c == '\t' || c == '\r';
}

// Source bytes to cut for the deleted rows of one category. Adjacent deleted
// rows (only whitespace between them) are cut as one run. A run that leaves
// nothing but whitespace on its lines takes those whole lines; otherwise it
// shares a line with surviving rows and takes just one separator with it, so
// the remaining rows keep their layout and no blank line is left behind.
void AddDeletionEdits(const MmcifWriteCategory &cat, const char *src, idx_t size, std::vector<PatchEdit> &edits) {
	std::vector<MmcifRowSpan> spans;
	for (auto &d : cat.deleted_rows) {
		if (d.end > d.start) {
			spans.push_back(d);
		}
	}
	std::sort(spans.begin(), spans.end(),
	          [](const MmcifRowSpan &a, const MmcifRowSpan &b) { return a.start < b.start; });
	std::vector<MmcifRowSpan> runs;
	for (auto &s : spans) {
		if (!runs.empty()) {
			auto &last = runs.back();
			idx_t gap = last.end;
			while (gap < s.start && isspace(static_cast<unsigned char>(src[gap]))) {
				gap++;
			}
			if (gap >= s.start) {
				last.end = MaxValue(last.end, s.end);
				continue;
			}
		}
		runs.push_back(s);
	}

	for (auto &run : runs) {
		idx_t line_start = run.start;
		while (line_start > 0 && MmcifPatchBlank(src[line_start - 1])) {
			line_start--;
		}
		bool starts_line = line_start == 0 || src[line_start - 1] == '\n';
		idx_t after = run.end;
		if (after == 0 || src[after - 1] != '\n') {
			while (after < size && MmcifPatchBlank(src[after])) {
				after++;
			}
		}
		bool ends_line = after >= size || src[after] == '\n' || src[after - 1] == '\n';
		if (starts_line && ends_line) {
			if (after < size && src[after] == '\n') {
				after++;
			} else if (after >= size && line_start > 0 && src[size - 1] != '\n') {
				line_start--; // last line has no newline: take the one before it
			}
			edits.push_back(PatchEdit {line_start, after, string()});
		} else if (starts_line) {
			// Rows follow on the same line: keep the indentation, take the gap.
			edits.push_back(PatchEdit {run.start, after, string()});
		} else {
			// Rows precede on the same line: take the gap in front.
			edits.push_back(PatchEdit {line_start, run.end, string()});
		}
	}
}

bool IsInsertedRow(const MmcifWriteCategory &cat, idx_t row) {
	return row >= cat.row_spans.size() || cat.row_spans[row].start == MMCIF_NO_SPAN;
}

// One generated row of a loop category: the values, whitespace separated.
string FormatLoopRow(const MmcifWriteCategory &cat, const std::vector<string> &row, const string &eol) {
	PatchLine line(0, eol);
	for (idx_t c = 0; c < row.size(); c++) {
		EmitValue(line, row[c], cat.name + "." + cat.columns[c]);
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
		auto &item = cat.columns[c];
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

void MmcifPatch::CheckValue(const string &value, const string &item) {
	PatchLine line;
	EmitValue(line, value, item);
}

string MmcifPatch::Apply(const MmcifWriteStore &store) {
	const char *src = store.SourceData();
	idx_t size = store.SourceSize();

	std::vector<PatchEdit> edits;
	for (auto &cat : store.categories) {
		MmcifCategory *source = store.SourceCategory(cat.name);
		if (!source) {
			throw IOException("mmcif: cannot write back category '%s' - it is not in "
			                  "the source file",
			                  cat.name.c_str());
		}

		// Deleted rows: cut them out of the source.
		AddDeletionEdits(cat, src, size, edits);

		// Multiple rows require a loop, even when the source used item/value pairs.
		bool promote_to_loop = !cat.is_loop && cat.rows.size() > 1;
		bool has_original = !cat.rows.empty() && !IsInsertedRow(cat, 0);
		auto loop_header = [&](const string &eol) {
			string header = "loop_" + eol;
			for (auto &column : cat.columns) {
				header += "_" + cat.name + "." + column + eol;
			}
			return header;
		};
		if (promote_to_loop && has_original) {
			auto first_tag = source->singles.front().tag_off;
			edits.push_back(PatchEdit {first_tag, first_tag, loop_header(MmcifPatchEolAt(src, size, first_tag))});
			// Keep the original values and comments; only the item tags move into the header.
			for (auto &cell : source->singles) {
				idx_t tag_end = cell.tag_end;
				if (cell.len != 0 && cell.off >= MmcifLineEnd(src, size, cell.tag_off)) {
					// A tag on its own line must not become a blank line inside the loop.
					while (tag_end < size && MmcifPatchBlank(src[tag_end])) {
						tag_end++;
					}
					if (tag_end < size && src[tag_end] == '\n') {
						tag_end++;
					}
				}
				edits.push_back(PatchEdit {cell.tag_off, tag_end, string()});
				if (cell.len == 0 && !cat.cell_spans[0][cell.col].edited) {
					edits.push_back(PatchEdit {cell.off, cell.off, " ?"});
				}
			}
		}

		// Updated cells: replace exactly the bytes of the old value.
		for (idx_t r = 0; r < cat.rows.size(); r++) {
			if (IsInsertedRow(cat, r)) {
				continue; // generated below, from the current values
			}
			D_ASSERT(cat.cell_spans[r].size() == cat.rows[r].size());
			for (idx_t c = 0; c < cat.rows[r].size(); c++) {
				const MmcifCellSpan &span = cat.cell_spans[r][c];
				if (!span.edited) {
					continue;
				}
				if (span.off == MMCIF_NO_SPAN) {
					throw IOException("mmcif: cannot write back %s.%s - the item has no "
					                  "position in the source file",
					                  cat.name.c_str(), cat.columns[c].c_str());
				}
				PatchLine line(span.off - MmcifLineStart(src, span.off), MmcifPatchEolAt(src, size, span.off));
				EmitValue(line, cat.rows[r][c], cat.name + "." + cat.columns[c]);
				string text = line.Str();
				if (span.len == 0 && !line.AfterTextBlock()) {
					text = " " + text; // the item had no value: separate it from its tag
				}
				if (line.AfterTextBlock() && MmcifPatchLineContinues(src, size, span.off + span.len)) {
					text += line.Eol();
				}
				edits.push_back(PatchEdit {span.off, span.off + span.len, text});
			}
		}

		// Inserted rows: generate them and splice them in as one block, using
		// the line ending of the line they are appended after.
		string inserted;
		idx_t insert_at = 0;
		string eol;
		for (idx_t r = 0; r < cat.rows.size(); r++) {
			if (!IsInsertedRow(cat, r)) {
				continue;
			}
			if (eol.empty()) {
				insert_at = InsertionPoint(cat, *source, src, size);
				eol = MmcifPatchEolBefore(src, insert_at);
				if (src[insert_at - 1] != '\n') {
					inserted = eol; // the source's last line has no newline
				}
				if (promote_to_loop && !has_original) {
					inserted += loop_header(eol);
				}
			}
			inserted += (cat.is_loop || promote_to_loop) ? FormatLoopRow(cat, cat.rows[r], eol)
			                                           : FormatItemRow(cat, cat.rows[r], eol);
		}
		if (!inserted.empty()) {
			edits.push_back(PatchEdit {insert_at, insert_at, std::move(inserted)});
		}
	}

	std::stable_sort(edits.begin(), edits.end(),
	                 [](const PatchEdit &a, const PatchEdit &b) { return a.start < b.start; });

	string out;
	out.reserve(size);
	idx_t cursor = 0;
	for (auto &e : edits) {
		if (e.start < cursor || e.end > size) {
			throw InternalException("mmcif: write-back patches overlap or run past the end of the file");
		}
		out.append(src + cursor, e.start - cursor);
		out += e.text;
		cursor = e.end;
	}
	out.append(src + cursor, size - cursor);
	return out;
}

} // namespace duckdb
