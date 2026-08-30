# DML operators: INSERT/UPDATE/DELETE (D3, D4)

Type: task
Status: pending
Blocked by: 02

## Question

How do INSERT / UPDATE / DELETE mutate the shared in-memory CifFile?

## Findings

- `MmcifCatalog` currently throws on all four DML/DDL plan hooks
  (`src/mmcif_core.cpp:618-633`): `PlanCreateTableAs`, `PlanInsert`,
  `PlanDelete`, `PlanUpdate`.
- DuckDB's stock `PhysicalInsert`/`PhysicalDelete`/`PhysicalUpdate`
  (`duckdb/src/execution/operator/persistent/`) are tightly coupled to
  `DuckTableEntry`/`DataTable` storage — not usable for a custom catalog
  (`MmcifCatalog::IsDuckCatalog()` false). The mmcif catalog must construct its
  own physical operators (SQLite-extension-style) that read the input chunk and
  apply row-level changes to the shared CifFile.
- Scans emit `row_id = row index` for `COUNT(*)`/virtual-column requests
  (`src/mmcif_core.cpp:259-263`), and are single-threaded (`MaxThreads()==1`).
- RCSB `ISTable` mutation API: `AddRow` / `InsertRow` / `FillRow` /
  `DeleteRow` / `DeleteRows` / `UpdateCell(rowIndex, colName, value)`
  (`modules/cpp-tables/include/ISTable.h:664,713,740,839,858,898`).
- `?`/`.`/`""` map to NULL on read (`MmcifBindData::IsNullCell`,
  `src/mmcif_core.cpp:192`); writes must round-trip NULL back to `?`.

## Decision (agreed)

Row-level DML only (D4); DDL stays read-only. DML operators map DuckDB row_ids
to physical `ISTable` indices and call `AddRow` / `UpdateCell` / `DeleteRow`.
Scans re-snapshot from the live CifFile at `GetScanFunction` time so a later
SELECT in the same transaction sees writes.

## Acceptance

- `INSERT INTO software (name, version) VALUES ('x', '1')` appends a row.
- `UPDATE software SET version='2' WHERE name='x'` calls `UpdateCell`.
- `DELETE FROM software WHERE name='x'` calls `DeleteRow` on the mapped index.
- NULL cells round-trip to `?` in the written file.

## Resolved bug (fixed)

Segfault on DELETE/UPDATE whose WHERE subquery scans the same table (e.g.
`DELETE FROM atom_site WHERE label_comp_id IN (SELECT label_comp_id FROM
atom_site ...)`), and Internal Error on same-table-subquery UPDATE.

Root cause: `MmcifSchemaEntry::GetTableEntry` (`src/mmcif_core.cpp`) always
created a fresh `MmcifTableEntry` and did `tables[entry_name] =
std::move(entry)`, freeing the previous entry. `GetScanFunction` stores a raw
`table_entry` pointer to that object in the scan's `MmcifBindData`. When the
target table and a subquery bind the same table, the second binding freed the
first entry while `BindRowIdColumns(table, get, ...)` still referenced it →
garbage vtable deref (SIGSEGV) / bad column index (Internal Error).

Fix: `GetTableEntry` now reuses an existing cached entry (`tables.find` before
creating), so the `MmcifTableEntry` referenced by scan bind data stays alive.
Verified: same-table-subquery DELETE and UPDATE, cross-table DELETE, simple
DELETE, read-only scans, and all `docs/examples/*.sql` pass. The
`confidence_filter.sql` TEMP-table workaround note was removed and the example
now uses a direct same-table subquery.
