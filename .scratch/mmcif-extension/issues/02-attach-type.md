# Implement the mmcif ATTACH extension type

Type: research
Status: resolved
Blocked by:

## Question

How does DuckDB expose an ATTACH-able custom extension type, and how do we use it to expose each mmcif category as a table? The SQLite extension is the model (`ATTACH 'x.db' (TYPE sqlite); USE db; SHOW TABLES; SELECT * FROM _atom_site;`). Resolve:

- The extension API surface for a custom `TYPE` in `ATTACH ... (TYPE mmcif)` (db_instance / catalog / attach callbacks).
- How attached tables are registered so `USE`, `SHOW TABLES`, `.schema`, `.tables`, `information_schema` all see `_atom_site` etc. (table functions per category? catalog entries?).
- How the extension also creates the `mmcif_relationships(file)` and `mmcif_tables(file)` metadata tables on attach, filtered to categories present in the file.
- How table-function column types (from the dictionary index) are surfaced so `DESCRIBE` shows DOUBLE/BIGINT/VARCHAR.

## Answer

**Resolution.** DuckDB exposes a custom `TYPE` via the `StorageExtension` API. The extension's Load entrypoint registers a `StorageExtension` (with `attach` + `create_transaction_manager` callbacks) against the `DBConfig` under its extension name (`StorageExtension::Register`, `storage_extension.hpp:25-50`; sqlite does it in `sqlite_extension.cpp:54`). `ATTACH 'x.cif' (TYPE mmcif)` resolves `mmcif` to that name via `ExtensionHelper::ApplyExtensionAlias` and calls `StorageExtension::Find`; on attach, `AttachedDatabase` invokes the `attach` callback, which **returns the Catalog** for the attached db (`database.cpp:173-200`, `attached_database.cpp:150-180`). `TYPE` is just one entry of `AttachInfo.options`.

Attached tables are surfaced as real catalog entries: sqlite builds a custom `SQLiteCatalog : Catalog` + `SQLiteSchemaEntry : SchemaCatalogEntry` whose `Scan(TABLE_ENTRY)` enumerates table names and materializes a `SQLiteTableEntry : TableCatalogEntry` per table (with `GetScanFunction` returning a per-table scan). `USE`/`SHOW TABLES`/`.tables`/`information_schema` all reach these via `Catalog::GetAllSchemas` → `schema.Scan(TABLE_ENTRY)` (`duckdb_tables.cpp:77-85`, `pragma_queries.cpp:23-61`, `shell.cpp:2134`). A simpler alternative: reuse `DuckCatalog` + a `DefaultGenerator` on the schema's `CatalogSet` (open-file model, `open_file_storage_extension.cpp`).

`mmcif_relationships(file)` / `mmcif_tables(file)` should be **global table functions registered on Load** via `loader.RegisterFunction` (sqlite's `sqlite_scan`/`sqlite_query` pattern, `sqlite_extension.cpp:34-42`), taking a `file` argument; the file-scoped filter falls out of parsing that file.

DESCRIBE surfaces real types two ways: attached category tables use the base-table path reading `column.Type()` from the entry's `ColumnList` (`bind_showref.cpp:90-151`, `pragma_table_info.cpp:218-222`); table functions use the bound `return_types` (`bind_showref.cpp:117-133`). Both should be filled from the dictionary type index mapped to `LogicalType` (ticket 03).

**Findings:** `.scratch/mmcif-extension/research/02-attach-type.md` — full citations and code paths for each claim.
