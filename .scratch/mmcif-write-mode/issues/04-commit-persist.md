# Persist back to .cif on COMMIT (D5)

Type: task
Status: pending
Blocked by: 03

## Question

When does the mutated in-memory CifFile get written back to the attached `.cif`
file?

## Findings

- `CifFile::Write(cifFileName)` opens the target `ios::out | ios::trunc` and
  writes the entire in-memory content (`modules/cpp-cif-file/src/CifFile.C:165-175`).
  Full rewrite, not incremental.
- `MmcifTransactionManager` (`src/mmcif_core.cpp:667`) today only tracks
  transactions in a `reference_map_t`; `CommitTransaction` erases without
  touching data, `Checkpoint` throws
  (`src/mmcif_core.cpp:680-692`).
- `AttachedDatabase::~AttachedDatabase` calls `Close(TRY_CHECKPOINT)`
  (`duckdb/src/main/attached_database.cpp:182-187`); storage extensions can
  hook checkpoint via `StorageExtension::OnCheckpointStart/End`
  (`duckdb/src/include/duckdb/storage/storage_extension.hpp:42-45`).

## Decision (agreed)

Write-back on COMMIT (`cif_file->Write(path)`), plus on detach/close so a
connection drop without COMMIT still flushes. ROLLBACK discards in-memory
mutations (see D6).

## Acceptance

- After `COMMIT`, `SELECT * FROM software` (re-attached) shows the new row, and
  the `.cif` file on disk contains it.
- `ROLLBACK` leaves the in-memory CifFile (and file) unchanged.
