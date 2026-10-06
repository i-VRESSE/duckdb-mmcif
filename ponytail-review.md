# Ponytail review: simplifications, improvements, dead code

Review date: 2026-10-06. No code was changed. Items are ordered by payoff.

## 1. Dead code: the regenerating writer and the comment tracking (≈900 lines) — DONE

[`MmcifIndex::Materialize`](src/mmcif_write_store.cpp) is the only way a write store gets created, and it always calls `SetSource`. So `HasSource()` is always true, and the fallback branch in `MmcifRenderStore` ([mmcif_file.cpp](src/mmcif_file.cpp)) that calls `MmcifWriteCif` never runs. The patch-based write-back already keeps comments byte-for-byte, so nothing else needs the comment table.

Delete:

- [mmcif_writer.cpp](src/mmcif_writer.cpp) and [mmcif_writer.hpp](src/include/mmcif_writer.hpp) (490 lines).
- [mmcif_comments.hpp](src/include/mmcif_comments.hpp), plus in `MmcifIndex::Build` the `pending`, `attach_comments` and `loop_row_at` code and the `counted_*` state (about 70 lines). The `#` branch then just skips the line.
- `MmcifIndex::comments` / `GetComments` and `MmcifWriteStore::comments`.
- `has_unrepresentable_content`, `MarkUnrepresentableContent` and `HasUnrepresentableContent`, and both `HasUnrepresentableContent() && !HasSource()` checks in [mmcif_catalog.cpp](src/mmcif_catalog.cpp). They are always false.
- `HasSource()`, the `!src` check in `MmcifPatch::Apply`, and the null checks in the `Source*()` getters. Make the source a required constructor argument instead.
- `has_multiple_blocks` / `has_save_frames` and their getters, since only the dead flag reads them.
- About 330 lines of `[writer]` and `[comments]` unit tests in [mmcif_unit_tests.cpp](test/cpp/mmcif_unit_tests.cpp), and the paragraph about the writer in [mmcif_patch.hpp](src/include/mmcif_patch.hpp).

## 2. Smaller dead code — DONE

- **`MmcifIndex::raw`**: never read. Its comment says it is "kept for cache invalidation checks", but nothing uses it, and it keeps an extra copy of every gzip file in memory.
- **`MmcifCategory::is_loop_col`**: written, never read.
- **Unused accessors**: `MmcifIndex::GetDataSize` is never called. `MmcifWriteStore::GetNumRows` and `GetRow` are only used by tests and just wrap the public `rows` field.
- **`content_data` / `content_size`**: they duplicate `text.data()` / `text.size()`.
- **Unused parameter**: `MmcifLoadIndex(…, file_name)` in [mmcif_table_functions.cpp](src/mmcif_table_functions.cpp) never uses it.
- **`MmcifWriteGlobalState::lock`**: all three operators set `ParallelSink() == false`, so the lock does nothing.
- **Leftover files**: [concurrent-write-issues.md](concurrent-write-issues.md) says "TODO remove this file before PR is merged". There are also untracked `*.cif(.gz)` files at the repo root.

## 3. Simplifications — DONE

- **DML operators** ([mmcif_catalog.cpp](src/mmcif_catalog.cpp)): Insert, Delete and Update each repeat the same eight overrides (`IsSink`, `ParallelSink`, `SinkOrderDependent`, `IsSource`, `GetGlobalSinkState`, `GetLocalSinkState`, `GetGlobalSourceState`, `GetDataInternal`). One `MmcifWriteOperator` base class saves about 80 lines.
- **`UpdateCell`**: it takes a column name and looks up its index with a case-insensitive search, but the caller already has the index. It also copies `cat->columns` on every chunk. Pass `idx_t col` directly.
- **`Build()`** ([mmcif_index.cpp](src/mmcif_index.cpp)):
  - `if (state == LOOP_DATA && cur) cur->data_end = line_start;` appears five times, so make it an `end_loop()` lambda.
  - The "find or add column" loop appears twice, so make it a helper.
  - The hand-written `;…;` scanner for single-tag values copies what `MmcifValueCursor` already does, so call the cursor on `[val_start, size)` instead.
- **`Materialize()`**: the code that flushes a partial last row is duplicated; one lambda covers both.
- **Index cache** (the context-based stamp was dropped together with the section 4 fix) ([mmcif_index.cpp](src/mmcif_index.cpp)):
  - Two maps with two mutexes (`g_index_cache` and `g_stamps`) can become one `map<path, {weak_ptr, stamp}>` with one lock.
  - Drop the context-based stamp and always use `MmcifLocalFileStamp`. Remote paths never reach this code anyway. The context-based stamp is the weaker one: it looks only at mtime and size, so it misses the same-size rewrite that the other stamp was written to catch.
  - The stamp string also doesn't need to include the path, since the path is already the map key.
- **`GetRowCount`**: drop the mutex double-check. Counting is idempotent, so the atomic store alone is enough.
- **`MmcifFile::Persist` for plain files** ([mmcif_file.cpp](src/mmcif_file.cpp)): it uses `std::ofstream` while the `.gz` branch uses DuckDB's file system. Use `fs.OpenFile` without compression for both, which removes about 12 lines and the Windows binary-mode workaround.
- **`MmcifBindData::Copy` / `MmcifMetaBindData::Copy`**: each can be `return make_uniq<X>(*this);`.
- **Metadata bind functions**: replace the runs of `names.emplace_back` / `return_types.push_back` with `names = {…}; return_types = {…};`.
- **Duplicate read-only bind**: the read-only branch of `MmcifTableEntry::GetScanFunction` repeats `MmcifLoadIndex` and the type lookup, so call the existing code.
- **`DeleteRows`**: it guards `r < row_spans.size()` and `r < cell_spans.size()`, but these vectors always have the same length as `rows`. Drop the guards, or keep one `D_ASSERT`.

## 4. Possible bug — DONE

A write-mode attach passes `nullptr` as the context (`MmcifCatalog` constructor), so it goes through the `ifstream` fallback in `MmcifFile::Read`, which returns empty content for a missing file. If that reading is right, `ATTACH 'missing.cif' (READ_WRITE TRUE)` succeeds with an empty database instead of failing. `MmcifAttach` has a context available, so it could pass it to the catalog. Worth testing before relying on it.

## 5. Comment cleanup — DONE

- Many header comments refer to things no longer in the repo: "RCSB", "recommendation 4 & 5", "issue 03", "D3/D5/D6", `big-pdb-too-slow.md`, `.scratch/mmcif-extension/map.md`.
- [CMakeLists.txt](CMakeLists.txt) still has the template text "Feel free to remove…".
- The target name `mmciff_dict_data` has a typo (double f).

## Suggested order

Sections 1 and 2 first: pure deletion, with the `[patch]` unit tests and [mmcif.test](test/sql/mmcif.test) still covering write-back.
