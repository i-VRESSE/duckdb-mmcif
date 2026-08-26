# Research 03 — Dictionary type index for mmcif_pdbx_v50.dic

Resolves ticket `03-dict-type-index.md`. All claims verified against primary sources:
the raw dictionary `mmcif_pdbx_v50.dic` (v5.416), the DDL dictionary `mmcif_ddl.dic`,
the wwPDB Downloads page, and the RCSB mmcif parser sources (`cpp-cif-file`, `py-mmcif`).

## 1. Sourcing the dictionary

- **Raw file (plain text):** `https://mmcif.wwpdb.org/dictionaries/ascii/mmcif_pdbx_v50.dic`
  (source: wwPDB Downloads page, `https://mmcif.wwpdb.org/dictionaries/downloads.html`,
  link text "Dictionary Text", href `/dictionaries/ascii/mmcif_pdbx_v50.dic`).
- **Gzipped:** `https://mmcif.wwpdb.org/dictionaries/ascii/mmcif_pdbx_v50.dic.gz` (same page, "Dictionary Text (gz)").
- **Browser / Items / Categories index pages:** `https://mmcif.wwpdb.org/dictionaries/mmcif_pdbx_v50.dic/Items/`
  and `/Categories/` (each item/category has a per-page HTML view; e.g. `Items/_atom_site.Cartn_x.html`).
  These are browser UI only; the authoritative content is the raw `.dic` file.
- **Version pinning:** the dictionary declares `_dictionary.version 5.416` (mmcif_pdbx_v50.dic:12).
  Pin the download to this exact version (e.g. record the version + date in the artifact).
- **Size (measured):** 5,945,920 bytes uncompressed, 611,329 bytes gzipped (~5.9 MB / 0.6 MB).
  This is the multi-MB file we do **not** bundle; the derived compact index is a few hundred KB.
- **License:** the dictionary is published openly by wwPDB at mmcif.wwpdb.org (no auth/paywall).
  The dictionary's own `_pdbx_data_usage` example states data files are
  "subject to CC0 creative commons license" and links `https://creativecommons.org/share-your-work/public-domain/cc0`
  (mmcif_pdbx_v50.dic:168096–168102). The dictionary distribution lists a dedicated
  `mmcif_pdbx_license.dic` "PDBx/mmCIF Dictionary License Extension" component
  (mmcif_pdbx_v50.dic:3986). The derived type index is a small factual table (item name → type);
  we should record provenance (source URL + version) in the artifact. RCSB *software* (py-mmcif) is
  Apache-2.0 (py-mmcif LICENSE), but that licenses the software, not the dictionary content.

## 2. Where the item type is declared (it is itself mmcif)

The dictionary `data_mmcif_pdbx.dic` is a single mmcif data block; each item is a `save_<item>` saveframe.
Two categories carry the type:

- **`_item_type.code`** — the primary, per-item type declaration. Example:
  `save__atom_site.Cartn_x` sets `_item_type.code  float` (mmcif_pdbx_v50.dic:8971–8999).
  So `_atom_site.Cartn_x → float`. ~6346 of ~6983 items declare `_item_type.code`.
- **`_pdbx_item_type.code`** — a PDBx-specific refinement/override for 12 items
  (mmcif_pdbx_v50.dic:13340,20241,65206,65432,65487,65522,66308,73026,77723,119592,119618,130996,168526).
  Example: `_pdbx_initial_refinement_model.entity_id_list` has `_item_type.code entity_id_list`
  but `_pdbx_item_type.code int_list` (mmcif_pdbx_v50.dic:168524–168526). Prefer the `_pdbx_item_type`
  value when present.

- **Type vocabulary:** the `_item_type_list` category in mmcif_pdbx_v50.dic defines every type code in use
  with its primitive code and regex (mmcif_pdbx_v50.dic:3605–3777): `code, ucode, line, uline, text, int,
  float, name, idname, any, yyyy-mm-dd, yyyy-mm-dd:hh:mm-flex, uchar3, uchar5, uchar1, symop, atcode,
  yyyy-mm-dd:hh:mm, fax, phone, email, int-range, float-range, code30, binary, operation_expression,
  ec-type, seq-one-letter-code, ucode-alphanum-csv, point_symmetry, asym_id, id_list, id_list_spc,
  3x4_matrices, 3x4_matrix, pdbx_related_db_id, pdbx_PDB_obsoleted_db_id, positive_int, emd_id, pdb_id,
  pdb_id_u, point_group, point_group_helical, boolean, author, orcid_id, symmetry_operation, sequence_dep,
  date_dep, citation_doi, exp_data_doi, deposition_email, pdbx_wavelength_list, entity_id_list, int_list,
  uniprot_ptm_id`. This is the authoritative, self-contained vocabulary (the PDBx extended set).
- **Canonical DDL2 types** (`code, char, text, int, name, aliasname, idname, any, yyyy-mm-dd, url`) are defined
  in mmcif_ddl.dic `_item_type_list` (mmcif_ddl.dic:364–380). The PDBx dictionary superset includes these
  plus the extended codes above.

## 3. How to extract `category.item → type` with an mmcif parser

The dictionary is a normal mmcif file, so any mmcif parser reads it. The RCSB parser (which the extension
already wires as submodules, see ticket 01) handles it directly:

- **C++ `cpp-cif-file` CifFile::GetItemTypeCode(typeCode, cifItemName, itemTypeTable)**
  reads the type code from the `item_type` (or `pdbx_item_type`) table of the dictionary block
  (CifFile.h:695; CifFile.C:1441–1476).
- **py-mmcif `DictionaryApi`** is the reference extraction path (py-mmcif `mmcif/api/DictionaryApi.py`):
  - `DATA_TYPE_CODE = ("item_type", "code")` and `DATA_TYPE_CODE_PDBX = ("pdbx_item_type", "code")`
    (DictionaryApi.py:101,134).
  - `getTypeCodeAlt(category, attribute)` prioritizes `pdbx_item_type` → `ndb_item_type` → `item_type`
    (DictionaryApi.py:582–588).
  - `getTypeRegex`/primitive come from `item_type_list` (`construct`, `primitive_code`)
    (DictionaryApi.py:102–103); `__typesDict` is built by reading the `item_type_list` category
    (DictionaryApi.py:1628–1632).

Practical extraction for the type index:
1. Parse `mmcif_pdbx_v50.dic` with the RCSB CifFile parser (or py-mmcif).
2. For each item saveframe, read `_pdbx_item_type.code`; if absent read `_item_type.code`;
   if neither, the item type is **unknown**.
3. Emit `_category.item<TAB>duckdb_type` rows.

## 4. Generation mechanism and artifact format

**Recommendation: a checked-in generator script produces a compact artifact once; the artifact ships with the
extension; the build does not download or parse the multi-MB dictionary.**

- The generator (e.g. Python using py-mmcif `DictionaryApi`, or a small direct parse) runs off-repo / in CI:
  downloads the pinned `mmcif_pdbx_v50.dic.gz` (0.6 MB), extracts `category.item → duckdb_type`, and writes a
  compact artifact. It runs rarely (on version bump), so the result is checked into the repo.
- **Artifact format (suggested):** a gzip'd sorted TSV, one line per item:
  `_atom_site.Cartn_x\tDOUBLE`. ~6300 rows × ~45 B ≈ 300 KB raw → ~40 KB gzip. The extension loads it at
  ATTACH time and looks up each table column. Record header line with provenance: source URL + `_dictionary.version 5.416`.
- This avoids bundling the 5.9 MB dictionary and avoids build-time network dependence (the artifact is already checked in).

## 5. Type-mapping table: dictionary type → DuckDB type

Rule (per ticket): `float`/`real`/`double` → DOUBLE; `int`/`integer` → BIGINT;
`char`/`text`/`enum`/`code`/`line` → VARCHAR; unknown → VARCHAR; `?`/`.` → NULL.

Complete mapping for every `_item_type.code` value actually used in mmcif_pdbx_v50.dic
(counts measured from the raw file; 49 distinct codes):

| dictionary type | count | DuckDB type |
|---|---:|---|
| float | 1820 | DOUBLE |
| float-range | 3 | DOUBLE |
| int | 772 | BIGINT |
| positive_int | 41 | BIGINT |
| int_list | 1 | BIGINT |
| int-range | 3 | BIGINT |
| code | 1454 | VARCHAR |
| text | 785 | VARCHAR |
| line | 673 | VARCHAR |
| ucode | 318 | VARCHAR |
| atcode | 146 | VARCHAR |
| uline | 44 | VARCHAR |
| yyyy-mm-dd | 48 | VARCHAR |
| yyyy-mm-dd:hh:mm | 43 | VARCHAR |
| symop | 43 | VARCHAR |
| uchar1 | 21 | VARCHAR |
| orcid_id | 5 | VARCHAR |
| operation_expression | 4 | VARCHAR |
| boolean | 4 | VARCHAR |
| author | 3 | VARCHAR |
| emd_id | 3 | VARCHAR |
| code30 | 3 | VARCHAR |
| fax | 2 | VARCHAR |
| phone | 2 | VARCHAR |
| email | 2 | VARCHAR |
| pdbx_PDB_obsoleted_db_id | 2 | VARCHAR |
| pdbx_wavelength_list | 2 | VARCHAR |
| sequence_dep | 2 | VARCHAR |
| ucode-alphanum-csv | 2 | VARCHAR |
| exp_data_doi | 2 | VARCHAR |
| asym_id | 2 | VARCHAR |
| uniprot_ptm_id | 2 | VARCHAR |
| uchar5 | 1 | VARCHAR |
| pdbx_related_db_id | 1 | VARCHAR |
| citation_doi | 1 | VARCHAR |
| date_dep | 1 | VARCHAR |
| ec-type | 1 | VARCHAR |
| deposition_email | 1 | VARCHAR |
| 3x4_matrix | 1 | VARCHAR |
| pdb_id_u | 1 | VARCHAR |
| point_group_helical | 1 | VARCHAR |
| point_group | 1 | VARCHAR |
| pdb_id | 1 | VARCHAR |
| 3x4_matrices | 1 | VARCHAR |
| symmetry_operation | 1 | VARCHAR |
| id_list_spc | 1 | VARCHAR |
| name | 1 | VARCHAR |
| binary | 1 | VARCHAR |
| (no `_item_type.code` — unknown) | 468 | VARCHAR |

Totals: **2640 numeric** (1820 float + 772 int + 41 positive_int + 3+3+1 range/list variants),
**4569 VARCHAR** including the 468 untyped items. `?`/`.` cell values → SQL NULL regardless of column type.

Notes on edge cases:
- `int-range`, `int_list`, `float-range` are composite strings; map per ticket (int→BIGINT, float→DOUBLE) —
  but a string like `1-5` or `1,2,3` will not parse as a plain DuckDB numeric on insert; for v1 read-only these
  columns are surfaced with the mapped type and `?`/`.` → NULL is the only value coercion applied. (Only 7 items.)
- `boolean` (`YES|NO`) maps to VARCHAR per ticket; could be BOOLEAN later but not for v1.
- `_pdbx_item_type.code` override (12 items) should win over `_item_type.code` when both present
  (matches `getTypeCodeAlt` priority).

## Sources

- wwPDB Downloads page — https://mmcif.wwpdb.org/dictionaries/downloads.html (download URLs).
- mmcif_pdbx_v50.dic v5.416 — https://mmcif.wwpdb.org/dictionaries/ascii/mmcif_pdbx_v50.dic.gz
  (version :12, item defs :8971–8999, pdbx_item_type rows, item_type_list :3605–3777, license :168096, :3986).
- mmcif_ddl.dic — https://mmcif.wwpdb.org/dictionaries/ascii/mmcif_ddl.dic.gz (item_type_list :364–380).
- RCSB C++ parser — https://github.com/rcsb/cpp-cif-file (CifFile.h:695, CifFile.C:1441–1476).
- py-mmcif DictionaryApi — https://github.com/rcsb/py-mmcif (mmcif/api/DictionaryApi.py:101,134,582–594,1628–1632).
