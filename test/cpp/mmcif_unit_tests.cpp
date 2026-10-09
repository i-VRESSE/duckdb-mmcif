// C++ unit tests for the mmcif extension, written with the vendored Catch
// header (duckdb/third_party/catch/catch.hpp) exactly like DuckDB's own core
// tests (duckdb/test/common/*.cpp).
//
// These target the code paths the SQL sqllogictest cannot reach: the value
// cursor token grammar, the pass-1 index Build() edge cases (comments, the
// keep-first-data-block rule, multi-line / next-line single-tag values, partial
// trailing rows), the write-store decode + DML helpers, and the byte-level
// output of the MmcifPatch write-back.

#define CATCH_CONFIG_MAIN
#include "catch.hpp"

#include "mmcif_file.hpp"
#include "mmcif_index.hpp"
#include "mmcif_patch.hpp"
#include "mmcif_write_store.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace duckdb;

namespace {

// Column index by name, so UpdateCell call sites stay readable.
idx_t Col(const MmcifWriteCategory &cat, const std::string &name) {
	for (idx_t i = 0; i < cat.columns.size(); i++) {
		if (cat.columns[i] == name) {
			return i;
		}
	}
	FAIL("no column " << name);
	return 0;
}

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

TEST_CASE("MmcifIndex indexes loop + single-tag, decodes values, keeps first "
          "data block",
          "[mmcif][index]") {
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

	REQUIRE(index->GetCategoryNames().size() == 2);

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
	REQUIRE(sfoo->rows.size() == 2);
	auto row0 = sfoo->rows[0];
	REQUIRE(row0[ColIndex(*sfoo, "a")] == "1");
	REQUIRE(row0[ColIndex(*sfoo, "b")] == "1");

	auto *ssingle = store->FindCategory("single");
	REQUIRE(ssingle);
	auto srow = ssingle->rows[0];
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
	REQUIRE(sp->rows.size() == 2);
	auto last = sp->rows[1];
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
	REQUIRE(q1->rows[0][0] == "1");

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
	REQUIRE(q2->rows[0][0] == "2");
}

TEST_CASE("MmcifIndex::FindCategory returns null for a missing category", "[mmcif][index]") {
	TempCif fixture("idx_missing.cif", "data_b\nloop_\n_q.a\n1\n");
	auto index = MmcifIndex::Load(fixture.Str(), nullptr);
	REQUIRE(index);
	REQUIRE(index->FindCategory("nope") == nullptr);
}

// ---------------------------------------------------------------------------
// MmcifWriteStore DML helpers
// ---------------------------------------------------------------------------

TEST_CASE("MmcifWriteStore add / update / delete / find", "[mmcif][store]") {
	TempCif fixture("store_dml.cif", "data_t\nloop_\n_foo.a\n_foo.b\n1 x\n2 y\n3 z\n");
	auto store_ptr = MmcifIndex::Load(fixture.Str(), nullptr)->Materialize();
	auto &store = *store_ptr;

	auto names = store.GetCategoryNames();
	REQUIRE(names.size() == 1);
	REQUIRE(names[0] == "foo");

	// FindCategory is case-insensitive
	REQUIRE(store.FindCategory("FOO") != nullptr);
	REQUIRE(store.FindCategory("missing") == nullptr);

	auto *f = store.FindCategory("foo");
	REQUIRE(f->rows.size() == 3);

	store.AddRow(*f, {"4", "w"});
	REQUIRE(f->rows.size() == 4);

	store.UpdateCell(*f, 2, Col(*f, "b"), "zz");
	REQUIRE(f->rows[2][1] == "zz");

	// rows sorted + de-duplicated, applied once against the original table
	store.DeleteRows(*f, {0, 2});
	REQUIRE(f->rows.size() == 2);
	REQUIRE(f->rows[0][0] == "2");
	REQUIRE(f->rows[1][0] == "4");
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

TEST_CASE("MmcifFile::Read throws for a missing local file", "[mmcif][file]") {
	REQUIRE_THROWS_AS(MmcifFile::Read("/nonexistent/path/mmcif_does_not_exist.cif", nullptr), IOException);
}

// ---------------------------------------------------------------------------
// MmcifPatch: surgical write-back. The point of these tests is that the patched
// text equals the original everywhere except where the transaction changed
// data, so they assert whole-file equality rather than substrings.
// ---------------------------------------------------------------------------

namespace {

const char *PATCH_SRC = "# top comment\n"
                        "data_test\n"
                        "#\n"
                        "loop_\n"
                        "_atom.id\n"
                        "_atom.name\n"
                        "1 alpha\n"
                        "2 beta\n"
                        "# trailing comment\n";

shared_ptr<MmcifWriteStore> PatchFixture(const TempCif &cif) {
	auto index = MmcifIndex::Load(cif.Str(), nullptr);
	auto store = index->Materialize();
	return store;
}

} // namespace

TEST_CASE("MmcifPatch changes only the bytes of the updated value", "[mmcif][patch]") {
	TempCif cif("mmcif_patch_update.cif", PATCH_SRC);
	auto store = PatchFixture(cif);
	auto *cat = store->FindCategory("atom");
	REQUIRE(cat != nullptr);

	store->UpdateCell(*cat, 0, Col(*cat, "name"), "ALPHA");
	std::string expected = "# top comment\n"
	                       "data_test\n"
	                       "#\n"
	                       "loop_\n"
	                       "_atom.id\n"
	                       "_atom.name\n"
	                       "1 ALPHA\n"
	                       "2 beta\n"
	                       "# trailing comment\n";
	REQUIRE(MmcifPatch::Apply(*store) == expected);
}

TEST_CASE("MmcifPatch cuts a deleted row out cleanly", "[mmcif][patch]") {
	TempCif cif("mmcif_patch_delete.cif", PATCH_SRC);
	auto store = PatchFixture(cif);
	auto *cat = store->FindCategory("atom");
	REQUIRE(cat != nullptr);

	store->DeleteRows(*cat, {0});
	std::string expected = "# top comment\n"
	                       "data_test\n"
	                       "#\n"
	                       "loop_\n"
	                       "_atom.id\n"
	                       "_atom.name\n"
	                       "2 beta\n"
	                       "# trailing comment\n";
	REQUIRE(MmcifPatch::Apply(*store) == expected);
}

// mmCIF loop rows are a flat token stream, so several rows may share one line.
static const char *PATCH_SHARED_SRC = "data_t\n"
                                      "loop_\n"
                                      "_c.x\n"
                                      "_c.y\n"
                                      "  1 2 3 4\n"
                                      "5 6\n";

static std::string PatchDeleteShared(const std::string &name, const std::vector<idx_t> &rows) {
	TempCif cif(name, PATCH_SHARED_SRC);
	auto store = PatchFixture(cif);
	auto *cat = store->FindCategory("c");
	REQUIRE(cat != nullptr);
	store->DeleteRows(*cat, rows);
	return MmcifPatch::Apply(*store);
}

TEST_CASE("MmcifPatch deletes only its own row from a line shared with other rows", "[mmcif][patch]") {
	const std::string head = "data_t\nloop_\n_c.x\n_c.y\n";
	// First row on the line: the indentation stays, the next row moves up.
	REQUIRE(PatchDeleteShared("mmcif_patch_shared_first.cif", {0}) == head + "  3 4\n5 6\n");
	// Last row on the line: the separator in front of it goes with it.
	REQUIRE(PatchDeleteShared("mmcif_patch_shared_last.cif", {1}) == head + "  1 2\n5 6\n");
	// Every row on the line: the whole line goes, no blank line is left.
	REQUIRE(PatchDeleteShared("mmcif_patch_shared_both.cif", {0, 1}) == head + "5 6\n");
	// A run of deleted rows that starts mid-line and ends on the next line.
	REQUIRE(PatchDeleteShared("mmcif_patch_shared_span.cif", {1, 2}) == head + "  1 2\n");
	REQUIRE(PatchDeleteShared("mmcif_patch_shared_all.cif", {0, 1, 2}) == head);
}

TEST_CASE("MmcifPatch keeps a comment between two deleted rows", "[mmcif][patch]") {
	TempCif cif("mmcif_patch_delete_comment.cif", "data_t\nloop_\n_c.x\nA\n# about B\nB\nC\n");
	auto store = PatchFixture(cif);
	auto *cat = store->FindCategory("c");
	REQUIRE(cat != nullptr);
	store->DeleteRows(*cat, {0, 1});
	REQUIRE(MmcifPatch::Apply(*store) == "data_t\nloop_\n_c.x\n# about B\nC\n");
}

TEST_CASE("MmcifPatch deletes a key-value row with its tags", "[mmcif][patch]") {
	TempCif cif("mmcif_patch_delete_single.cif",
	            "data_t\n#\n_entry.title\n;a long\ntitle\n;\n_entry.id B\n#\n_x.y 1\n");
	auto store = PatchFixture(cif);
	auto *cat = store->FindCategory("entry");
	REQUIRE(cat != nullptr);
	store->DeleteRows(*cat, {0});
	REQUIRE(MmcifPatch::Apply(*store) == "data_t\n#\n#\n_x.y 1\n");
}

TEST_CASE("MmcifPatch inserts after a line holding several rows", "[mmcif][patch]") {
	TempCif cif("mmcif_patch_shared_insert.cif", "data_t\nloop_\n_c.x\n_c.y\n1 2 3 4\n# end\n");
	auto store = PatchFixture(cif);
	auto *cat = store->FindCategory("c");
	REQUIRE(cat != nullptr);
	store->AddRow(*cat, {"5", "6"});
	REQUIRE(MmcifPatch::Apply(*store) == "data_t\nloop_\n_c.x\n_c.y\n1 2 3 4\n5 6\n# end\n");
}

TEST_CASE("MmcifPatch inserts into a loop that ends the file without a newline", "[mmcif][patch]") {
	TempCif cif("mmcif_patch_insert_eof.cif", "data_t\nloop_\n_c.x\n_c.y\n1 2");
	auto store = PatchFixture(cif);
	auto *cat = store->FindCategory("c");
	REQUIRE(cat != nullptr);
	store->AddRow(*cat, {"3", "4"});
	REQUIRE(MmcifPatch::Apply(*store) == "data_t\nloop_\n_c.x\n_c.y\n1 2\n3 4\n");
	store->DeleteRows(*cat, {0});
	REQUIRE(MmcifPatch::Apply(*store) == "data_t\nloop_\n_c.x\n_c.y\n3 4\n");
}

TEST_CASE("MmcifPatch splices an inserted row in after the last original row", "[mmcif][patch]") {
	TempCif cif("mmcif_patch_insert.cif", PATCH_SRC);
	auto store = PatchFixture(cif);
	auto *cat = store->FindCategory("atom");
	REQUIRE(cat != nullptr);

	store->AddRow(*cat, {"3", "gamma"});
	std::string expected = "# top comment\n"
	                       "data_test\n"
	                       "#\n"
	                       "loop_\n"
	                       "_atom.id\n"
	                       "_atom.name\n"
	                       "1 alpha\n"
	                       "2 beta\n"
	                       "3 gamma\n"
	                       "# trailing comment\n";
	REQUIRE(MmcifPatch::Apply(*store) == expected);
}

TEST_CASE("MmcifPatch returns the original bytes for a net-zero edit", "[mmcif][patch]") {
	TempCif cif("mmcif_patch_netzero.cif", PATCH_SRC);
	auto store = PatchFixture(cif);
	auto *cat = store->FindCategory("atom");
	REQUIRE(cat != nullptr);

	store->AddRow(*cat, {"3", "gamma"});
	store->DeleteRows(*cat, {2});
	REQUIRE(MmcifPatch::Apply(*store) == std::string(PATCH_SRC));
}

TEST_CASE("MmcifPatch opens a text field in column 1", "[mmcif][patch]") {
	TempCif cif("mmcif_patch_textfield.cif", PATCH_SRC);
	auto store = PatchFixture(cif);
	auto *cat = store->FindCategory("atom");
	REQUIRE(cat != nullptr);

	// A multi-line value cannot stay on the row's line: the ';...' block has to
	// start at column 1, so the row breaks first.
	store->UpdateCell(*cat, 0, Col(*cat, "name"), "line one\nline two");
	std::string expected = "# top comment\n"
	                       "data_test\n"
	                       "#\n"
	                       "loop_\n"
	                       "_atom.id\n"
	                       "_atom.name\n"
	                       "1 \n"
	                       ";line one\n"
	                       "line two\n"
	                       ";\n"
	                       "2 beta\n"
	                       "# trailing comment\n";
	REQUIRE(MmcifPatch::Apply(*store) == expected);
}

TEST_CASE("MmcifPatch ends the line after a text field's closing ';'", "[mmcif][patch]") {
	const std::string head = "data_t\nloop_\n_c.x\n_c.y\n_c.z\n";
	TempCif cif("mmcif_patch_textfield_midrow.cif", head + "1 a 2\n");
	auto store = PatchFixture(cif);
	auto *cat = store->FindCategory("c");
	REQUIRE(cat != nullptr);

	// Updated mid-row: the rest of the source row moves to the next line.
	store->UpdateCell(*cat, 0, Col(*cat, "y"), "both ' and \"");
	// Inserted row: values after the text field start a new line too.
	store->AddRow(*cat, {"3", "x\ny", "4"});
	REQUIRE(MmcifPatch::Apply(*store) == head + "1 \n;both ' and \"\n;\n 2\n3\n;x\ny\n;\n4\n");
}

TEST_CASE("MmcifPatch quotes values with embedded quotes", "[mmcif][patch]") {
	TempCif cif("mmcif_patch_embedded_quotes.cif", PATCH_SRC);
	auto store = PatchFixture(cif);
	auto *cat = store->FindCategory("atom");
	REQUIRE(cat != nullptr);

	store->UpdateCell(*cat, 0, Col(*cat, "name"), "H5'1");
	store->UpdateCell(*cat, 1, Col(*cat, "name"), "a\"b");
	auto out = MmcifPatch::Apply(*store);
	REQUIRE(out.find("1 \"H5'1\"\n") != std::string::npos);
	REQUIRE(out.find("2 'a\"b'\n") != std::string::npos);
}

TEST_CASE("MmcifPatch keeps extra data blocks and save frames verbatim", "[mmcif][patch]") {
	const char *multi = "# preamble\n"
	                    "data_FIRST\n"
	                    "#\n"
	                    "loop_\n"
	                    "_a.id\n"
	                    "_a.v\n"
	                    "1 one\n"
	                    "# between blocks\n"
	                    "data_SECOND\n"
	                    "_a.id 9\n"
	                    "_a.v 'second block value'\n";
	TempCif cif("mmcif_patch_multiblock.cif", multi);
	auto store = PatchFixture(cif);
	// The store does not model the second block, but the patch keeps its bytes.
	auto *cat = store->FindCategory("a");
	REQUIRE(cat != nullptr);

	store->UpdateCell(*cat, 0, Col(*cat, "v"), "ONE");
	std::string expected = "# preamble\n"
	                       "data_FIRST\n"
	                       "#\n"
	                       "loop_\n"
	                       "_a.id\n"
	                       "_a.v\n"
	                       "1 ONE\n"
	                       "# between blocks\n"
	                       "data_SECOND\n"
	                       "_a.id 9\n"
	                       "_a.v 'second block value'\n";
	REQUIRE(MmcifPatch::Apply(*store) == expected);
}

TEST_CASE("MmcifPatch refuses a value it cannot represent", "[mmcif][patch]") {
	TempCif cif("mmcif_patch_unrepresentable.cif", PATCH_SRC);
	auto store = PatchFixture(cif);
	auto *cat = store->FindCategory("atom");
	REQUIRE(cat != nullptr);

	// A text field cannot contain a line starting with ';'.
	store->UpdateCell(*cat, 0, Col(*cat, "name"), "ok\n;not ok");
	REQUIRE_THROWS_AS(MmcifPatch::Apply(*store), IOException);
}

TEST_CASE("MmcifPatch keeps a CRLF file's line ending convention", "[mmcif][patch]") {
	const char *crlf = "# top comment\r\n"
	                   "data_test\r\n"
	                   "loop_\r\n"
	                   "_atom.id\r\n"
	                   "_atom.name\r\n"
	                   "1 alpha\r\n"
	                   "2 beta\r\n";
	TempCif cif("mmcif_patch_crlf.cif", crlf);
	auto store = PatchFixture(cif);
	auto *cat = store->FindCategory("atom");
	REQUIRE(cat != nullptr);

	// A plain update leaves every line ending alone.
	store->UpdateCell(*cat, 0, Col(*cat, "name"), "ALPHA");
	REQUIRE(MmcifPatch::Apply(*store) == std::string("# top comment\r\ndata_test\r\nloop_\r\n_atom.id\r\n_atom.name\r\n"
	                                                 "1 ALPHA\r\n"
	                                                 "2 beta\r\n"));

	// A value that needs a text field breaks the line with CRLF, not LF.
	store->UpdateCell(*cat, 1, Col(*cat, "name"), "both 'quotes' and \"quotes\"");
	REQUIRE(MmcifPatch::Apply(*store) == std::string("# top comment\r\ndata_test\r\nloop_\r\n_atom.id\r\n_atom.name\r\n"
	                                                 "1 ALPHA\r\n"
	                                                 "2 \r\n"
	                                                 ";both 'quotes' and \"quotes\"\r\n"
	                                                 ";\r\n"));
}

TEST_CASE("MmcifPatch promotes item/value categories to loops when inserting rows", "[mmcif][patch]") {
	TempCif cif("mmcif_patch_promote.cif",
	            "data_t\n_entry.id original\n\n# keep\n_entry.note\n;long\nnote\n;\n_entry.empty\n");
	auto store = PatchFixture(cif);
	auto *cat = store->FindCategory("entry");
	store->AddRow(*cat, {"added", "new note", "present"});
	store->UpdateCell(*cat, 0, Col(*cat, "id"), "updated");
	TempCif patched("mmcif_patch_promoted.cif", MmcifPatch::Apply(*store));
	auto reloaded = PatchFixture(patched);
	REQUIRE(reloaded->FindCategory("entry")->rows ==
	        std::vector<std::vector<string>> {{"updated", "long\nnote", "?"}, {"added", "new note", "present"}});
	REQUIRE(Contains(MmcifFile::Read(patched.Str(), nullptr), "# keep\n"));
	REQUIRE(Contains(MmcifFile::Read(patched.Str(), nullptr), "\n\n# keep\n"));

	store->DeleteRows(*cat, {0});
	store->AddRow(*cat, {"another", "note", "value"});
	TempCif replaced("mmcif_patch_promoted_replaced.cif", MmcifPatch::Apply(*store));
	REQUIRE(PatchFixture(replaced)->FindCategory("entry")->rows == cat->rows);
}

TEST_CASE("MmcifPatch preserves trailing newlines in text fields", "[mmcif][patch]") {
	for (const auto &eol : {std::string("\n"), std::string("\r\n")}) {
		TempCif cif("mmcif_patch_trailing_newline.cif", "data_t" + eol + "_entry.note original" + eol);
		for (const auto &value : {std::string("hello\n"), std::string("hello\n\n"), std::string("\n")}) {
			auto store = PatchFixture(cif);
			auto *cat = store->FindCategory("entry");
			store->UpdateCell(*cat, 0, 0, value);
			TempCif patched("mmcif_patch_trailing_newline_roundtrip.cif", MmcifPatch::Apply(*store));
			REQUIRE(PatchFixture(patched)->FindCategory("entry")->rows[0][0] == value);
		}
	}
}

TEST_CASE("MmcifPatch inserts into an empty loop after deleting all rows and reloading", "[mmcif][patch]") {
	TempCif cif("mmcif_patch_empty_loop_source.cif", "data_t\nloop_\n_entry.id\noriginal\n");
	auto store = PatchFixture(cif);
	store->DeleteRows(*store->FindCategory("entry"), {0});
	TempCif empty("mmcif_patch_empty_loop.cif", MmcifPatch::Apply(*store));
	auto reloaded = PatchFixture(empty);
	auto *cat = reloaded->FindCategory("entry");
	REQUIRE(cat->rows.empty());
	reloaded->AddRow(*cat, {"new"});
	TempCif filled("mmcif_patch_empty_loop_filled.cif", MmcifPatch::Apply(*reloaded));
	REQUIRE(PatchFixture(filled)->FindCategory("entry")->rows == cat->rows);

	TempCif middle("mmcif_patch_empty_loop_middle.cif", "data_t\nloop_\n_entry.id\n# keep\nloop_\n_other.id\nx\n");
	auto middle_store = PatchFixture(middle);
	REQUIRE(middle_store->FindCategory("entry")->rows.empty());
	middle_store->AddRow(*middle_store->FindCategory("entry"), {"new"});
	TempCif middle_filled("mmcif_patch_empty_loop_middle_filled.cif", MmcifPatch::Apply(*middle_store));
	auto middle_reloaded = PatchFixture(middle_filled);
	REQUIRE(middle_reloaded->FindCategory("entry")->rows[0][0] == "new");
	REQUIRE(middle_reloaded->FindCategory("other")->rows[0][0] == "x");
}

TEST_CASE("MmcifPatch deletes tags separated from their values by comments and blank lines", "[mmcif][patch]") {
	TempCif cif("mmcif_patch_nextline_delete.cif",
	            "data_t\n_entry.id\n# between tag and value\n\noriginal\n_other.id keep\n");
	auto store = PatchFixture(cif);
	store->DeleteRows(*store->FindCategory("entry"), {0});
	auto text = MmcifPatch::Apply(*store);
	REQUIRE(!Contains(text, "_entry.id"));
	REQUIRE(!Contains(text, "original"));
	TempCif patched("mmcif_patch_nextline_deleted.cif", text);
	auto reloaded = PatchFixture(patched);
	REQUIRE(reloaded->FindCategory("entry") == nullptr);
	REQUIRE(reloaded->FindCategory("other")->rows[0][0] == "keep");
}

TEST_CASE("MmcifPatch preserves comments inside deleted loop and key-value rows", "[mmcif][patch]") {
	for (const auto &eol : {std::string("\n"), std::string("\r\n")}) {
		TempCif loop("mmcif_patch_delete_inner_loop_comment.cif", "data_t" + eol + "loop_" + eol + "_entry.id" + eol +
		                                                              "_entry.note" + eol + "A" + eol + "# inside row" +
		                                                              eol + "B" + eol + "C D" + eol);
		auto loop_store = PatchFixture(loop);
		loop_store->DeleteRows(*loop_store->FindCategory("entry"), {0});
		REQUIRE(MmcifPatch::Apply(*loop_store) == "data_t" + eol + "loop_" + eol + "_entry.id" + eol + "_entry.note" +
		                                              eol + "# inside row" + eol + "C D" + eol);

		TempCif single("mmcif_patch_delete_inner_single_comment.cif",
		               "data_t" + eol + "_entry.id" + eol + "# before value" + eol + "A" + eol + "# between items" +
		                   eol + "_entry.note" + eol + ";body" + eol + "# part of value" + eol + ";" + eol +
		                   "_other.id keep" + eol);
		auto single_store = PatchFixture(single);
		single_store->DeleteRows(*single_store->FindCategory("entry"), {0});
		REQUIRE(MmcifPatch::Apply(*single_store) ==
		        "data_t" + eol + "# before value" + eol + "# between items" + eol + "_other.id keep" + eol);
	}
}

namespace {

shared_ptr<MmcifIndex> LoadBlock(const TempCif &cif, const string &name) {
	return MmcifIndex::Load(cif.Str(), nullptr, &name);
}

} // namespace

TEST_CASE("MmcifIndex selects blocks and patches only the selected block", "[mmcif][index][patch]") {
	std::string first = "# data_comment\r\nDATA_First  # header\r\n_a.v first\r\n";
	std::string second = "data_Second\r\nloop_\r\n_a.v\r\n'data_quoted'\r\n;\r\ndata_text\r\n;\r\n";
	std::string last = "data_empty\r\n";
	TempCif cif("mmcif_selected_block.cif", first + second + last);
	auto index = LoadBlock(cif, "SECOND");
	REQUIRE(index->GetDataBlockNames() == vector<string> {"First", "Second", "empty"});
	REQUIRE(index->GetDataBlockName() == "Second");
	REQUIRE(index->GetRowCount(*index->FindCategory("a")) == 2);
	auto default_index = MmcifIndex::Load(cif.Str(), nullptr);
	REQUIRE(default_index->GetDataBlockName() == "First");
	REQUIRE(default_index != index);
	REQUIRE(LoadBlock(cif, "second") == index);

	auto store = index->Materialize();
	auto cat = store->FindCategory("a");
	store->UpdateCell(*cat, 0, 0, "changed");
	auto patched = MmcifPatch::Apply(*store);
	REQUIRE(patched.substr(0, first.size()) == first);
	REQUIRE(patched.substr(patched.size() - last.size()) == last);
	REQUIRE(patched.find("changed") != std::string::npos);
	REQUIRE_THROWS_WITH(LoadBlock(cif, "missing"), Catch::Contains("not present in file"));
}

TEST_CASE("MmcifIndex supports unnamed blocks and rejects ambiguous selection", "[mmcif][index]") {
	TempCif unnamed("mmcif_unnamed_blocks.cif", "data_\n_a.v unnamed\ndata_named\n_a.v named\n");
	auto index = LoadBlock(unnamed, "");
	REQUIRE(index->GetDataBlockName().empty());
	REQUIRE(index->GetDataBlockNames().size() == 2);
	TempCif duplicate("mmcif_duplicate_blocks.cif", "data_a\n_a.v one\ndata_A\n_a.v two\n");
	REQUIRE(MmcifIndex::Load(duplicate.Str(), nullptr)->GetDataBlockNames().size() == 2);
	REQUIRE_THROWS_WITH(LoadBlock(duplicate, "a"), Catch::Contains("ambiguous"));
}
