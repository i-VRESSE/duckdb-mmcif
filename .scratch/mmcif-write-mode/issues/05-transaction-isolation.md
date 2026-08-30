# Transaction isolation semantics (D6)

Type: task
Status: pending
Blocked by: 04

## Question

What isolation do write-mode transactions get?

## Findings

- `MmcifTransactionManager::StartTransaction` returns a `Transaction` with no
  data state; `CommitTransaction`/`RollbackTransaction` only erase the map
  entry (`src/mmcif_core.cpp:672-687`). There is no isolation today even for
  reads.
- Full isolation would require a per-transaction copy of the CifFile
  (parse/duplicate per transaction, merge on commit) — expensive for large
  structural files.

## Decision (agreed)

v1 = mutate-in-place + write-on-commit (single-writer, no multi-transaction
isolation). Mutations are visible within the transaction and flushed to the
`.cif` on COMMIT; ROLLBACK discards (re-parse from disk). Per-transaction
CifFile copy is the isolation upgrade path, noted but not implemented.

## Acceptance

- Within one connection, a write followed by a SELECT in the same transaction
  sees the write; COMMIT persists; ROLLBACK reverts.
- Concurrent write transactions are not isolated (documented limitation).
