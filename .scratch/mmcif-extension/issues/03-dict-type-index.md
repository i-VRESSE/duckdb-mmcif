# Generate the dictionary type index from mmcif_pdbx_v50.dic

Type: research
Status: claimed
Blocked by:

## Question

How should we build the precomputed dictionary type index? We decided to derive column types from `mmcif_pdbx_v50.dic` (e.g. `_atom_site.Cartn_x` → `float`) as a compact build-time artifact rather than bundling the multi-MB dictionary. Resolve:

- How to source the dictionary (URL/version, licensing, size) and whether to pin a specific version.
- How to extract `category.item → item type` from the dictionary (it is itself mmcif; which categories carry the type, e.g. `_pdbx_item_type`, and how to parse with the RCSB dict parser).
- The generation mechanism (build-time script vs. checked-in generated table) and artifact format shipped with the extension.
- The type-mapping table: dictionary types (`float`, `int`, `char`, `enum`, `line`, `code`, ...) → DuckDB types (DOUBLE, BIGINT, VARCHAR), with `?`/`.` → NULL, unknown → VARCHAR.

## Answer

Resolved. The dictionary is itself mmcif; each item saveframe declares its type via
`_item_type.code` (e.g. `_atom_site.Cartn_x` → `float`, mmcif_pdbx_v50.dic:8998), with a small
`_pdbx_item_type.code` override table (~12 items). The full type vocabulary lives in the dictionary's own
`_item_type_list` category (mmcif_pdbx_v50.dic:3605–3777). Extract with the RCSB parser: C++ CifFile
`GetItemTypeCode` reads the `item_type`/`pdbx_item_type` table (CifFile.C:1441–1476), and py-mmcif
`DictionaryApi.getTypeCodeAlt` prioritizes pdbx → ndb → item_type (DictionaryApi.py:582–588).

- **Source/version/size:** `https://mmcif.wwpdb.org/dictionaries/ascii/mmcif_pdbx_v50.dic` (and `.gz`), pin
  `_dictionary.version 5.416`; 5.9 MB raw / 0.6 MB gz. Published freely by wwPDB; data files CC0 per the
  dictionary's own `_pdbx_data_usage` example (mmcif_pdbx_v50.dic:168096); license component
  `mmcif_pdbx_license.dic` (mmcif_pdbx_v50.dic:3986).
- **Generation/artifact:** checked-in generator script (py-mmcif DictionaryApi or direct parse) runs off-repo
  on the pinned version and emits a compact gzip'd TSV (`_category.item\tDuckDB_type`, ~300 KB raw → ~40 KB gz)
  shipped with the extension; build never downloads/parses the dictionary.
- **Type mapping (measured, 49 codes in use):** float/float-range → DOUBLE; int/positive_int/int_list/int-range
  → BIGINT; all other codes (code/text/line/ucode/uline/atcode/name/boolean/yyyy-mm-dd/... ) → VARCHAR;
  468 items with no `_item_type.code` → VARCHAR (unknown); `?`/`.` cell values → NULL. Totals: 2640 numeric,
  4569 VARCHAR.

Full findings + concrete mapping table: `research/03-dict-type-index.md`
  (`.scratch/mmcif-extension/research/03-dict-type-index.md`).
