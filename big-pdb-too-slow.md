# Why `SELECT * FROM atom_site LIMIT 10` takes ~16s on 3J3Q.cif.gz

**tl;dr** — the extension fully re-parses the entire 242 MB uncompressed file **three times per query**, and re-parses it once **per category** whenever table names are enumerated. The RCSB flex/bison parser itself is also slow (~50 MB/s). A `LIMIT 10` never avoids any of this work, and it costs ~5.6 GB of peak RSS.

## Measured numbers (build/release/duckdb, v1.5.4, this machine)

File: `3J3Q.cif.gz` = 48 MB gzip → **241,803,672 B** uncompressed, 2,843,773 lines, **73 categories**, `atom_site` = **2,440,800 rows × 21 cols**.

| Query | wall | peak RSS | parses |
|---|---|---|---|
| `ATTACH '3J3Q.cif.gz' AS db (TYPE mmcif);` | 0.01 s | — | 0 (lazy) |
| `SELECT * FROM db.atom_site LIMIT 10` | **15.9 s** | **5.6 GB** | **3** |
| `SELECT count(*) FROM db.atom_site` | 17.5 s | 5.6 GB | 3 |
| `SELECT count(Cartn_x) FROM db.atom_site` | 17.6 s | 5.6 GB | 3 |
| `SELECT * FROM atom_site LIMIT 10` (unqualified, no `USE`) | ~7 min | — | **75** |
| `SHOW TABLES` | ~7 min | — | 75 |
| same LIMIT query twice in one session | — | — | 6 (3 per statement) |
| `SELECT count(*) FROM mmcif_scan('3J3Q.cif.gz','atom_site')` | 6.6 s | 3.7 GB | 1 |
| `CREATE TABLE atoms AS SELECT * FROM mmcif_scan('3J3Q.cif.gz','atom_site')` | 11.8 s | 3.8 GB | 1 |

Per-parse breakdown (instrumented): read ≈ 0.01 s (page-cached) + gunzip ≈ 0.41–0.56 s + `ParseString` ≈ **4.3–5.2 s** ≈ 50 MB/s single-thread. `gunzip -c` of the whole file alone: **0.47 s**. `gzip -c` (compress): 5.5 s. Loading rows into bind data (`MmcifLoadRows`, 2.44M×21 strings): 0.2 s.

So of the 16 s, **~15 s is `ParseString` ×3**, ~1.3 s is gunzip ×3, and the scan of 10 rows is negligible.

## Root causes

1. **Read-only mode re-parses the whole file on every resolve, with no cache.**
   `MmcifResolveCifFile` (src/mmcif_core.cpp:868) parses a fresh copy of the file each call; only write-mode keeps a persistent `CifFile` (mmcif_core.cpp:683). A single query walks the binder path three times:
   - `MmcifSchemaEntry::LookupEntry` → parse #1 (mmcif_core.cpp:650)
   - `MmcifTableEntry::GetTableEntry` → parse #2 (mmcif_core.cpp:608)
   - `MmcifTableEntry::GetScanFunction` → parse #3 (mmcif_core.cpp:494)
   → 3 × ~5.5 s ≈ 16.5 s. Every statement repeats this (6 parses for 2 statements).

2. **Enumerating tables re-parses once per category.**
   `MmcifSchemaEntry::Scan` (mmcif_core.cpp:626) calls `GetTableEntry` for every category, and each `GetTableEntry` calls `MmcifResolveCifFile` again → 73 parses. `SHOW TABLES` and any unqualified-name resolution (which scans the attached schema) pay **75 full parses ≈ 7 minutes**.

3. **Bind loads *all* rows even for `LIMIT 10`.**
   `MmcifLoadRows` (mmcif_core.cpp:260) copies every row into `bind_data->rows` (`vector<vector<string>>`, 2.44M×21 std::strings) during bind. LIMIT can't short-circuit it. Peak RSS 5.6 GB ≈ cached CifFile (~2–3 GB) + this row copy (~1.6 GB).

4. **The parser is slow.**
   `CifParser::ParseString` (modules/cpp-cif-parser/src/CifParserBase.C:164) uses flex `cifparser__scan_string`, which **copies the entire 242 MB string** into a flex buffer, then lexes token-by-token into `std::string` and stores every cell as a `std::string` (32-byte SSO objects → ~51M cells). ~50 MB/s, single-threaded, 3–4 passes over the text.

5. **gzip is re-run per parse.** DuckDB's `UncompressGZIPString` (0.45 s) plus the 48 MB→242 MB string copy happen once per parse; the decompressed 242 MB is never cached.

6. **Row emission is per-cell `Value(...)`.** `MmcifScan` (mmcif_core.cpp:296) converts one cell at a time with `Value(r[col_id])` + cast instead of vectorized string→typed conversion (adds ~1–2 s on full scans).

7. Minor: `MmcifBindData::Copy()` deep-copies all rows and `Equals()` always returns false (mmcif_core.cpp:227,237); `GetStorageInfo` hardcodes cardinality 10000 and `GetStatistics` returns nullptr (mmcif_core.cpp:522,490) so the planner gets no real stats; `mmcif_tables`/`mmcif_relationships` each parse twice (mmcif_core.cpp:400–405).

## Recommended fixes (ordered by impact/effort)

### 1. Parse once per attached catalog (biggest, cheapest win)
Give read-only mode a persistent, lazily-initialized `CifFile` on `MmcifCatalog` (same mechanism write-mode already uses), guarded by a mutex; have `MmcifResolveCifFile` return it instead of re-parsing. Result:
- Query: 3 parses → 1 → **16 s → ~5.7 s** (first query), ~0 s for subsequent statements.
- `SHOW TABLES` / unqualified resolution: 75 parses → 1 → **7 min → ~6 s**.
This is ~80% of the problem for essentially no risk. Keep `ReloadFromDisk` semantics for ROLLBACK in write mode.

### 2. Cache the parsed file across statements/attaches
Key the cached `CifFile` (or at least the decompressed 242 MB content) by `(path, mtime, size)` at process level so re-attaching the same file in one DuckDB session, or querying it repeatedly, skips re-gunzip + re-parse entirely. Especially valuable for httpfs/S3 reads, where today every parse re-downloads 48 MB.

### 3. Stream rows instead of copying at bind
Make `MmcifBindData` hold a reference/shared_ptr to the cached `CifFile` + `ISTable` and emit rows in `MmcifScan` via `table.GetRow(r)`, instead of materializing `vector<vector<string>>` at bind. Removes the ~1.6 GB copy, the 5.6 GB peak, and lets `LIMIT 10` really touch 10 rows. Pairs with fix 1.

### 4. Faster parser (the fundamental fix for huge files)
Replace flex/bison with a hand-written streaming mmCIF scanner (the format is line-oriented, like a fast CSV reader; 1–2 GB/s is realistic), storing cells into a flat string arena (offsets into one buffer) instead of `vector<std::string>`. Would take `ParseString` from ~4.8 s to ~0.2–0.5 s per parse — 10–20×.

### 5. Two-pass lazy index parse (long-term "right" answer)
Pass 1 builds a lightweight index of (data block, categories, column names, loop line offsets) by scanning without building strings — makes `mmcif_tables()`, schema resolution, and `DESCRIBE` fast with **no** full parse. Pass 2 materializes only the queried category, seeking to its loop start; enables per-category lazy load, `LIMIT` pushdown to seek, and parallel per-category parsing (the table-function scan currently reports `MaxThreads()=1`).

### 6. Vectorized scan
Convert rows column-wise (string→DOUBLE/BIGINT/VARCHAR) with DuckDB's vectorized casts instead of per-cell `Value()`.

### 7. Planner hints
Feed real row count from the cached parse into `GetStorageInfo` and return basic min/max stats from `GetStatistics`; drop the hardcoded cardinality 10000. Fix `Equals()`/`Copy()` so bind data doesn't deep-copy 2.44M rows.

## Workarounds today (no code changes)

- **Materialize once:** `CREATE TABLE atoms AS SELECT * FROM mmcif_scan('3J3Q.cif.gz','atom_site')` ≈ 11.8 s (1 parse), then all queries are instant. Note each category still costs a full parse.
- **`mmcif_scan()` instead of ATTACH for single-category queries:** 6.6 s vs 16 s (1 parse vs 3) — e.g. `SELECT * FROM mmcif_scan('3J3Q.cif.gz','atom_site') LIMIT 10`.
- **ATTACH with `READ_WRITE TRUE`** parses once at attach (fast queries), but it enables DML and writes the file back (gzip-compress ~5.5 s+) on COMMIT/DETACH/exit — unsafe for analysis; not recommended.

## Replacing the parser: C++ libraries from the wwPDB software-resources page

The current `modules/cpp-cif-parser` + `cpp-cif-file` are the RCSB CIFPARSE-OBJ lineage (~50 MB/s, last updated 2013-era). The wwPDB "Software resources" page lists these C/C++ options that could beat it:

- **GEMMI** (`project-gemmi/gemmi`) — the strongest candidate. C++14, PEGTL-based fast CIF parser. Directly relevant to us:
  - `read_cif_gz(path)` decompresses `*.gz` on the fly; `read_cif_from_memory(data,size,name)` / `read_string(data)` parse from the in-memory buffer we already have after gunzip.
  - DOM is `Document`/`Block`/`Loop` with `tags` + flat `values` (`std::vector<std::string>`) — the same shape as the `ISTable` the extension uses today, so the migration maps cleanly.
  - Helpers `as_number()`, `as_int()`, `is_null()` cover `.`/`?` null handling; `write_cif_*` for write-back.
  - Header-only or `libgemmi_cpp`; updated 2026; mature (387 stars, CCP4/Global Phasing). License: **MPL-2.0 OR LGPLv3** — copyleft, so embedding it in a distributed extension needs a license review (MPL-2.0 is file-level; LGPLv3 affects linking). Fine if this repo stays OSS.
- **libcif++** (`PDB-REDO/libcifpp`) — safest swap. It is the modern, actively-maintained (2025) C++17 successor of the exact RCSB lineage already used; **BSD-2-Clause** (permissive, ideal for embedding). Reads gzip (`cif::pdb::read(path)`), dictionary-aware with DDL validation. Heavier (needs Eigen/pcre2/zlib) and full-DDL; migration is moderate. Best license + lineage fit.
- **readcif** (`RBVI/readcif`) — "a fast C++ CIF and mmCIF parser", C++11, one header + one source file to vendor. Permissive license with an acknowledgement clause. Leanest to integrate as a low-level tokenizer; small project (11 stars).
- **iotbx.cif / ucif** (cctbx/Phenix) and **CCIF** (CCP4) — bigger dependency trees and dictionary-validation focus; overkill for a read path.

None of these beat the two-pass index idea for *schema enumeration* (they still parse the whole file to find categories); they attack the 5 s/parse bottleneck. GEMMI is the best on speed + features, libcif++ the best on license/lineage, readcif the leanest. Swapping any of them means reworking `mmcif_core.cpp`'s storage + write-mode coupling to `CifFile`/`ISTable`.

### Measured: GEMMI on 3J3Q (single-threaded, `libgemmi_cpp`, -O2)

Verified against the cloned repo (`../gemmi/`), built `libgemmi_cpp` with `INTERNAL_ZLIB=ON`:

| path | gemmi time | current extension | speedup |
|---|---|---|---|
| full `read_cif_gz("3J3Q.cif.gz")` (read+gunzip+parse) | **1.538 s** | ~5.7 s (0.5 s gunzip + 4.3–5.2 s ParseString) | **~3.7×** |
| parse only (decompressed 242 MB file) | **1.420 s** | ~4.8 s ParseString | **~3.4×** |
| iterate `atom_site` (2,440,800 rows, 2 cols, `as_number`) | **0.029 s** | bind copy of all 21 cols ~1.6 GB | ~O(1) vs full copy |

Row count verified: 2,440,800 (matches). GEMMI stores each category as a single flat `std::vector<std::string>` (column-stride), so category iteration is contiguous-memory and effectively free (29 ms) — that removes the bind-copy cost entirely, not just the parse. End-to-end, a `LIMIT 10` query could drop from ~16 s to ~1.6 s with a parse-once cache, and schema enumeration (75 parses) from ~7 min to ~1.9 min.

## Storage layer: duckdb-nanoarrow vs DuckDB native

**duckdb-nanoarrow is not a storage engine** — it's an MIT-licensed bridge to the Apache Arrow ecosystem: read/write Arrow IPC streams/files (`read_arrow`/`.arrows`), and `from_arrow`/`to_arrow_ipc` for Python/Node. Storing parsed mmCIF "as in-memory tables" via it would require: parse → build Arrow arrays → serialize to Arrow IPC → DuckDB reads them back — a serialization round-trip that buys nothing inside DuckDB.

**DuckDB native in-memory storage is strictly better** for pure in-memory querying: parse once, `INSERT INTO` a real DuckDB table (columnar, vectorized, compressed; no serialization). It also fixes the extension's current worst storage behavior — the custom `vector<vector<string>>` bind data + per-cell `Value()` casts (the 5.6 GB peak). The `CREATE TABLE atoms AS ...` workaround above already demonstrates native storage: 11.8 s once, then instant queries.

Use nanoarrow (or DuckDB core's `arrow_scan`) only if the goal is **interop** — exporting `atom_site` to pandas/polars/numpy as Arrow. It does not address the parsing bottleneck or the in-memory storage cost, and it is a separate build dependency.

## Implemented: streaming scanner + two-pass lazy index (recommendations 1–7)

Recommendations 5 and 4 are now implemented in the read-only path (`src/mmcif_index.hpp/.cpp`, wired into `src/mmcif_core.cpp`), which also covers 1 (catalog lazy index), 2 (process-level content+index cache), 3 (stream rows, no bind copy), 6 (vectorized scan + cast), and 7 (exact cardinality via `GetRowCount`; real min/max `GetStatistics` deliberately skipped). Write-mode DML keeps the RCSB `CifFile` path.

New measured numbers (same machine, `build/release/duckdb`, v1.5.4, `3J3Q.cif.gz`):

| Query | before | after | speedup |
|---|---|---|---|
| `SELECT * FROM atom_site LIMIT 10` (cold) | 15.9 s / 5.6 GB | **0.49 s / 341 MB** | ~32× |
| same LIMIT, second time in one session (index cached) | 6 parses (~32 s) | ~instant (~0.18 s) | ~O(1) |
| `SELECT count(*) FROM atom_site` | 17.5 s / 5.6 GB | **1.32 s** | ~13× |
| `SELECT table_name FROM duckdb_tables() ...` (SHOW TABLES) | ~7 min (75 parses) | **0.94 s** | ~450× |
| `mmcif_tables('3J3Q.cif.gz')` | ~7 min | (pass-1, no cell materialization) | ~O(1) |

Why the big win: the index path does one gunzip + one pass-1 line scan (~0.5 s) to build the category index, then a `LIMIT 10` parses only ~10 rows from the byte cursor (`MmcifScanIndex`), and cells are `(offset,len)` slices into one flat decompressed buffer — no 2.44M×21 string copies, no per-cell `Value()` casts, so peak RSS drops from 5.6 GB to ~341 MB. `count(*)` uses `GetRowCount`, a value-scan of the loop range (no string materialization). SHOW TABLES/catalog enumeration never materialize cells.

Remaining gap vs GEMMI's 1.5 s end-to-end parse: our pass-1 is a byte-wise line scan (~0.5 s incl. gunzip), not a hand-optimized C parser; a full `count(Cartn_x)`/aggregate still value-scans 2.44M cells. GEMMI would close that (~3.4×), but the index design already removes the parse-per-query and parse-per-category blowups, and LIMIT/point queries are now effectively O(rows scanned).


