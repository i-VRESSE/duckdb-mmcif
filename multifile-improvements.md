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
- SQL tests pass (591 assertions), C++ tests pass (181 assertions), and three
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
