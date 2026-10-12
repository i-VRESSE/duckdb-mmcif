"""Offline checks for dictionary parsing, overlap policy and reproducibility."""

import gzip
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import generate_type_index as generator


class DictionaryGenerationTests(unittest.TestCase):
    def parse(self, content):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "test.dic"
            path.write_text(content)
            return generator.parse_dict(path)

    def test_scalar_and_loop_items_links_and_multiline_descriptions(self):
        items, categories, links, version = self.parse("""data_test
_dictionary.version '1.0'
save_table
_category.id table
save_
save__table.id
_item_description.description
;
_item_type.code float
save__fake.item
;
loop_
_item.name
_item.category_id
'_table.id' table
'_child.ref' child
_item_type.code int
_pdbx_item_type.code code
loop_
_item_linked.parent_name
_item_linked.child_name
'_table.id' '_child.ref'
'_table.id' '_child.other'
? '_child.missing'
save_
save__child.amount
_item.name '_child.amount'
_item_type.code 'float'
_item_linked.parent_name '_table.id'
_item_linked.child_name '_child.ref'
save_
loop_
_pdbx_item_linked_group_list.child_name
_pdbx_item_linked_group_list.parent_name
'_child.ref' '_table.id'
""")
        self.assertEqual(version, "1.0")
        self.assertEqual(items, {"_table.id": "VARCHAR", "_child.amount": "DOUBLE"})
        self.assertEqual(categories, {"table": "table"})
        self.assertEqual(links, {("_table.id", "_child.ref"), ("_table.id", "_child.other")})

    def test_ranges_and_lists_preserve_their_text(self):
        for code in ("int-range", "float-range", "int_list"):
            with self.subTest(code=code):
                self.assertEqual(generator.duckdb_type(code), "VARCHAR")
        self.assertEqual(generator.duckdb_type("int"), "BIGINT")
        self.assertEqual(generator.duckdb_type("positive_int"), "BIGINT")
        self.assertEqual(generator.duckdb_type("float"), "DOUBLE")

    def test_incomplete_loop_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "Incomplete loop"):
            self.parse("data_x\nloop_\n_item.name\n_item.category_id\n'_x.id'\n")

    def test_duplicate_category_frames_are_allowed(self):
        _, categories, _, _ = self.parse("data_x\nsave_x\n_category.id x\nsave_\nsave_x\n_category.id x\nsave_\n")
        self.assertEqual(categories, {"x": "x"})

    def source(self, name, dtype):
        return (
            {"filename": name, "documentation_url": "https://example.org/" + name},
            {"_Table.ID": dtype},
            {"table": "Table"},
            {("_TABLE.ID", "_child.ref")},
        )

    def test_duplicates_are_case_insensitive_and_base_owns_documentation(self):
        base, extension = self.source("base.dic", "BIGINT"), self.source("extension.dic", "BIGINT")
        extension[1]["_table.id"] = extension[1].pop("_Table.ID")
        types, docs, links = generator.combine([base, extension], {})
        self.assertEqual(types, {"_Table.ID": "BIGINT"})
        self.assertEqual(docs["_Table.ID"], "https://example.org/base.dic")
        self.assertEqual(docs["Table"], "https://example.org/base.dic")
        self.assertEqual(links, {("_Table.ID", "_child.ref")})

    def test_new_type_conflict_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "Unresolved type conflict"):
            generator.combine(
                [
                    self.source("base.dic", "BIGINT"),
                    self.source("extension.dic", "DOUBLE"),
                ],
                {},
            )

    def test_exact_conflict_resolution_and_stale_resolution(self):
        sources = [
            self.source("base.dic", "BIGINT"),
            self.source("extension.dic", "DOUBLE"),
        ]
        resolutions = {
            "_table.id": {
                "types": {"base.dic": "BIGINT", "extension.dic": "DOUBLE"},
                "use": "base.dic",
            }
        }
        types, docs, _ = generator.combine(sources, resolutions)
        self.assertEqual(types["_Table.ID"], "BIGINT")
        self.assertEqual(docs["_Table.ID"], "https://example.org/base.dic")
        with self.assertRaisesRegex(ValueError, "Stale type conflict"):
            generator.combine([sources[0]], resolutions)
        resolutions["_table.id"]["use"] = "unknown.dic"
        with self.assertRaisesRegex(ValueError, "Invalid resolution source"):
            generator.combine(sources, resolutions)
        resolutions["_table.id"]["use"] = "base.dic"
        resolutions["_table.id"]["types"]["extension.dic"] = "VARCHAR"
        with self.assertRaisesRegex(ValueError, "Unresolved type conflict"):
            generator.combine(sources, resolutions)

    def test_source_verification_and_reproducible_artifacts(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "base.dic"
            source.write_text("data_base\n_dictionary.version 1\nsave__a.id\n_item_type.code int\nsave_\n")
            manifest = {
                "sources": [
                    {
                        "filename": source.name,
                        "version": "1",
                        "url": "https://example.org/base.dic",
                        "documentation_url": "https://example.org/base.dic",
                        "sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                    }
                ],
                "type_conflicts": {},
            }
            path = root / "sources.json"
            path.write_text(json.dumps(manifest))
            first, second = root / "first", root / "second"
            generator.generate(root, first, path)
            generator.generate(root, second, path)
            for artifact in first.iterdir():
                self.assertEqual(artifact.read_bytes(), (second / artifact.name).read_bytes())
            self.assertIn(
                "_a.id\tBIGINT",
                gzip.decompress((first / "mmcif_type_index.tsv.gz").read_bytes()).decode(),
            )
            manifest["sources"][0]["version"] = "2"
            path.write_text(json.dumps(manifest))
            with self.assertRaisesRegex(ValueError, "Version mismatch"):
                generator.generate(root, root / "invalid", path)
            source.write_text(source.read_text() + "# changed\n")
            with self.assertRaisesRegex(ValueError, "Checksum mismatch"):
                generator.generate(root, root / "invalid", path)
            self.assertFalse((root / "invalid").exists())


if __name__ == "__main__":
    unittest.main()
