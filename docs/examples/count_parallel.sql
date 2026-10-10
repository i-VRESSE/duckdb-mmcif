-- Count atoms with one dictionary scan and DuckDB's bounded worker pool.
-- Edit the glob below. At most one active file is retained per worker;
-- threads caps concurrent readers and the input file count is another cap.
-- Set threads = 1 for a sequential scan.
-- Run from the repository root:
--   ./build/release/duckdb < docs/examples/count_parallel.sql
SET threads = 6;
SET enable_progress_bar = true;
SET progress_bar_time = 1000;

SELECT count(*) AS atoms
FROM mmcif_scan('structures/**/*.cif.gz', 'atom_site',
                column_source := 'dictionary');
