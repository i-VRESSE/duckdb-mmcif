// C++ unit tests for the mmcif extension, written with the vendored Catch
// header (duckdb/third_party/catch/catch.hpp) exactly like DuckDB's own core
// tests (duckdb/test/common/*.cpp).
//
// These target the code paths the SQL sqllogictest cannot reach: the value
// cursor token grammar, the pass-1 index Build() edge cases (comments, the
// keep-first-data-block rule, multi-line / next-line single-tag values, partial
// trailing rows), the write-store decode + DML helpers, and the byte-level
// quoting / text-block / null behaviour of MmcifWriteCif.

#define CATCH_CONFIG_MAIN
#include "catch.hpp"

#include "mmcif_file.hpp"
#include "mmcif_index.hpp"
#include "mmcif_write_store.hpp"
#include "mmcif_writer.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace duckdb;

namespace {

// Writes a fixture file into a dedicated temp directory and removes it on scope
// exit. MmcifIndex::Load(path, nullptr) reads local files via std::ifstream, so
// no ClientContext / attached database is required to exercise the parser.
class TempCif {
public:
	TempCif(const std::string &name, const std::string &content) {
		auto dir = std::filesystem::temp_directory_path() / "duckdb_mmcif_catch_tests";
		std::filesystem::create_directories(dir);
		path = dir / name;
		std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
		ofs << content;
		ofs.close();
	}
	~TempCif() {
		std::error_code ec;
		std::filesystem::remove(path, ec);
	}
	std::string Str() const {
		return path.string();
	}

private:
	std::filesystem::path path;
};

std::string ReadAll(const char *buf, idx_t len) {
	return std::string(buf, len);
}

std::string NextValue(MmcifValueCursor &cursor, bool *is_null) {
	const char *out = nullptr;
	idx_t len = 0;
	bool null = false;
	bool ok = cursor.Next(&out, &len, &null);
	REQUIRE(ok);
	*is_null = null;
	return ReadAll(out, len);
}

std::string WriteToString(const MmcifWriteStore &store) {
	std::ostringstream ss;
	MmcifWriteCif(ss, store);
	return ss.str();
}

bool Contains(const std::string &haystack, const std::string &needle) {
	return haystack.find(needle) != std::string::npos;
}

idx_t ColIndex(const MmcifWriteCategory &cat, const std::string &name) {
	for (idx_t i = 0; i < cat.columns.size(); i++) {
		if (cat.columns[i] == name) {
			return i;
		}
	}
	return idx_t(-1);
}

MmcifWriteStore MakeLoopStore(const std::string &block, const std::string &cat_name,
                              const std::vector<std::string> &cols, std::vector<std::vector<std::string>> rows) {
	MmcifWriteStore store;
	store.data_block_name = block;
	MmcifWriteCategory cat;
	cat.name = cat_name;
	cat.is_loop = true;
	cat.columns = cols;
	cat.rows = std::move(rows);
	store.categories.push_back(std::move(cat));
	return store;
}

} // namespace

// ---------------------------------------------------------------------------
// MmcifValueCursor::Next token grammar (mmcif_index.hpp)
// ---------------------------------------------------------------------------

TEST_CASE("MmcifValueCursor reads nulls and plain tokens", "[mmcif][cursor]") {
	char buf[] = ". ? foo abc";
	MmcifValueCursor cursor(buf, 0, std::strlen(buf));
	bool is_null = false;
	REQUIRE(NextValue(cursor, &is_null) == ".");
	REQUIRE(is_null);
	REQUIRE(NextValue(cursor, &is_null) == "?");
	REQUIRE(is_null);
	REQUIRE(NextValue(cursor, &is_null) == "foo");
	REQUIRE(!is_null);
	REQUIRE(NextValue(cursor, &is_null) == "abc");
	REQUIRE(!is_null);

	const char *out = nullptr;
	idx_t len = 0;
	REQUIRE_FALSE(cursor.Next(&out, &len, &is_null));
}

TEST_CASE("MmcifValueCursor strips single/double quoted values and keeps escapes", "[mmcif][cursor]") {
	char single[] = "'ab''cd' tail";
	MmcifValueCursor c1(single, 0, std::strlen(single));
	bool is_null = false;
	REQUIRE(NextValue(c1, &is_null) == "'ab''cd'");
	REQUIRE(!is_null);
	REQUIRE(NextValue(c1, &is_null) == "tail");

	char dbl[] = "\"a\"\"b\" rest";
	MmcifValueCursor c2(dbl, 0, std::strlen(dbl));
	REQUIRE(NextValue(c2, &is_null) == "\"a\"\"b\"");
	REQUIRE(NextValue(c2, &is_null) == "rest");
}

TEST_CASE("MmcifValueCursor reads triple-quoted values", "[mmcif][cursor]") {
	bool is_null = false;

	char single[] = "'''hello world''' x";
	MmcifValueCursor c1(single, 0, std::strlen(single));
	REQUIRE(NextValue(c1, &is_null) == "'''hello world'''");
	REQUIRE(!is_null);
	REQUIRE(NextValue(c1, &is_null) == "x");

	char dbl[] = "\"\"\"a\"b\"\"\" y";
	MmcifValueCursor c2(dbl, 0, std::strlen(dbl));
	REQUIRE(NextValue(c2, &is_null) == "\"\"\"a\"b\"\"\"");
	REQUIRE(NextValue(c2, &is_null) == "y");
}

TEST_CASE("MmcifValueCursor reads ;...; multi-line values", "[mmcif][cursor]") {
	char buf[] = ";\nline one\nline two\n;\ndone";
	MmcifValueCursor cursor(buf, 0, std::strlen(buf));
	bool is_null = false;
	auto value = NextValue(cursor, &is_null);
	REQUIRE(!is_null);
	REQUIRE(Contains(value, "line one"));
	REQUIRE(Contains(value, "line two"));
	REQUIRE(NextValue(cursor, &is_null) == "done");
}

// ---------------------------------------------------------------------------
// MmcifIndex::Build / Materialize edge cases
// ---------------------------------------------------------------------------

TEST_CASE("MmcifIndex indexes loop + single-tag, decodes values, keeps first data block", "[mmcif][index]") {
	std::string cif = "data_testblock\n"
	                  "loop_\n"
	                  "_foo.a\n"
	                  "_foo.b\n"
	                  "1 1\n"
	                  "2 2\n"
	                  "\n"
	                  "_single.tag1 value1\n"
	                  "_single.tag2\n"
	                  ";a multi\n"
	                  "line value\n"
	                  ";\n"
	                  "_single.tag3\n"
	                  "_single.tag4 'quoted value'\n"
	                  "data_second\n"
	                  "_foo.x ignored\n";
	TempCif fixture("idx_basic.cif", cif);

	auto index = MmcifIndex::Load(fixture.Str(), nullptr);
	REQUIRE(index);
	REQUIRE(index->GetDataBlockName() == "testblock");

	vector<string> names;
	index->GetCategoryNames(names);
	REQUIRE(names.size() == 2);

	auto *foo = index->FindCategory("foo");
	REQUIRE(foo);
	REQUIRE(foo->is_loop);
	REQUIRE(foo->columns.size() == 2);
	REQUIRE(index->GetRowCount(*foo) == 2);
	// keep-first-data-block: the second data block's column must not leak in
	for (auto &col : foo->columns) {
		REQUIRE(col != "x");
	}

	auto *single = index->FindCategory("single");
	REQUIRE(single);
	REQUIRE(!single->is_loop);
	REQUIRE(index->GetRowCount(*single) == 1);

	auto store = index->Materialize();
	REQUIRE(store);
	REQUIRE(store->GetCategoryNames().size() == 2);

	auto *sfoo = store->FindCategory("foo");
	REQUIRE(sfoo);
	REQUIRE(store->GetNumRows(*sfoo) == 2);
	auto row0 = store->GetRow(*sfoo, 0);
	REQUIRE(row0[ColIndex(*sfoo, "a")] == "1");
	REQUIRE(row0[ColIndex(*sfoo, "b")] == "1");

	auto *ssingle = store->FindCategory("single");
	REQUIRE(ssingle);
	auto srow = store->GetRow(*ssingle, 0);
	REQUIRE(srow[ColIndex(*ssingle, "tag1")] == "value1");
	// next-line ;...; value is decoded with the delimiters + trailing ws stripped
	REQUIRE(srow[ColIndex(*ssingle, "tag2")] == "a multi\nline value");
	// empty single-tag value -> NULL -> stored as an empty cell
	REQUIRE(srow[ColIndex(*ssingle, "tag3")] == "");
	// quoted single-tag value -> quotes stripped
	REQUIRE(srow[ColIndex(*ssingle, "tag4")] == "quoted value");
}

TEST_CASE("MmcifIndex keeps loop data that follows a comment line", "[mmcif][index]") {
	std::string cif = "data_b\n"
	                  "loop_\n"
	                  "_c.x\n"
	                  "1\n"
	                  "2\n"
	                  "# comment inside the loop\n"
	                  "3\n";
	TempCif fixture("idx_comment.cif", cif);

	auto index = MmcifIndex::Load(fixture.Str(), nullptr);
	REQUIRE(index);
	auto *c = index->FindCategory("c");
	REQUIRE(c);
	REQUIRE(c->is_loop);
	// A comment sits between values - it does not end the loop, so the row
	// after it is still loop data (it used to be dropped here).
	REQUIRE(index->GetRowCount(*c) == 3);

	// The comment itself is retained, anchored to the row that followed it.
	auto store = index->Materialize();
	REQUIRE(store->comments.size() == 1);
	REQUIRE(store->comments[0].anchor == MmcifCommentAnchor::LOOP_ROW);
	REQUIRE(store->comments[0].category == "c");
	REQUIRE(store->comments[0].row == 2);
	REQUIRE(store->comments[0].text == "# comment inside the loop");
}

TEST_CASE("MmcifIndex anchors every comment to what it sits in front of", "[mmcif][index][comments]") {
	std::string cif = "# preamble one\n"
	                  "# preamble two\n"
	                  "data_b\n"
	                  "# before the category\n"
	                  "_entry.id   B\n"
	                  "# before the second item\n"
	                  "_entry.title   Something\n"
	                  "# before the loop\n"
	                  "loop_\n"
	                  "_c.x\n"
	                  "# inside the loop header\n"
	                  "_c.y\n"
	                  "1 2\n"
	                  "# between rows\n"
	                  "3 4\n"
	                  "# trailing\n";
	TempCif fixture("idx_anchors.cif", cif);

	auto index = MmcifIndex::Load(fixture.Str(), nullptr);
	REQUIRE(index);

	std::vector<std::string> got;
	for (auto &c : index->GetComments()) {
		std::string anchor;
		switch (c.anchor) {
		case MmcifCommentAnchor::BLOCK_HEADER:
			anchor = "BLOCK_HEADER";
			break;
		case MmcifCommentAnchor::CATEGORY:
			anchor = "CATEGORY(" + c.category + ")";
			break;
		case MmcifCommentAnchor::ITEM:
			anchor = "ITEM(" + c.category + "." + c.item + ")";
			break;
		case MmcifCommentAnchor::LOOP_ROW:
			anchor = "LOOP_ROW(" + c.category + "," + std::to_string(c.row) + ")";
			break;
		case MmcifCommentAnchor::TRAILER:
			anchor = "TRAILER";
			break;
		}
		got.push_back(anchor + " " + c.text);
	}
	std::vector<std::string> expected = {
	    "BLOCK_HEADER # preamble one",           "BLOCK_HEADER # preamble two",
	    "CATEGORY(entry) # before the category", "ITEM(entry.title) # before the second item",
	    "CATEGORY(c) # before the loop",         "ITEM(c.y) # inside the loop header",
	    "LOOP_ROW(c,1) # between rows",          "TRAILER # trailing",
	};
	REQUIRE(got == expected);
}

TEST_CASE("MmcifIndex closes loop data before a single-tag category", "[mmcif][index]") {
	// A loop followed straight by a single-tag line of another category (no
	// blank or comment between) used to finalize the loop before its data_end
	// was set, leaving the loop with no data at all.
	std::string cif = "data_b\n"
	                  "loop_\n"
	                  "_a.x\n"
	                  "1\n"
	                  "2\n"
	                  "_b.y 9\n";
	TempCif fixture("idx_loop_then_single.cif", cif);

	auto index = MmcifIndex::Load(fixture.Str(), nullptr);
	REQUIRE(index);
	auto *a = index->FindCategory("a");
	REQUIRE(a);
	REQUIRE(index->GetRowCount(*a) == 2);
	auto *b = index->FindCategory("b");
	REQUIRE(b);
	REQUIRE(!b->is_loop);
	REQUIRE(index->GetRowCount(*b) == 1);
}

TEST_CASE("MmcifIndex counts and materializes a partial trailing row", "[mmcif][index]") {
	std::string cif = "data_b\n"
	                  "loop_\n"
	                  "_p.a\n"
	                  "_p.b\n"
	                  "1 2\n"
	                  "3\n"; // trailing row has only one of two values
	TempCif fixture("idx_partial.cif", cif);

	auto index = MmcifIndex::Load(fixture.Str(), nullptr);
	REQUIRE(index);
	auto *p = index->FindCategory("p");
	REQUIRE(p);
	REQUIRE(index->GetRowCount(*p) == 2);

	auto store = index->Materialize();
	auto *sp = store->FindCategory("p");
	REQUIRE(sp);
	REQUIRE(store->GetNumRows(*sp) == 2);
	auto last = store->GetRow(*sp, 1);
	REQUIRE(last[ColIndex(*sp, "a")] == "3");
	REQUIRE(last[ColIndex(*sp, "b")] == "");
}

TEST_CASE("MmcifIndex::Load detects a same-size rewrite without a context", "[mmcif][index][cache]") {
	// Write-mode attaches load without a ClientContext. A size-only stamp let a
	// cached index survive an external rewrite of identical byte length, so the
	// next write-mode attach materialized stale data.
	TempCif fixture("idx_same_size_rewrite.cif", "data_b\n_q.a 1\n");
	auto first = MmcifIndex::Load(fixture.Str(), nullptr);
	REQUIRE(first);
	REQUIRE(MmcifIndex::Load(fixture.Str(), nullptr) == first); // unchanged file is served from the cache
	auto first_store = first->Materialize();
	auto *q1 = first_store->FindCategory("q");
	REQUIRE(q1);
	REQUIRE(first_store->GetRow(*q1, 0)[0] == "1");

	// Rewrite in place with same length and restore the mtime, simulating a
	// rewrite within the filesystem's mtime granularity.
	auto original_mtime = std::filesystem::last_write_time(fixture.Str());
	{
		std::ofstream ofs(fixture.Str(), std::ios::binary | std::ios::trunc);
		ofs << "data_b\n_q.a 2\n";
	}
	std::filesystem::last_write_time(fixture.Str(), original_mtime);

	auto second = MmcifIndex::Load(fixture.Str(), nullptr);
	REQUIRE(second);
	REQUIRE(second != first);
	auto store = second->Materialize();
	auto *q2 = store->FindCategory("q");
	REQUIRE(q2);
	REQUIRE(store->GetRow(*q2, 0)[0] == "2");
}

TEST_CASE("MmcifIndex::FindCategory returns null for a missing category", "[mmcif][index]") {
	TempCif fixture("idx_missing.cif", "data_b\nloop_\n_q.a\n1\n");
	auto index = MmcifIndex::Load(fixture.Str(), nullptr);
	REQUIRE(index);
	REQUIRE(index->FindCategory("nope") == nullptr);
}

TEST_CASE("MmcifIndex anchors repeated in-loop comments to the right rows", "[mmcif][index][comments]") {
	std::string cif = "data_b\n"
	                  "loop_\n"
	                  "_c.x\n"
	                  "_c.y\n"
	                  "1 2\n"
	                  "# before row 1\n"
	                  "3 4\n"
	                  "5 6\n"
	                  "# before row 3\n"
	                  "# still before row 3\n"
	                  "7 8\n";
	TempCif fixture("idx_multi_row_comments.cif", cif);

	auto index = MmcifIndex::Load(fixture.Str(), nullptr);
	REQUIRE(index);
	auto *c = index->FindCategory("c");
	REQUIRE(c);
	REQUIRE(index->GetRowCount(*c) == 4);

	std::vector<std::string> got;
	for (auto &comment : index->GetComments()) {
		REQUIRE(comment.anchor == MmcifCommentAnchor::LOOP_ROW);
		REQUIRE(comment.category == "c");
		got.push_back(std::to_string(comment.row) + " " + comment.text);
	}
	std::vector<std::string> expected = {"1 # before row 1", "3 # before row 3", "3 # still before row 3"};
	REQUIRE(got == expected);
}

// ---------------------------------------------------------------------------
// MmcifWriteStore DML helpers
// ---------------------------------------------------------------------------

TEST_CASE("MmcifWriteStore add / update / delete / find", "[mmcif][store]") {
	MmcifWriteStore store;
	store.data_block_name = "t";
	MmcifWriteCategory cat;
	cat.name = "foo";
	cat.is_loop = true;
	cat.columns = {"a", "b"};
	cat.rows = {{"1", "x"}, {"2", "y"}, {"3", "z"}};
	store.categories.push_back(cat);

	auto names = store.GetCategoryNames();
	REQUIRE(names.size() == 1);
	REQUIRE(names[0] == "foo");

	// FindCategory is case-insensitive
	REQUIRE(store.FindCategory("FOO") != nullptr);
	REQUIRE(store.FindCategory("missing") == nullptr);

	auto *f = store.FindCategory("foo");
	REQUIRE(store.GetNumRows(*f) == 3);

	store.AddRow(*f, {"4", "w"});
	REQUIRE(store.GetNumRows(*f) == 4);

	store.UpdateCell(*f, 2, "b", "zz");
	REQUIRE(store.GetRow(*f, 2)[1] == "zz");

	// rows sorted + de-duplicated, applied once against the original table
	store.DeleteRows(*f, {0, 2});
	REQUIRE(store.GetNumRows(*f) == 2);
	REQUIRE(store.GetRow(*f, 0)[0] == "2");
	REQUIRE(store.GetRow(*f, 1)[0] == "4");
}

// ---------------------------------------------------------------------------
// MmcifWriteCif byte format
// ---------------------------------------------------------------------------

TEST_CASE("MmcifWriteCif emits header, loop header and aligned rows", "[mmcif][writer]") {
	auto store = MakeLoopStore("block1", "foo", {"a", "b"}, {{"1", "x"}, {"2", "y"}});
	auto out = WriteToString(store);

	REQUIRE(Contains(out, "data_block1\n"));
	REQUIRE(out.rfind("data_block1\n", 0) == 0);
	REQUIRE(Contains(out, "loop_\n"));
	REQUIRE(Contains(out, "_foo.a \n"));
	REQUIRE(Contains(out, "_foo.b \n"));
	REQUIRE(Contains(out, "1 x \n"));
	REQUIRE(Contains(out, "2 y \n"));
	REQUIRE(out.size() >= 3);
	REQUIRE(out.compare(out.size() - 3, 3, "# \n") == 0);
}

TEST_CASE("MmcifWriteCif selects quotes, emits text blocks and nulls", "[mmcif][writer]") {
	std::vector<std::vector<std::string>> rows;
	rows.push_back({"hello world"});          // space -> single-quoted
	rows.push_back({"loop_x"});               // keyword -> single-quoted
	rows.push_back({"_x"});                   // leading underscore -> single-quoted
	rows.push_back({"$id"});                  // leading special char -> single-quoted
	rows.push_back({"a(b)"});                 // embedded special char -> single-quoted
	rows.push_back({"it's"});                 // embedded single quote -> double-quoted
	rows.push_back({"ab\"cd\""});             // embedded double quote -> single-quoted
	rows.push_back({"say 'hi' and \"bye\""}); // both quote kinds + space -> text block
	rows.push_back({"a\nb"});                 // embedded newline -> text block
	rows.push_back({std::string(85, 'z')});   // >= 80 columns -> text block
	rows.push_back({""});                     // empty -> '?' null
	auto store = MakeLoopStore("b", "c", {"v"}, std::move(rows));
	auto out = WriteToString(store);

	REQUIRE(Contains(out, "'hello world'"));
	REQUIRE(Contains(out, "'loop_x'"));
	REQUIRE(Contains(out, "'_x'"));
	REQUIRE(Contains(out, "'$id'"));
	REQUIRE(Contains(out, "'a(b)'"));
	REQUIRE(Contains(out, "\"it's\""));
	REQUIRE(Contains(out, "'ab\"cd\"'"));
	REQUIRE(Contains(out, ";say 'hi' and \"bye\"\n;\n"));
	REQUIRE(Contains(out, ";a\nb\n;\n"));
	REQUIRE(Contains(out, "\n;\n"));
	REQUIRE(Contains(out, std::string(85, 'z')));
	REQUIRE(Contains(out, "?"));
}

TEST_CASE("MmcifWriteCif emits a single-row category as item/value pairs", "[mmcif][writer]") {
	MmcifWriteStore store;
	store.data_block_name = "b";
	MmcifWriteCategory cat;
	cat.name = "c";
	cat.is_loop = false;
	cat.columns = {"a_long_column_name", "s"};
	cat.rows = {{"hello", "1"}};
	store.categories.push_back(cat);

	auto out = WriteToString(store);
	REQUIRE(Contains(out, "data_b\n"));
	REQUIRE(out.find("loop_") == std::string::npos);
	REQUIRE(Contains(out, "_c.a_long_column_name"));
	REQUIRE(Contains(out, "_c.s"));
	REQUIRE(Contains(out, "hello"));
	REQUIRE(Contains(out, "1"));
}

TEST_CASE("MmcifWriteCif skips empty categories", "[mmcif][writer]") {
	MmcifWriteStore store;
	store.data_block_name = "b";
	MmcifWriteCategory cat;
	cat.name = "empty";
	cat.is_loop = true;
	cat.columns = {"v"};
	// no rows
	store.categories.push_back(cat);

	auto out = WriteToString(store);
	REQUIRE(out.find("_empty") == std::string::npos);
	REQUIRE(out.rfind("data_b\n", 0) == 0);
}

TEST_CASE("MmcifWriteCif round-trips through the index", "[mmcif][writer][roundtrip]") {
	auto store = MakeLoopStore("rt", "atom", {"id", "name"}, {{"1", "alpha"}, {"2", "beta two"}});
	auto out = WriteToString(store);

	TempCif fixture("rt_roundtrip.cif", out);
	auto index = MmcifIndex::Load(fixture.Str(), nullptr);
	REQUIRE(index);
	REQUIRE(index->GetDataBlockName() == "rt");
	auto *atom = index->FindCategory("atom");
	REQUIRE(atom);
	REQUIRE(index->GetRowCount(*atom) == 2);

	auto reread = index->Materialize();
	auto *ratom = reread->FindCategory("atom");
	REQUIRE(ratom);
	auto r0 = reread->GetRow(*ratom, 0);
	REQUIRE(r0[ColIndex(*ratom, "id")] == "1");
	REQUIRE(r0[ColIndex(*ratom, "name")] == "alpha");
	auto r1 = reread->GetRow(*ratom, 1);
	REQUIRE(r1[ColIndex(*ratom, "id")] == "2");
	REQUIRE(r1[ColIndex(*ratom, "name")] == "beta two");
}

// ---------------------------------------------------------------------------
// Comments table write-back (write mode used to drop every '#' line)
// ---------------------------------------------------------------------------

namespace {

std::vector<std::string> CommentLines(const std::string &text) {
	std::vector<std::string> lines;
	std::istringstream stream(text);
	std::string line;
	while (std::getline(stream, line)) {
		if (!line.empty() && line[0] == '#') {
			lines.push_back(line);
		}
	}
	return lines;
}

MmcifComment MakeComment(MmcifCommentAnchor anchor, const std::string &category, const std::string &item, idx_t row,
                         const std::string &text) {
	MmcifComment comment;
	comment.anchor = anchor;
	comment.category = category;
	comment.item = item;
	comment.row = row;
	comment.text = text;
	return comment;
}

} // namespace

TEST_CASE("MmcifWriteCif writes retained comments back where they belong", "[mmcif][writer][comments]") {
	std::string cif = "# preamble\n"
	                  "data_b\n"
	                  "# before entry\n"
	                  "_entry.id   B\n"
	                  "# before the second item\n"
	                  "_entry.title   Something\n"
	                  "# before the loop\n"
	                  "loop_\n"
	                  "_c.x\n"
	                  "# inside the loop header\n"
	                  "_c.y\n"
	                  "1 2\n"
	                  "# between rows\n"
	                  "3 4\n"
	                  "# trailing\n";
	TempCif fixture("comments_roundtrip.cif", cif);
	auto store = MmcifIndex::Load(fixture.Str(), nullptr)->Materialize();
	REQUIRE(store->comments.size() == 7);

	auto out = WriteToString(*store);
	// Nothing is lost and the order is kept...
	REQUIRE(CommentLines(out) == CommentLines(cif));
	// ... and each line lands in front of the element it annotates.
	REQUIRE(Contains(out, "# preamble\ndata_b\n"));
	REQUIRE(Contains(out, "# before entry\n_entry.id"));
	REQUIRE(Contains(out, "# before the second item\n_entry.title"));
	REQUIRE(Contains(out, "# before the loop\nloop_\n"));
	REQUIRE(Contains(out, "# inside the loop header\n_c.y"));
	REQUIRE(Contains(out, "# between rows\n3 4 \n"));
	REQUIRE(out.compare(out.size() - 11, 11, "# trailing\n") == 0);
}

TEST_CASE("MmcifWriteCif keeps comments in place across an INSERT", "[mmcif][writer][comments]") {
	std::string cif = "data_b\n"
	                  "# before the loop\n"
	                  "loop_\n"
	                  "_c.x\n"
	                  "1\n"
	                  "# between rows\n"
	                  "2\n";
	TempCif fixture("comments_insert.cif", cif);
	auto store = MmcifIndex::Load(fixture.Str(), nullptr)->Materialize();
	store->AddRow(*store->FindCategory("c"), {"3"});

	auto out = WriteToString(*store);
	// The anchored comments keep their positions; the new row lands after them.
	REQUIRE(Contains(out, "# before the loop\nloop_\n"));
	REQUIRE(Contains(out, "# between rows\n2 \n"));
	REQUIRE(Contains(out, "2 \n3 \n"));
}

TEST_CASE("MmcifWriteCif clamps comments of deleted rows to the end of the loop", "[mmcif][writer][comments]") {
	MmcifWriteStore store;
	store.data_block_name = "b";
	MmcifWriteCategory cat;
	cat.name = "c";
	cat.is_loop = true;
	cat.columns = {"x"};
	cat.rows = {{"1"}};
	store.categories.push_back(cat);
	// Anchored in front of a row that has since been deleted.
	store.comments.push_back(MakeComment(MmcifCommentAnchor::LOOP_ROW, "c", "", 7, "# orphan"));

	auto out = WriteToString(store);
	REQUIRE(Contains(out, "1 \n# orphan\n"));
}

TEST_CASE("MmcifWriteCif drops the comments of a category with no rows left", "[mmcif][writer][comments]") {
	MmcifWriteStore store;
	store.data_block_name = "b";
	MmcifWriteCategory cat;
	cat.name = "gone";
	cat.is_loop = true;
	cat.columns = {"x"};
	store.categories.push_back(cat);
	store.comments.push_back(MakeComment(MmcifCommentAnchor::CATEGORY, "gone", "", 0, "# note about gone"));

	auto out = WriteToString(store);
	// writeEmptyTables=false: the category is not written, so its comments go with it.
	REQUIRE(out.find("gone") == std::string::npos);
}

TEST_CASE("MmcifWriteCif emits the generated separator when nothing was retained", "[mmcif][writer][comments]") {
	auto store = MakeLoopStore("b", "c", {"x"}, {{"1"}});
	REQUIRE(store.comments.empty());
	auto out = WriteToString(store);
	// Comment-free stores write exactly the old "# " separators.
	REQUIRE(Contains(out, "data_b\n# \nloop_\n"));
	REQUIRE(out.compare(out.size() - 3, 3, "# \n") == 0);
}

// ---------------------------------------------------------------------------
// MmcifFile path policy
// ---------------------------------------------------------------------------

TEST_CASE("MmcifFile::IsRemotePath classifies URL schemes", "[mmcif][file]") {
	REQUIRE(MmcifFile::IsRemotePath("https://files.rcsb.org/download/1AMB.cif.gz"));
	REQUIRE(MmcifFile::IsRemotePath("s3://bucket/x.cif"));
	REQUIRE(MmcifFile::IsRemotePath("weirdproto://x/y.cif"));
	REQUIRE(!MmcifFile::IsRemotePath("/local/path/1AMB.cif"));
	REQUIRE(!MmcifFile::IsRemotePath("relative/1AMB.cif"));
}

TEST_CASE("MmcifFile::Read returns empty content for a missing local file", "[mmcif][file]") {
	// With no context MmcifFile::Read uses std::ifstream and returns an empty
	// string for a path that does not exist (no exception on this path).
	auto content = MmcifFile::Read("/nonexistent/path/mmcif_does_not_exist.cif", nullptr);
	REQUIRE(content.empty());
}
