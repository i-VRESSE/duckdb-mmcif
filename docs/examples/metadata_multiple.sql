-- Resolution and deposition date across a collection, without ATTACH.
-- Edit this glob to point at your files. It should match multiple files so
-- that mmcif_scan exposes filename as the join key.
-- Run from the repository root:
--   ./build/release/duckdb < docs/examples/metadata_multiple.sql
SET VARIABLE cif_files = 'structures/**/*.cif.gz';

-- A category missing from one file contributes no rows. LEFT JOIN preserves
-- the entry when resolution or deposition date is absent from that file.
-- Each scanned category must occur in at least one matched file.
-- Reduce categories to one row per filename before joining: a file can have
-- several refinement records, and entry IDs need not be unique across files.
WITH entries AS (
    SELECT filename, min(id) AS id
    FROM mmcif_scan(getvariable('cif_files'), 'entry')
    GROUP BY filename
), resolutions AS (
    SELECT filename, min(ls_d_res_high) AS resolution
    FROM mmcif_scan(getvariable('cif_files'), 'refine')
    GROUP BY filename
), deposits AS (
    SELECT filename,
           min(recvd_initial_deposition_date::DATE) AS deposit_date
    FROM mmcif_scan(getvariable('cif_files'), 'pdbx_database_status')
    GROUP BY filename
)
SELECT e.filename, e.id, r.resolution, d.deposit_date
FROM entries e
LEFT JOIN resolutions r USING (filename)
LEFT JOIN deposits d USING (filename)
ORDER BY e.filename;
