# mmcif write mode — spec

Extend the mmcif extension so an attached mmcif database can be opened in
**read/write mode** (INSERT / UPDATE / DELETE on category tables, with the
attached `.cif` file updated to carry the new rows). Read-only remains the
default.

## Decisions (grilled)

- **D1 ATTACH option (Q1):** use DuckDB's built-in access-mode keys
  `READ_ONLY TRUE` / `READ_WRITE TRUE` next to `TYPE mmcif`. Read-only is the
  default: `MmcifAttach` inspects the raw `AttachInfo.options` map and only
  enters write mode when an explicit `readwrite`/`read_write` key is present,
  because DuckDB core's default `access_mode` is `READ_WRITE`.
  README uses `READ_ONLY TRUE` on select-only examples and explicit
  `READ_WRITE TRUE` on write examples.
- **D2 Persistent CifFile (Q2):** write mode keeps one persistent, shared
  in-memory `CifFile`, parsed once at ATTACH and owned by the catalog, mutated
  by DML operators and written back on COMMIT. Read-only path stays
  re-parse-on-demand.
- **D3 Row identity (Q3):** `row_id = physical ISTable row index`; DML maps
  DuckDB row_ids to `ISTable` indices and calls `UpdateCell` / `DeleteRow` /
  `AddRow`. Scans re-snapshot from the live CifFile at `GetScanFunction` time.
- **D4 Scope (Q4):** write mode = row-level DML into existing categories only.
  DDL (`CREATE TABLE`/`ALTER`/`DROP`, new categories/columns) stays read-only.
- **D5 Persistence timing (Q5):** write-back to the `.cif` on COMMIT
  (`CifFile::Write(path)`), plus on detach/close.
- **D6 Isolation (Q6):** v1 = mutate-in-place + write-on-commit (no multi-tx
  isolation); per-transaction CifFile copy noted as the isolation upgrade.
- **D7 Global functions (Q7):** `mmcif_scan(file, table)`,
  `mmcif_tables(file)`, `mmcif_relationships(file)` stay read-only (they take a
  path directly, no attached db).

## Out of scope

- DDL / new categories / new columns.
- Multi-transaction isolation and true MVCC.
- Writing through the global table functions.
