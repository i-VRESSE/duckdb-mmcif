-- Count atoms using six independent scans so DuckDB can use six cores.
-- Edit the glob below. This example requires at least six matched files.
-- Single mmcif_scan readers are currently sequential; the six disjoint
-- batches below can run concurrently without retaining the whole collection.
-- Run from the repository root:
--   ./build/release/duckdb < docs/examples/count_parallel.sql
SET threads = 6;
SET enable_progress_bar = true;
SET progress_bar_time = 1000;
SET VARIABLE cif_files = (SELECT list(file ORDER BY file) FROM glob('structures/**/*.cif.gz'));

SELECT sum(atoms)::BIGINT AS atoms
FROM (
    SELECT count(*) AS atoms
    FROM mmcif_scan(list_slice(getvariable('cif_files'), 1, len(getvariable('cif_files')), 6),
                    'atom_site', column_source := 'dictionary')
    UNION ALL
    SELECT count(*) AS atoms
    FROM mmcif_scan(list_slice(getvariable('cif_files'), 2, len(getvariable('cif_files')), 6),
                    'atom_site', column_source := 'dictionary')
    UNION ALL
    SELECT count(*) AS atoms
    FROM mmcif_scan(list_slice(getvariable('cif_files'), 3, len(getvariable('cif_files')), 6),
                    'atom_site', column_source := 'dictionary')
    UNION ALL
    SELECT count(*) AS atoms
    FROM mmcif_scan(list_slice(getvariable('cif_files'), 4, len(getvariable('cif_files')), 6),
                    'atom_site', column_source := 'dictionary')
    UNION ALL
    SELECT count(*) AS atoms
    FROM mmcif_scan(list_slice(getvariable('cif_files'), 5, len(getvariable('cif_files')), 6),
                    'atom_site', column_source := 'dictionary')
    UNION ALL
    SELECT count(*) AS atoms
    FROM mmcif_scan(list_slice(getvariable('cif_files'), 6, len(getvariable('cif_files')), 6),
                    'atom_site', column_source := 'dictionary')
) counts;
