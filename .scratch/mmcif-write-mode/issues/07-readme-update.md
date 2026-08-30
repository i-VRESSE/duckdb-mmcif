# README: document both modes (D1 follow-up)

Type: task
Status: pending
Blocked by: 01

## Question

How should the README present ATTACH modes?

## Decision (agreed)

Use DuckDB's builtin access keys, read-only by default:
- Select-only examples use `ATTACH 'x.cif' AS db (TYPE mmcif, READ_ONLY TRUE)`.
- Write examples use explicit `ATTACH 'x.cif' AS db (TYPE mmcif, READ_WRITE TRUE)`
  followed by `INSERT`/`UPDATE`/`DELETE` and a note that COMMIT writes the new
  rows back to the attached `.cif`.
- The current `### Read-only` section is renamed/extended to a
  `### Read-only (default)` + `### Write mode (READ_WRITE)` pair.

## Acceptance

- README reflects read-only default and the `READ_WRITE TRUE` opt-in.
