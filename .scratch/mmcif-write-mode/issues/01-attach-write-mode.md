# ATTACH write-mode option (D1)

Type: task
Status: pending
Blocked by:

## Question

How does a user open an attached mmcif database in write mode, with read-only
as the default?

## Findings

- DuckDB core consumes `readonly`/`read_only` and `readwrite`/`read_write`
  keys into `AttachOptions::access_mode`
  (`duckdb/src/main/attached_database.cpp:39-69`); `type` is consumed too.
- The default `access_mode` comes from `config.options.access_mode`, which is
  `READ_WRITE` (`duckdb/src/execution/operator/schema/physical_attach.cpp:21`).
  So keying write mode off `options.access_mode` alone would make mmcif
  write-by-default.
- `AttachInfo.options` (`duckdb/src/include/duckdb/parser/parsed_data/attach_info.hpp:34`)
  is the raw pre-consumption `unordered_map<string, Value>`, passed unchanged
  to `MmcifAttach` (`src/mmcif_core.cpp:704`). It still contains the
  explicit `readwrite`/`read_only` keys, so the extension can detect whether
  the user explicitly asked for a mode.
- `options.options` (`attached_database.hpp:71`) only holds *unconsumed* keys,
  so it cannot be used for builtin access keys.

## Decision (agreed)

Use builtins: `ATTACH 'x.cif' AS db (TYPE mmcif, READ_WRITE TRUE)` enters write
mode; `READ_ONLY TRUE` (or no access key) stays read-only. `MmcifAttach`
checks `info.options` for an explicit `readwrite`/`read_write` key; if absent,
default read-only.

## Acceptance

- `ATTACH 'x.cif' AS db (TYPE mmcif)` -> read-only (default).
- `ATTACH 'x.cif' AS db (TYPE mmcif, READ_ONLY TRUE)` -> read-only.
- `ATTACH 'x.cif' AS db (TYPE mmcif, READ_WRITE TRUE)` -> write mode;
  `INSERT INTO software VALUES (...)` mutates the in-memory CifFile and the
  attached `.cif` carries the new row after COMMIT.
