# Global table functions stay read-only (D7)

Type: task
Status: pending
Blocked by:

## Question

Do `mmcif_scan(file, table)`, `mmcif_tables(file)`, `mmcif_relationships(file)`
gain write ability?

## Decision (agreed)

No — keep them read-only. They take a file path directly (no attached database),
so there is no catalog/CifFile to mutate or persist. Write mode is only via an
attached database opened with `READ_WRITE TRUE`.

## Acceptance

- Global functions continue to reject writes; no new write-capable variant.
