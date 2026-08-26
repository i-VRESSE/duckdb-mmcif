# Map the RCSB C++ data model to DuckDB tables

Type: research
Status: claimed
Blocked by:

## Question

How does the RCSB C++ mmcif library's in-memory model map to DuckDB tables? Resolve:

- The library's table/category API (CifFile, TTable/ITTable/ISTable): how categories, rows, and columns are named and iterated.
- How single-row blocks (e.g. `_entry.id 1AMB`, `_entity.id`) become one-row tables named by category.
- How to read column values and handle `?`/`.` (unknown) as NULL and the `loop_` vs. non-loop shapes.
- First-block-only behaviour: only the first `data_` block is exposed; emit a warning when the file has additional blocks.
- Table naming edge cases: category names starting with `_` (`_atom_site`) and column names as item suffixes (`Cartn_x`) inside DuckDB identifiers.

## Answer

Resolved via research. Full findings: `.scratch/mmcif-extension/research/04-data-model-mapping.md`.

**Gist.** `CifFile` is a `TableFile` (list of `Block`s, each a list of `ISTable`s). Parse with `CifParser`, then:
1. Expose only `GetFirstBlockName()` (the first `data_` block in file order — `mapped_ptr_vector` preserves insertion order); warn when `GetNumBlocks() > 1`.
2. For each `block.GetTableNames()`, `block.GetTable(name)` returns an `ISTable`; read `GetColumnNames()`, `GetNumRows()`, then `GetRow(i)` (values in column order).
3. Table names = categories with the leading `_` stripped (`_entry` → `entry`, `_atom_site` → `atom_site`); columns = item suffixes (`_atom_site.Cartn_x` → `Cartn_x`, `_atom_sites.fract_transf_matrix[1][1]` → `fract_transf_matrix[1][1]`).
4. Non-loop item-value pairs become **1-row** tables (each `_category.item value` fills a column of row 0); `loop_` becomes an N-row table. Both are stored identically as `ISTable` rows×columns.
5. Cells hold the literal sentinels `"?"` (unknown, `CifString::UnknownValue`) and `"."` (inapplicable, `CifString::InapplicableValue`); empty cells also `"?"`. Map `"?"`, `"."`, `""` → SQL `NULL` (py-mmcif does the same: `DataCategory.py:145,168`).
6. Emit identifiers always double-quoted (DuckDB unquoted identifiers allow only `[A-Za-z_]` start + `[A-Za-z0-9_$]` cont; `[`/`]`/`.`/keywords require quoting); escape embedded `"` by doubling.

`CifDataInfo` is the dictionary-side class (for the ticket-03 type index), not row reading.
