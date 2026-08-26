# duckdb-mmcif

A DuckDB extension that exposes mmCIF (PDBx) structural-biology data files as read-only DuckDB tables. It builds on the RCSB `cpp-cif-parser` / `cpp-cif-file` core libraries (see `modules/`) and is loaded through the `waddle` extension.

With this extension you can `ATTACH` a `.cif` file and immediately query every category in it as a normal DuckDB table, with column types inferred from the embedded [mmCIF dictionary](https://mmcif.wwpdb.org/) type index.

## Building

Requires DuckDB and the extension-ci-tools submodules; fetch them with `--recurse-submodules` when cloning.

```sh
make
```

The main binaries built are:

```sh
./build/release/duckdb                                # duckdb shell with the extension pre-loaded
./build/release/test/unittest                         # DuckDB test runner (extension linked in)
./build/release/extension/waddle/waddle.duckdb_extension   # loadable extension binary
```

To speed up rebuilds install [ccache](https://ccache.dev/) and [ninja](https://ninja-build.org/) and build with `GEN=ninja make`.

## Running the tests

SQL tests live in `./test/sql` (see `test/sql/mmcif.test`). Run them with:

```sh
make test
```

## Using the extension

Start the shell (the extension is pre-loaded):

```sh
./build/release/duckdb
```

### ATTACH a cif file

Attach a `.cif` file as a database using the `mmcif` storage type. Give it an explicit alias so the catalog name is predictable:

```sql
ATTACH '1amb_updated.cif' AS mmcifdb (TYPE mmcif);
```

Without an alias the database name is derived from the file name. Either way, `USE` the catalog to make its tables current:

```sql
USE mmcifdb;
```

### Query categories as tables

Each category in the file becomes a table. List them with `SHOW TABLES` (43 categories for the sample file), inspect a table, and scan it:

```sql
SHOW TABLES;

DESCRIBE atom_site;
-- Cartn_x DOUBLE, label_seq_id BIGINT, type_symbol VARCHAR, ...

SELECT count(*) FROM atom_site;          -- 438

SELECT Cartn_x, type_symbol FROM atom_site ORDER BY label_atom_id LIMIT 2;
-- 17.882 C
-- 16.209 C

SELECT count(*) FROM atom_site WHERE Cartn_x > 5;   -- 155
```

Column types come from the mmCIF dictionary type index (`dict/mmcif_pdbx_v50_type_index.tsv.gz`), so e.g. `Cartn_x` is `DOUBLE` and `label_seq_id` is `BIGINT`. Cells containing `.` or `?` are mapped to `NULL`.

### Metadata table functions

The extension also provides three global table functions that work without attaching a database:

```sql
-- one row per (category, column) with its inferred type
SELECT * FROM mmcif_tables('1amb_updated.cif');

-- one row per parent/child category relationship
SELECT * FROM mmcif_relationships('1amb_updated.cif');

-- scan a single category directly
SELECT * FROM mmcif_scan('1amb_updated.cif', 'atom_site');
```

### Read-only

mmcif databases are read-only. Data definition and mutation statements are rejected:

```sql
CREATE TABLE foo(i int);      -- Binder Error: mmcif databases are read-only - cannot CREATE TABLE
DELETE FROM atom_site;        -- Binder Error: Can only delete from base table
UPDATE atom_site SET type_symbol='X';  -- Binder Error: Can only update base table
```

## Loading the distributed binary

The loadable extension (`waddle.duckdb_extension`) can be loaded into a DuckDB started with unsigned extensions allowed:

```sh
duckdb -unsigned
```

```sql
LOAD 'build/release/extension/waddle/waddle.duckdb_extension';
```
