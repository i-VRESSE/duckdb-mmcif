# Multi-file scan improvements

Multi-file access is provided by `mmcif_scan`; `ATTACH` remains single-file.
Baseline on 6,397 protein-quest gzip files (49,203,033 atoms): a fresh `LIMIT 1`
takes 36.0 seconds and peaks at 6.21 GiB RSS; a coordinate aggregate takes
56.1 seconds and peaks at 6.22 GiB. Measurements use two fresh processes,
one DuckDB thread and filesystem caches that were not flushed.

Ranked by expected impact:

1. **Dictionary-backed schemas plus lazy file readers — biggest overall impact.**
   Bind a known category from complete bundled dictionary columns and types,
   without opening every input. Open files as execution advances and release
   finished contents. This addresses startup latency and collection-sized RAM.
   Start with an explicit `column_source := 'dictionary'` mode; preserve automatic
   union discovery as the default. Reject custom category items in dictionary
   mode with guidance to use discovery, rather than silently dropping data.
   Missing categories produce no rows; missing known columns produce typed NULL.
   Each active reader initially still loads one whole decompressed file.

2. **Compact schema discovery plus lazy readers — biggest memory improvement
   while preserving automatic union.** Inspect files individually and retain
   only schemas and source identities. An initial discovery pass remains, and
   reopening files for execution can increase full-scan runtime.

3. **Cache compact metadata with file-change detection — biggest benefit for
   repeated metadata queries.** Cache schemas and small category records, not
   decompressed file bodies. Dictionary `is_single_row` can guide compact
   storage; multi-row categories such as `refine` require separate handling.
   `is_mandatory` supports optional validation, not assumptions about file data.

4. **Streaming gzip/CIF parsing — strongest memory bound within each file.**
   Use bounded buffers rather than retaining a whole decompressed file. This is
   particularly useful for unusually large individual inputs.

5. **Filename filter pushdown.** Skip irrelevant files before opening them.
   Potentially large gains for selective queries; little gain for full scans.

6. **Bounded parallel reading.** Improve full-scan throughput across files after
   memory is bounded. Limit concurrent readers because each adds memory.

7. **Dictionary-guided validation and planning.** Use `is_single_row` and
   `is_mandatory` only where conformance is enforced. These flags describe
   dictionary constraints, not validated contents or file positions.

## Improvement 1: implemented

- Complete category schemas must include valid items missing from the old type
  artifact, including `atom_site.id`; generation must be reproducible offline.
- Bind dictionary schemas without reading CIF contents. Keep only one active
  file index per scan, reset file-local cursors and map columns by name.
- Output strings must remain valid after the reader releases its file buffer.
- Preserve gzip, block selection, file order, projection and existing union-mode
  behavior. Unknown items fail when their file is reached in dictionary mode.
- A known category absent from every file yields an empty typed result in
  dictionary mode; no dictionary row constraint is used to discard input rows.
- Test LIMIT without touching a later invalid file, EXPLAIN without reading
  contents, missing categories/columns, schema errors and retained string chunks.
- SQL tests pass (591 assertions), C++ tests pass (212 assertions), and three
  offline dictionary-generator tests pass. Repository formatting checks pass.

Use the opt-in argument:

```sql
SELECT filename, id, Cartn_x
FROM mmcif_scan('/home/verhoes/.cache/protein-quest/**/*.cif.gz', 'atom_site',
                column_source := 'dictionary')
LIMIT 1;
```

`column_source := 'files'` remains the default and discovers the union from
input files. Dictionary mode exposes complete bundled category definitions,
including 61 previously omitted valid items restored by fixing the generator.
The source dictionary is version 5.417 and checksum-pinned; ordinary builds
use checked-in artifacts and never download it. This does not depend on the
proposed `is_single_row` or `is_mandatory` columns.

### Measured results

Same 6,397-path glob, two fresh processes per query, `SET threads = 1`, warm
filesystem caches that were not flushed, DuckDB v1.5.4 release build. Wall time
is the median for the entire process, measured with Python's monotonic clock;
peak RSS is the maximum across both repetitions, from `/usr/bin/time -f %M`.
Baseline and new measurements were taken sequentially on the same machine.

| Query | Default file discovery | Dictionary columns and lazy readers |
| --- | --- | --- |
| Coordinate `LIMIT 1` wall time | 36.03 s | 0.515 s |
| Coordinate `LIMIT 1` peak RSS | 6.21 GiB | 34.7 MiB |
| Full coordinate aggregate wall time | 56.06 s | 53.05 s |
| Full coordinate aggregate peak RSS | 6.22 GiB | 96.1 MiB |

The full aggregate is:

```sql
SET threads = 1;
SELECT count(*) AS atoms, count(Cartn_x) AS valid_x,
       sum(Cartn_x) AS sum_x, count(DISTINCT filename) AS contributing_files
FROM mmcif_scan('/home/verhoes/.cache/protein-quest/**/*.cif.gz', 'atom_site',
                column_source := 'dictionary');
```

Both runs exactly match the baseline: `atoms = valid_x = 49203033`,
`sum_x = 903837555.1229798`, and `contributing_files = 6397`.
The LIMIT result also matches (`31.789, -12.828, -0.907` when selecting all
three coordinates). LIMIT startup is about 70 times faster; full-scan peak
RSS falls about 98.5%. Full-scan time improves only about 5%, since dictionary
mode still decompresses and indexes every input file for a complete aggregate.

Remaining limitations: glob expansion enumerates all paths before execution;
readers are sequential; each active file is fully decompressed and indexed;
multiple category scans reread files independently. Query operators or retained
results can consume additional DuckDB memory. This benchmark measures an
aggregate, not collecting every atom row in a client. Improvements 2–7 remain
follow-up work.


### Scan progress feedback

`mmcif_scan` and attached readers register DuckDB's `table_scan_progress`
callback. An atomic completed-file counter supports concurrent polling without
accessing mutable reader state, reading ahead, or counting rows. Dictionary
mode includes inputs with missing categories and empty loops; file-discovery
mode counts the category readers retained after binding. A single-file reader
reports completion after its scan ends. Files receive equal weight.
A file-count row estimate gives large collection scans appropriate weight in
DuckDB's query progress instead of the default estimate of one row for the
entire collection. This is an estimate, not a maximum row count, and requires
no file reads.

Enable terminal feedback with `SET enable_progress_bar = true` and
`SET progress_bar_time = 1000`. Binding still has no progress feedback.
Regression tests exercise initial, partial-file, skipped-file and completed
states across dictionary, file-discovery and attached readers.

The terminal bar was tested in a pseudo-terminal against all 6,397 files with
`count(*)`, with progress enabled and a 1,000 ms display threshold. In both
single-scan thread configurations it first appeared after approximately 1.5 s,
advanced through 1–99%, and cleared when the result was printed. DuckDB's shell
clears the completed bar rather than leaving a literal 100% frame on screen.
The callback's 100% completion state is covered by the C++ regression tests.

### Counting with six CPU cores

On the same Ryzen 5 5600G host with six available cores, two fresh processes
per case, warm filesystem caches, dictionary column mode and terminal progress
enabled:

| Query arrangement | DuckDB threads | Median wall time | Peak RSS |
| --- | --- | --- | --- |
| One `mmcif_scan` over the full glob | 1 | 51.28 s | 96.4 MiB |
| One `mmcif_scan` over the full glob | 6 | 51.15 s | 96.6 MiB |
| Six disjoint scans, `UNION ALL` their counts and sum | 6 | 9.29 s | 276.3 MiB |

Every run returns exactly 49,203,033 atoms across all 6,397 inputs. Single scans
consume about one core because `MmcifGlobalState::MaxThreads()` is still 1.
Six independent scan pipelines consume about 5.7 cores on average and give a
5.5-fold speedup, at the cost of several active file buffers.
For the six-scan benchmark, files were sorted by decompressed size and assigned
to the currently smallest batch, yielding approximately 971 MB and 1,066–1,067
files per batch. Execution still uses the existing sequential reader in each
branch; internal parallel reading from one function remains improvement 6.

[docs/examples/count_parallel.sql](docs/examples/count_parallel.sql) provides a
runnable six-scan query using interleaved path lists instead of a size-based
partition. Its input glob should match at least six files. Progress is visible
for these concurrent scans too; equal file weights make percentages and ETA
approximate, particularly when larger files are processed first.

The interleaved example was also run directly against the real 6,397-file glob:
10.21 s process wall time, 177.2 MiB peak RSS, and the same 49,203,033 atoms
(one fresh-process run). A smaller fixture run returned the expected 24 atoms.
In the CLI's standard non-TTY display this run printed a final 100% bar;
the interactive shell display clears its final bar instead.
