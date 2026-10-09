"""Offline generator regression tests (python3 -m unittest discover -s test/python)."""
import gzip
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("generator", ROOT / "scripts/generate_type_index.py")
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)


class CategoryMetadataTests(unittest.TestCase):
    def parse(self, text):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "dictionary.dic"
            path.write_text(text)
            return generator.parse_categories(path)

    def test_scalar_loop_keys_links_and_unknown_constraints(self):
        metadata = self.parse('''data_dictionary
save_entry
_category.id entry
_category_key.name '_entry.id'
save_
save__entry.id
_item.name '_entry.id'
save_
save_single
_category.id single
_category.description
;A description with 'quotes', a tab\tand a backslash \\.
Second line.
;
_category.mandatory_code yes
_category_key.name '_single.ref'
loop_
_category_group.id
'z_group' 'a_group' 'z_group'
save_
save__single.ref
_item.name '_single.ref'
_item_linked.parent_name '_entry.id'
_item_linked.child_name '_single.ref'
save_
save_multiple
_category.id multiple
_category.mandatory_code no
loop_
_category_key.name
'_multiple.ref' '_multiple.ordinal'
save_
save__multiple.items
loop_
_item.name
'_multiple.ref' '_multiple.ordinal'
loop_
_item_linked.child_name
_item_linked.parent_name
'_multiple.ref' '_entry.id'
save_
save_missing
_category.id missing
_category.description ?
_category.mandatory_code conditional
save_
save_unresolved
_category.id unresolved
_category_key.name '_unresolved.ref'
save_
save__unresolved.ref
_item.name '_unresolved.ref'
_item_linked.child_name '_unresolved.ref'
_item_linked.parent_name '_absent.id'
save_
save_cycle
_category.id cycle
_category_key.name '_cycle.id'
save_
save__cycle.id
_item.name '_cycle.id'
_item_linked.child_name '_cycle.id'
_item_linked.parent_name '_cycle.id'
save_
save_indirect
_category.id indirect
_category_key.name '_indirect.ref'
save_
save__indirect.ref
_item.name '_indirect.ref'
_item_linked.child_name '_indirect.ref'
_item_linked.parent_name '_single.ref'
save_
''')
        for name in ("entry", "single", "indirect"):
            self.assertIs(metadata[name]["is_single_row"], True)
        self.assertIs(metadata["multiple"]["is_single_row"], False)
        self.assertEqual(metadata["multiple"]["keys"], ["_multiple.ordinal", "_multiple.ref"])
        for name in ("missing", "unresolved", "cycle"):
            self.assertIsNone(metadata[name]["is_single_row"])
        self.assertEqual(metadata["single"]["groups"], ["a_group", "z_group"])
        self.assertIs(metadata["single"]["is_mandatory"], True)
        self.assertIs(metadata["multiple"]["is_mandatory"], False)
        self.assertIsNone(metadata["missing"]["is_mandatory"])
        self.assertIsNone(metadata["missing"]["description"])
        self.assertIsNone(metadata["missing"]["groups"])
        rows = generator.category_rows(metadata)
        description = next(row for row in rows if row.startswith("single\tdescription\t"))
        self.assertIn(r"\tand a backslash \\.", description)
        self.assertIn(r"\nSecond line.", description)
        self.assertEqual(len(description.split("\t")), 3)

    def test_shipped_metadata(self):
        with gzip.open(ROOT / "dict/mmcif_pdbx_v50_categories.tsv.gz", "rt") as source:
            rows = [line.rstrip("\n").split("\t") for line in source if not line.startswith("#")]
        flags = {category: value for category, field, value in rows if field == "is_single_row"}
        for category in ("entry", "cell", "struct", "struct_keywords", "symmetry"):
            self.assertEqual(flags[category], "true")
        self.assertEqual(flags["atom_site"], "false")
        groups = [value for category, field, value in rows if category == "cell" and field == "group"]
        self.assertEqual(groups, ["cell_group", "inclusive_group"])


if __name__ == "__main__":
    unittest.main()
