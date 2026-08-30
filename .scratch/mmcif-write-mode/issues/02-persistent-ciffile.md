# Persistent shared CifFile (D2)

Type: task
Status: pending
Blocked by: 01

## Question

Where does the mutable in-memory `CifFile` live so mutations survive across
scans and can be written back?

## Findings

- Today the file is re-parsed into a fresh `CifFile` per access:
  `MmcifSchemaEntry::GetTableEntry` (`src/mmcif_core.cpp:527`),
  `MmcifTableEntry::GetScanFunction` (`src/mmcif_core.cpp:425`),
  `LookupEntry` (`src/mmcif_core.cpp:566`), and `Scan` (`src/mmcif_core.cpp:545`).
  Any mutation would be lost on the next access.
- `MmcifCatalog` (`src/mmcif_core.cpp:579`) owns `main_schema`
  (`MmcifSchemaEntry`), which holds `file_name` and re-parses on demand.
- RCSB `CifFile` is read/write capable: `eFileMode` read-only/create/update/
  virtual (`modules/cpp-cif-file/include/TableFile.h:332`), and
  `CifFile::Write(cifFileName)` writes the whole in-memory content to a text
  file (`modules/cpp-cif-file/src/CifFile.C:165`).

## Decision (agreed)

In write mode, parse once at ATTACH into a single persistent `CifFile` owned by
`MmcifCatalog` (shared via `MmcifSchemaEntry`/`MmcifTableEntry`), mutated by DML
operators, written back on COMMIT. Read-only mode keeps the re-parse-on-demand
path unchanged.

## Acceptance

- One parse per ATTACH in write mode; no re-parse per table access.
- Mutations applied by INSERT/UPDATE/DELETE are visible to subsequent SELECTs
  in the same transaction (re-snapshot at `GetScanFunction` time).
