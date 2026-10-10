"""Offline checks for complete, reproducible dictionary schemas."""

import gzip
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("generator", ROOT / "scripts/generate_type_index.py")
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)


class DictionarySchemaTests(unittest.TestCase):
    def test_saveframe_names_identify_items_with_looped_names(self):
        text = """data_dictionary
save__atom_site.id
loop_
_item.name
_item.category_id
'_atom_site.id' atom_site
'_related.id' related
_item_type.code code
save_
save__atom_site.Cartn_x
_item.name '_wrong.item'
_item_type.code float
save_
save__atom_site.label_seq_id
_item_type.code code
_pdbx_item_type.code int
save_
save_category
_category.id category
save_
"""
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "dictionary.dic"
            source.write_text(text)
            items, _, _, _ = generator.parse_dict(source)
        self.assertEqual(
            dict(items),
            {
                "_atom_site.id": "VARCHAR",
                "_atom_site.Cartn_x": "DOUBLE",
                "_atom_site.label_seq_id": "BIGINT",
            },
        )

    def test_gzip_is_independent_of_filename_and_timestamp(self):
        with tempfile.TemporaryDirectory() as directory:
            a, b = Path(directory) / "a.gz", Path(directory) / "b.gz"
            generator.write_gz(a, "# header\n", ["_entry.id\tVARCHAR"])
            generator.write_gz(b, "# header\n", ["_entry.id\tVARCHAR"])
            self.assertEqual(a.read_bytes(), b.read_bytes())
            self.assertEqual(gzip.decompress(a.read_bytes()), b"# header\n_entry.id\tVARCHAR\n")

    def test_bundled_schema_contains_previously_omitted_valid_items(self):
        with gzip.open(ROOT / "dict/mmcif_type_index.tsv.gz", "rt") as source:
            items = dict(line.rstrip("\n").split("\t") for line in source if not line.startswith("#"))
        self.assertEqual(items["_atom_site.id"], "VARCHAR")
        self.assertEqual(items["_atom_site.Cartn_x"], "DOUBLE")
        self.assertEqual(items["_atom_site.label_seq_id"], "BIGINT")
        self.assertEqual(items["_entry.id"], "VARCHAR")
        self.assertGreaterEqual(len(items), 6801)
        self.assertEqual(len(items), len({item.lower() for item in items}))


if __name__ == "__main__":
    unittest.main()
