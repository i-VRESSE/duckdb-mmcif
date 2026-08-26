# Research 02 — Exposing an ATTACH-able custom type (`TYPE mmcif`) and per-category tables

Ticket: `.scratch/mmcif-extension/issues/02-attach-type.md`
Question: how DuckDB exposes an ATTACH-able custom extension type and how each mmcif category becomes a table, using the SQLite extension as the model.

All core-source claims cite `duckdb/src` in this repo (the `duckdb` submodule); sqlite-extension claims cite the cloned primary source at `github.com/duckdb/duckdb-sqlite` (local clone: `/tmp/opencode/duckdb-sqlite`).

## 1. API surface for `ATTACH ... (TYPE mmcif)` — StorageExtension / attach callback / db_instance

DuckDB resolves a non-`duckdb` `TYPE` to a **StorageExtension** registered against the `DBConfig`. The whole custom-type hook is `duckdb/storage/storage_extension.hpp`:

- `StorageExtension` holds two function pointers plus optional static info:
  - `attach_function_t attach` — `unique_ptr<Catalog> (optional_ptr<StorageExtensionInfo>, ClientContext &, AttachedDatabase &, const string &name, AttachInfo &, AttachOptions &)` (`storage_extension.hpp:25-27`)
  - `create_transaction_manager_t create_transaction_manager` — builds the `TransactionManager` for the attached catalog (`storage_extension.hpp:28-29`)
  - `shared_ptr<StorageExtensionInfo> storage_info` — opaque static info handed to both callbacks (`storage_extension.hpp:37`)
  - `StorageExtension::Find(config, name)` / `StorageExtension::Register(config, name, ext)` — lookup/registration on the config's callback manager (`storage_extension.hpp:48-49`; impl `duckdb/src/main/extension_callback_manager.cpp:71,125,155-165`).

- **Registration is done inside the extension's Load entrypoint.** The sqlite extension, in `LoadInternal`, calls:
  `StorageExtension::Register(config, "sqlite_scanner", make_shared_ptr<SQLiteStorageExtension>())` (`duckdb-sqlite/src/sqlite_extension.cpp:54`), where `SQLiteStorageExtension` just sets `attach` and `create_transaction_manager` in its constructor (`sqlite_storage.cpp:39-42`). `config` comes from `DBConfig::GetConfig(loader.GetDatabaseInstance())` (`sqlite_extension.cpp:44-45`).

- **The `TYPE` keyword maps to the extension name via an alias table.** `ATTACH 'x.cif' (TYPE mmcif)` → `DatabaseManager::GetDatabaseType` applies `ExtensionHelper::ApplyExtensionAlias(db_type)` and checks `StorageExtension::Find(config, extension_name)` (`duckdb/src/main/database_manager.cpp:366-397`). Aliases are in `duckdb/src/main/extension/extension_alias.cpp` (e.g. `sqlite`→`sqlite_scanner`, `mysql`→`mysql_scanner`). So `TYPE mmcif` would resolve to an extension named `mmcif` (or an alias like `cif`→`mmcif`). If not yet registered, `Catalog::TryAutoLoad` / `ExtensionHelper::LoadExternalExtension` loads the extension by that name (`database_manager.cpp:390-396`), which runs `DUCKDB_CPP_EXTENSION_ENTRY(mmcif, loader)` and thus `StorageExtension::Register`.

- **The attach callback runs during `AttachedDatabase` construction.** `DatabaseInstance::CreateAttachedDatabase` looks up `StorageExtension::Find(config, ...)`; if found it builds `AttachedDatabase(db, catalog, *storage_extension, context, name, info, options)` (`duckdb/src/main/database.cpp:173-200`). That constructor calls `catalog = storage_extension->attach(...)`, requires a non-null `Catalog`, and then `transaction_manager = storage_extension->create_transaction_manager(...)` (`duckdb/src/main/attached_database.cpp:150-180`). So the extension's `attach` callback **returns the catalog object** for the attached db — the "db_instance" is `AttachedDatabase`, and the callback receives it plus the `AttachInfo` (path, name, options) and `AttachOptions` (`attach_info.hpp:19-45`).

- **Reference implementations in core.** `duckdb/src/storage/open_file_storage_extension.cpp` shows the minimal attach callback: it creates a `DuckCatalog`, `Initialize(false)`, and returns it (lines 45-67); its `create_transaction_manager` returns `make_uniq<DuckTransactionManager>(db)` (69-72). `OpenFileStorageExtension::Create()` sets both pointers and is registered under `"__open_file__"` (`database.cpp:516`).

- **`AttachInfo.options` are the `(...)` clause.** The sqlite attach reads `attach_options.options` for keys like `busy_timeout`/`journal_mode` and throws on unknown ones (`sqlite_storage.cpp:19-29`). mmcif can read its own options (e.g. dictionary path) the same way.

## 2. Registering attached tables so USE / SHOW TABLES / .schema / .tables / information_schema see `_atom_site`

The sqlite extension makes each sqlite table a **real `CatalogEntry` (a `TableCatalogEntry` subclass) surfaced through the custom catalog/schema**, not just a global table function. Chain:

- Custom `SQLiteCatalog : public Catalog`. It must override (base pure-virtuals in `duckdb/src/include/duckdb/catalog/catalog.hpp:116-452`): `Initialize`, `GetCatalogType()` (returns `"sqlite"`), `CreateSchema`, `ScanSchemas`, `LookupSchema`, `DropSchema`, `GetDatabaseSize`, `InMemory`, `GetDBPath` (`sqlite_catalog.cpp:21-101`; decl `sqlite_catalog.hpp:18-79`). `Initialize` creates a single `SQLiteSchemaEntry` (`sqlite_catalog.cpp:21-24`); `ScanSchemas` calls the callback once with `main_schema` (34-36); `LookupSchema` maps `DEFAULT_SCHEMA`/`INVALID_SCHEMA` to `main_schema` (38-49).

- Custom `SQLiteSchemaEntry : public SchemaCatalogEntry`. The key methods for enumeration are `Scan(ClientContext&, CatalogType, callback)` and `LookupEntry`. `SQLiteSchemaEntry::Scan(TABLE_ENTRY)` enumerates sqlite table names from the db and materializes each as an entry via `GetEntry(...)` (`sqlite_schema_entry.cpp:255-276`). `LookupEntry` delegates to `SQLiteTransaction::GetCatalogEntry(entry_name)` (302-313).

- `SQLiteTransaction::GetCatalogEntry` builds a `CreateTableInfo` with the table's columns/types, then `make_uniq<SQLiteTableEntry>(sqlite_catalog, main_schema, info, all_varchar)` — i.e. a `TableCatalogEntry` subclass carrying a real `ColumnList` of DuckDB `LogicalType`s — and caches it in a per-transaction `SQLiteCatalogMap` (`sqlite_transaction.cpp:183-249`, `SQLiteTableEntry` in `sqlite_table_entry.cpp`). `SQLiteTableEntry` overrides `GetScanFunction` to return a per-table scan `TableFunction` (`SqliteScanFunction`) and `GetStorageInfo` (`sqlite_table_entry.cpp:23-84`).

- **Why USE/SHOW TABLES/.schema/.tables/information_schema all see them:** those all iterate `Catalog::GetAllSchemas` then `SchemaCatalogEntry::Scan(TABLE_ENTRY)`:
  - `duckdb_tables()` (feeds `SHOW TABLES`, `information_schema.tables`, CLI `.tables`) does `Catalog::GetAllSchemas(context)` then `schema.get().Scan(context, CatalogType::TABLE_ENTRY, cb)` (`duckdb/src/function/table/system/duckdb_tables.cpp:77-85`). `duckdb_columns()` similarly scans `TABLE_ENTRY` (`duckdb_columns.cpp:91`), `duckdb_views()` scans `VIEW_ENTRY` (`duckdb_views.cpp:83`).
  - `Catalog::GetAllSchemas` walks every attached database's catalog via `catalog.GetSchemas(context)` → `catalog.ScanSchemas` (`duckdb/src/catalog/catalog.cpp:1168-1195`).
  - `SHOW TABLES` is a pragma that selects from `duckdb_tables` (`duckdb/src/function/pragma/pragma_queries.cpp:23-61`).
  - CLI `.tables` (`shell.cpp:2134-2173`) and `.schema` (`shell_metadata_command.cpp:910,127-147`) query `duckdb_columns()`/`duckdb_tables()`/`information_schema.tables`. `USE` just changes the default catalog in the search path (docs: attach.html; `DatabaseManager::SetDefaultDatabase`).
  - `CatalogSet::Scan` first materializes the schema's `DefaultGenerator` entries via `CreateDefaultEntries` (`duckdb/src/catalog/catalog_set.cpp:663-692`), so generator-based entries also show up.

- **Simpler alternative: DuckCatalog + a `DefaultGenerator` (open-file model).** `open_file_storage_extension.cpp` reuses `DuckCatalog` and installs a `DefaultGenerator` on a schema's `CatalogSet` (`SetDefaultGenerator`) whose `GetDefaultEntries()`/`CreateDefaultEntry` lazily expose entries (`open_file_storage_extension.cpp:10-66`). A generator that returns `TableCatalogEntry`/`TableFunctionCatalogEntry` per mmcif category would be visible to the same scans because `CatalogSet::Scan` calls `CreateDefaultEntries` (`catalog_set.cpp:692-695`). This avoids writing a custom `Catalog`/`SchemaCatalogEntry`, at the cost of less control (single `main` schema, DuckCatalog semantics).

- **For mmcif**: either (a) follow sqlite with `MmcifCatalog`/`MmcifSchemaEntry`/`MmcifTableEntry` where `Scan(TABLE_ENTRY)` enumerates the categories present in the file and each becomes a `TableCatalogEntry` with `GetScanFunction` returning a per-category scan; or (b) reuse DuckCatalog with a DefaultGenerator over the category names. Both make `_atom_site`, `mmcif_relationships`, etc. appear in `USE`/`SHOW TABLES`/`.tables`/`information_schema`.

## 3. Creating `mmcif_relationships(file)` and `mmcif_tables(file)` metadata tables on attach

These are **global table functions registered on Load**, not catalog entries tied to the attached db — matching how sqlite exposes `sqlite_scan(file, table)` / `sqlite_query(db, sql)`:

- In `LoadInternal`, sqlite calls `loader.RegisterFunction(SqliteScanFunction)`, `loader.RegisterFunction(SqliteAttachFunction)`, `loader.RegisterFunction(SQLiteQueryFunction)` (`sqlite_extension.cpp:34-42`). `ExtensionLoader::RegisterFunction(TableFunction)` registers into the **system catalog** (`Catalog::GetSystemCatalog(db)` → `CreateFunction`) so the function is callable globally (`duckdb/src/main/extension/extension_loader.cpp:90-108`; API `extension_loader.hpp:54-57`).
- `SqliteScanFunction` is a `TableFunction("sqlite_scan", {VARCHAR, VARCHAR}, scan, bind, init_global, init_local)` (`sqlite_scanner.cpp:415-424`); `SQLiteQueryFunction` is `TableFunction("sqlite_query", {VARCHAR, VARCHAR}, ...)` (`sqlite_query.cpp:97-105`).
- So `mmcif_relationships(file)` and `mmcif_tables(file)` would be two `TableFunction`s registered via `loader.RegisterFunction` on Load, taking a `file` (VARCHAR) argument. Their bind reads the file, lists the categories/relationships present, and returns the row set — "filtered to categories present in the file" happens naturally because the function parses that file. No attach-time table creation needed.
- Docs confirm the model: sqlite tables are read "as if they were normal DuckDB tables" and `SHOW TABLES` lists them (`core_extensions/sqlite.html` Usage); ATTACH with `(TYPE sqlite)` is the documented syntax (`sql/statements/attach.html`).

## 4. Surfacing column types (from the dictionary type index) to DESCRIBE

`DESCRIBE` (`BindShowQuery` in `duckdb/src/planner/binder/tableref/bind_showref.cpp:90-151`) binds the child plan and emits one row per column, with two paths:

- **Base-table path (attached category tables):** `FindBaseTableColumn` traces a column back to a `TableCatalogEntry` (`bind_showref.cpp:22-83`), then `PragmaTableInfo::GetColumnInfo(table, column, ...)` reads `column.Type().ToString()` — i.e. the real `LogicalType` stored in the entry's `ColumnList` (`pragma_table_info.cpp:218-222`; `PragmaShowHelper::GetTableColumns` at `pragma_table_info.cpp:72-80` writes `column.Type().ToString()`). So if each category's `TableCatalogEntry` is built with a `ColumnList` of `LogicalType`s derived from the dictionary type index (sqlite does exactly this: `GetTableInfo` maps sqlite declared types to DuckDB `LogicalType`s via `SQLiteUtils::TypeToLogicalType`, `sqlite_utils.cpp:57-114`), `DESCRIBE _atom_site` shows DOUBLE/BIGINT/VARCHAR etc. `TableCatalogEntry::columns` is a public `ColumnList` member (`table_catalog_entry.hpp:134-135`).
- **Table-function path (`mmcif_relationships(file)` etc.):** no base table is found, so DESCRIBE reads `plan.types[column_idx]` — the bound `return_types` filled by the function's bind callback (`bind_showref.cpp:117-133`). sqlite's `SQLiteQueryBind` and `SqliteBind` populate `return_types` with concrete `LogicalType`s (`sqlite_query.cpp:77-87`, `sqlite_scanner.cpp:70-74`). So mmcif's metadata table functions surface real types by filling `return_types` with the dictionary-index-mapped `LogicalType`s during bind.
- `TableCatalogEntry::GetScanFunction(context, bind_data)` returns the scan `TableFunction` and can carry the same types into bind (`sqlite_table_entry.cpp:23-66`), so attached-table scans and DESCRIBE agree.

**Conclusion for mmcif:** the dictionary type index (ticket 03) should produce a `LogicalType` per category item; those go into the category `TableCatalogEntry`'s `ColumnList` (DESCRIBE base-table path) and into the metadata table functions' `return_types` (DESCRIBE function path). Both mechanisms surface DOUBLE/BIGINT/VARCHAR.
