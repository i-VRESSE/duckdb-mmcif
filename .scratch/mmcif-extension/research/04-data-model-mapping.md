# Research 04 — Map the RCSB C++ data model to DuckDB tables

Status: resolved (research)
Primary sources:
- `cpp-cif-file` (github.com/rcsb/cpp-cif-file): `include/CifFile.h`, `include/CifDataInfo.h`, `src/CifFile.C`
- `cpp-tables` (github.com/rcsb/cpp-tables): `include/TableFile.h`, `include/ISTable.h`, `include/ITTable.h`, `include/TTable.h`, `src/TableFile.C`
- `cpp-cif-parser` (github.com/rcsb/cpp-cif-parser): `src/CifParser.y`, `src/CifParserBase.C`, `src/CifScanner.l`
- `cpp-common` (github.com/rcsb/cpp-common): `include/CifString.h`, `src/CifString.C`, `include/mapped_ptr_vector.h`
- `py-mmcif` (github.com/rcsb/py-mmcif): `mmcif/io/IoAdapterCore.py`, `mmcif/api/DataCategory.py`, `modules/.../wrapCifFile.cpp`, `wrapISTable.cpp`
- DuckDB submodule in this repo: `third_party/libpg_query/scan.l`, `extension/autocomplete/grammar/keywords/*.list`
- Sample file `1amb_updated.cif` in this repo

Line numbers refer to the raw files fetched to `/tmp/opencode/research/src`.

---

## 1. The library's table/category API

`CifFile` is a `TableFile` subclass (`CifFile.h:47`: `class CifFile : public TableFile`). A file is a list of **data blocks** (`Block`), each block is a list of **tables** (`ISTable`), each table is a 2-D grid of **rows** × **columns**.

The chain of ownership is documented in `TableFile.h`:
- `TableFile` = "ordered container of data blocks" (`TableFile.h:345-365`); blocks stored in `mapped_ptr_vector<Block, StringLess> _blocks` (`TableFile.h:706`).
- `Block` = "a data block, that contains tables" (`TableFile.h:30-39`); tables stored in the public member `mapped_ptr_vector<ISTable, StringLess> _tables` (`TableFile.h:43`).
- `ISTable` = "Public class that represents a two-dimensional table of strings ... identified by its name" (`ISTable.h:34-52`); rows by integer index, columns by (non-empty) column name (`ISTable.h:40-44`).

**Block-level access** (`TableFile.h`):
- `GetNumBlocks()` → number of data blocks (`TableFile.h:518`, inline `:780`).
- `GetBlockNames(vector<string>&)` → all block names (`TableFile.h:533`).
- `GetFirstBlockName()` → name of the first data block (`TableFile.h:548`).
- `GetBlock(blockName)` → `Block&` (`TableFile.h:603`).
- `Block::GetTableNames(vector<string>&)` → all table (category) names in a block (`TableFile.h:218`).
- `Block::GetTable(tableName)` → `ISTable&` (`TableFile.h:250`); `Block::GetTablePtr(tableName)` → pointer (`TableFile.h:266`).
- `Block::IsTablePresent(tableName)` → bool (`TableFile.h:234`).
- Because `Block::_tables` is a public member, tables can also be iterated directly (index + `GetName()`).

**Table-level (ISTable) access** (`ISTable.h`):
- `GetName()` → table name (`ISTable.h:301`, inline `:1453`).
- `GetNumColumns()`, `GetNumRows()` (`ISTable.h:331`, `:626`; inline `:1465`, `:1459`).
- `GetColumnNames()` → `const vector<string>&` of column names in order (`ISTable.h:346`).
- `IsColumnPresent(colName)` → bool (`ISTable.h:362`).
- `GetColumn(col, colName)` → one column's values (`ISTable.h:490`).
- `GetRow(row, rowIndex)` → one row's values in column order (`ISTable.h:782`, const-ref overload `:802`).
- `operator()(rowIndex, colName)` → const ref to a single cell (`ISTable.h:922`).
- `GetColCaseSense()` → case sensitivity of column names (`ISTable.h:1116`; default is case-sensitive, `ISTable.h:147`, `:169`, `:189`, `:212`).

**py-mmcif's reference usage** (`IoAdapterCore.py:211-255`, `__processContent`):
```
blockNames = cifFileObj.GetBlockNames(...)
block = cifFileObj.GetBlock(name)
tableNames = block.GetTableNames(...)
table = block.GetTable(tableName)
attributeNames = table.GetColumnNames()
numRows = table.GetNumRows()
for i in range(numRows):
    row = table.GetRow(i)          # row values in column order
```
Each table becomes a `DataCategory(tableName, attributeNameList, rowList)` — i.e. table name = category name, column names = attribute/item names, rows = rows (`IoAdapterCore.py:248`). The pybind wrappers expose exactly these methods (`wrapCifFile.cpp` / `wrapISTable.cpp`).

**`CifDataInfo`** is a separate, dictionary-side class (`CifDataInfo.h:25`: `class CifDataInfo : public DataInfo`) built from a `DicFile`; it reports dictionary category/item names (`GetCatNames`, `GetItemsNames`, `IsCatDefined`, `IsItemDefined`, `GetCatKeys`, `CifDataInfo.h:31-52`). It is used for the **dictionary** type index (research ticket 03), not for reading data-file rows. `CifFile::GetAttributeValue(attribVal, blockId, category, attribute)` (`CifFile.C:1874`) is a convenience that looks up table `category`, checks column `attribute`, and returns row 0's cell — confirming the category/attribute naming convention (see §2/§5).

## 2. Single-row blocks (`_entry.id`, `_entity.id`) become one-row tables named by category

The parser splits each CIF item name `_category.item`:
- `CifString::GetCategoryFromCifItem` copies chars from index 1 up to the first `.` — i.e. `_entry.id` → category `entry` (leading underscore stripped) (`CifString.C:182-215`; `JOIN_CHAR = '.'` at `CifString.h:34`).
- `CifString::GetItemFromCifItem` copies chars after the first `.` — `_entry.id` → item `id` (`CifString.C:143-179`).

Non-loop **item-value pairs** are handled by `ProcessItemValuePair` (`CifParserBase.C:579-791`):
- Creates `new ISTable(categoryName, ...)` when the category is first seen (`CifParserBase.C:713`).
- Adds a column named `keywordName` (`CifParserBase.C:762`).
- If the table has 0 rows, adds one (`CifParserBase.C:764-765`).
- Writes the value into that one row: `_tbl->UpdateCell(i, keywordName, value)` (`CifParserBase.C:767-786`).

So every `_entry.id 1AMB`, `_entity.id 1`, `_entity.type polymer`, ... becomes a **1-row** table named `entry` / `entity`, each distinct item suffix becoming a column. `_entity.*` (id, type, src_method, pdbx_description, ...) all share category `entity`, so they merge into one `entity` table with many columns and **1 row** (`CifParserBase.C:629-717`). This matches the sample: `_entry.id` → table `entry` (col `id`, value `1AMB`); `_entity.*` → table `entity` (1 row, cols id/type/src_method/...).

## 3. Column values, `?`/`.` → NULL, and loop_ vs non-loop shapes

**Loop tables** (`_atom_site`, `_citation`, ...) are built by:
- `ProcessLoopDeclaration` creates the category table and calls `ProcessItemNameList` (`CifParserBase.C:243-335`).
- `ProcessItemNameList` strips each item name (`GetItemFromCifItem`) and does `_tbl->AddColumn(keywordName)` (`CifParserBase.C:421-443`). Columns are added in loop-header order; `GetColumnNames()` preserves that order.
- `ProcessValueList` starts a new row when `_curValueNo == 0`, fills a row buffer initialized to `UnknownValue` for all columns, then `UpdateCell(_curRow-1, _fieldList[_curValueNo], value)` per value (`CifParserBase.C:471-575`). So a loop becomes an **N-row** table; any cell not given a value stays `UnknownValue` (e.g. `_atom_site.label_alt_id` is `?` in the sample).

**The parser stores the CIF sentinels as literal strings in the cells:**
- `CifString::UnknownValue = "?"` and `CifString::InapplicableValue = "."` (`CifString.C:108-109`; `NULL_CHAR='?'`, `NOT_APPROPRIATE_CHAR='.'` at `CifString.h:36-37`).
- Lexer: `[.]` returns token `UNKNOWN_CIF`; `[?]` returns token `MISSING_CIF` (`CifScanner.l:67-81`).
- Parser: `UNKNOWN_CIF` → `_pBufValue = CifString::InapplicableValue` (`.`); `MISSING_CIF` → `_pBufValue = CifString::UnknownValue` (`?`) (`CifParserBase.C:827-835`). The token names are confusingly swapped but the effect is: cells hold the literal `"."` (inapplicable) and `"?"` (unknown).
- Loop rows are pre-filled with `UnknownValue` (`CifParserBase.C:534`); empty `_pBufValue` also stores `UnknownValue` (`CifParserBase.C:539-551`, `:744-753`, `:767-786`).

**DuckDB mapping:** read each cell via `GetRow`/`operator()`, and convert the sentinels to SQL `NULL`:
- `"?"` (unknown) → NULL
- `"."` (inapplicable) → NULL
- `""` empty string → NULL (py-mmcif treats `None`, `""`, `"."`, `"?"` all as null, `DataCategory.py:145,168`; `CifString::IsEmptyValue`/`IsUnknownValue` at `CifString.C:218-239`).
- Any other literal string → its value (e.g. `1AMB`, `polymer`, `1.000000`).

Helper to detect: `CifString::IsUnknownValue(value)` is true for empty or `"?"` (`CifString.C:232-239`); `CifString::IsEmptyValue(value)` is true for empty, `"."`, or `"?"` (`CifString.C:218-229`). `CifFile::FindCifNullRows` demonstrates scanning cells for null rows (`CifFile.C:1590-1614`).

**loop_ vs non-loop shapes:** there is no structural difference after parsing — both produce `ISTable` rows×columns. The only difference is construction:
- non-loop item-value pairs → exactly 1 row (each `_category.item value` fills a column of the single row),
- loop_ → 1 row per value-group, column count = number of items in the loop header.
A single-row loop_ (e.g. `_atom_type` in the sample has one value per column) still yields a 1-row table. Row/column count is uniform per table (rectangular).

## 4. First-block-only behaviour

- `GetNumBlocks()` counts data blocks (`TableFile.h:780`).
- `GetFirstBlockName()` returns `_blocks[0].GetName()` (`TableFile.C:480-501`).
- `mapped_ptr_vector` "maintains the order of the inserted elements (as vector does)" (`mapped_ptr_vector.h:22-24`), and blocks are added in parse order by `ProcessDataBlockName` → `_fobj->AddBlock(...)` (`CifParserBase.C:837-912`). So `_blocks[0]` is the **first `data_` block in file order**.

Plan for the extension:
- Read tables only from `GetBlock(GetFirstBlockName())`.
- Emit a warning when `GetNumBlocks() > 1` (e.g. DuckDB `Warning`/log), saying only the first data block is exposed.
- (Optional) `GetStatusInd()` reports `eDUPLICATE_BLOCKS`/`eUNNAMED_BLOCKS` flags (`TableFile.h:369-374`, `:503`).

Sample `1amb_updated.cif` has exactly one `data_1AMB` block (grep `^data_` → line 1 only), so no warning fires for it; multi-block files (NMR-STAR, concatenated mmCIF) exercise the warning.

## 5. Table naming edge cases as DuckDB identifiers

**Category → table name:** the parser strips the leading `_` (see §2), so table names never begin with `_`. `_atom_site` → table `atom_site`; `_entry` → `entry`; `_atom_sites` → `atom_sites`. Sample categories: `atom_site`, `atom_sites`, `atom_type`, `citation`, `citation_author`, `entity`, `entity_poly`, `entity_poly_seq`, `entity_src_gen`, `cell`, `symmetry`, `exptl`, `struct`, `software`, `entry` — all valid letter/underscore identifiers.

**Item suffix → column name:** columns are the item suffix after `_category.`, e.g. `_atom_site.Cartn_x` → column `Cartn_x`; `_atom_site.group_PDB` → `group_PDB`; `_atom_site.B_iso_or_equiv` → `B_iso_or_equiv`; `_atom_sites.fract_transf_matrix[1][1]` → column `fract_transf_matrix[1][1]` (brackets preserved — see sample lines 922-933). These are the DuckDB identifiers.

**DuckDB unquoted-identifier rules** (from this repo's libpg_query scanner, `third_party/libpg_query/scan.l`):
- `ident_start  [A-Za-z\200-\377_]` (line 325)
- `ident_cont   [A-Za-z\200-\377_0-9\$]` (line 326)
- `identifier   {ident_start}{ident_cont}*` (line 328)

So unquoted identifiers may contain only letters, digits, `_`, `$`, and must start with a letter or `_`. Characters like `[`, `]`, `.`, `"`, space require **double-quoting** (`scan.l:144` "delimited identifiers (double-quoted identifiers)"; `:275` "Allows embedded spaces and other special characters into identifiers").

**DuckDB keywords:** DuckDB treats most keywords as usable identifiers, but a small set is reserved (`extension/autocomplete/grammar/keywords/reserved_keyword.list`: e.g. `ALL`, `AS`, `CASE`, `CREATE`, `GROUP`, `SELECT`, `TABLE`, `WITH`, ...). Category/column names that collide with reserved words must be quoted.

**Recommended rule for the extension:** always emit identifiers double-quoted, escaping any embedded `"` by doubling it:
- `"atom_site"`, `"entry"`, `"Cartn_x"`, `"fract_transf_matrix[1][1]"`, `"B_iso_or_equiv"`.
This makes every edge case safe regardless of keywords or special characters (`[`, `]`, `$`, spaces). It also avoids case-sensitivity surprises: DuckDB is case-insensitive for unquoted identifiers but preserves case for quoted ones (consistent with the C++ library's default case-sensitive column names, `ISTable.h:147`).

## Summary for the extension (gist)

- Parse the file with `CifParser` into a `CifFile` (`CifParserBase.h`; sample usage `CifReader.C:87-92`).
- Take `CifFile::GetFirstBlockName()`; if `GetNumBlocks() > 1`, warn and ignore the rest.
- Iterate `block.GetTableNames(...)` → for each table name (category), `block.GetTable(name)` → `table.GetColumnNames()`, `table.GetNumRows()`, then `table.GetRow(i)` (values in column order).
- Emit one DuckDB table per category named `"<category>"` (always quoted), columns `"<item>"` (always quoted), rows from the ISTable rows; map `"?"`, `"."`, `""` cell values to SQL `NULL`.
- `CifDataInfo` is for the dictionary type index (ticket 03), not row reading.
