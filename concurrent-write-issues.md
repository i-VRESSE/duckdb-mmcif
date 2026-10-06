# Commit with two write-mode mmcif attachments — how it works and what breaks

Investigation date: 2026-10-02. All findings reproduced against `build/release/duckdb` (v1.5.4, extension built in) with minimal two-file fixtures.

## How commit works

1. DuckDB core runs every statement in a transaction (explicit `BEGIN` or auto-commit). A transaction lazily starts a per-database `Transaction` for every attached database it touches (`MetaTransaction::GetTransaction`, `duckdb/src/transaction/meta_transaction.cpp:56`).
2. `COMMIT` calls `MetaTransaction::Commit()` (`meta_transaction.cpp:108`), which walks the touched databases **in reverse usage order** and calls each one's `TransactionManager::CommitTransaction`.
3. For mmcif write-mode attachments, `MmcifTransactionManager::CommitTransaction` (`src/mmcif_catalog.cpp:660`) unconditionally calls `catalog.Persist(context)` → `MmcifFile::Persist` (`src/mmcif_file.cpp:74`), which writes the catalog's in-memory `MmcifWriteStore` back to the file (plain `std::ofstream` trunc, or gzip via DuckDB's FS with `WRITE|CREATE_NEW` = `O_CREAT|O_TRUNC`).

There is no dirty tracking: `CommitTransaction` persists whether or not the transaction modified anything, and whether the store was mutated by DML or merely materialized at attach.

## Confirmed problems

### 1. Any read-only query rewrites the file to disk (unconditional persist)

`MmcifTransactionManager::CommitTransaction` never checks `transaction.IsReadOnly()` or whether changes were made. Since every SELECT auto-commits, **every read-only query against a write-mode attachment rewrites the whole file**.

Repro: attach `a.cif` and `b.cif` with `READ_WRITE TRUE`, run two `SELECT count(*)`s. Both files' mtimes change and content is rewritten.

### 2. The rewrite is destructive — data loss on mere reads

The write store materializes only the **first data block** (`src/mmcif_index.cpp:134` "keep only the FIRST data block", `:313-318`), and `MmcifWriteCif` (`src/mmcif_writer.cpp:258`) re-emits only the store's categories with `writeEmptyTables=false`. Confirmed on a fixture containing a comment, a save frame, and a second `data_` block: after two read-only SELECTs, the file on disk had lost the comment, the save frame, and the entire second data block (`A1 9.9`), and was reformatted (`# ` separators, trailing spaces on tag lines).

So merely reading a write-mode file can permanently destroy content that the parser doesn't represent.

### 3. Commit errors on plain files are silent

`MmcifFile::Persist` for non-gzip paths uses `std::ofstream(path, out|trunc)` with **no failbit check** (`src/mmcif_file.cpp:94-96`). `std::ofstream` doesn't throw; writes to a failed stream are dropped and `close()` masks it. The gzip path uses DuckDB's `FileSystem::OpenFile`, which throws.

Repro: `chmod 444` a plain `.cif`, attach it `READ_WRITE`, `COMMIT` → exit 0, no error, file unchanged, **other** file's changes still persisted. Same setup on a `.cif.gz` → `TransactionContext Error: ... Permission denied` (error surfaced).

### 4. Multi-file COMMIT is not atomic

`MetaTransaction::Commit` commits databases in reverse usage order and, on any error, "rolls back" the remaining ones via `RollbackTransaction` → `ReloadFromDisk` (`src/mmcif_catalog.cpp:668`). For mmcif, `CommitTransaction` already persisted earlier databases to disk, so `ReloadFromDisk` (re-parse from disk) **keeps** those persisted changes instead of discarding them.

Repro: touch `wb` (gzip, chmod 444) first, then INSERT into `wa` (plain), `COMMIT` → overall commit errors (`Permission denied`), but `wa`'s file still contains the inserted row on disk. A failed COMMIT leaves partial changes persisted.

### 5. Stale index cache + unconditional persist = committed data silently erased by a read

The process-level index cache (`src/mmcif_index.cpp:19`) is keyed by path. Its staleness check is **skipped when context is null** (`MmcifFileChanged`, `mmcif_index.cpp:22-27` returns false), and write-mode loads always pass `nullptr` (`LoadWriteStore` from the catalog constructor and `ReloadFromDisk`). `Persist` never invalidates the cache either.

Repro (confirmed): attach `ro` (read-only) and scan it so its `MmcifIndex` stays alive in the cache; attach `wdb` (write-mode, materialized from the cached original), INSERT `Z9`, COMMIT (persisted to disk), `DETACH`, re-ATTACH write-mode `wdb2` → `LoadWriteStore` gets the **stale cached index** (Z9 lost from the in-memory store), then a plain `SELECT` from `wdb2` auto-commits and **persists the stale store over the good file — the committed Z9 is erased from disk**. Data loss from a read-only SELECT.

### 6. Two write-mode files cannot be mutated in one transaction (DuckDB core rule)

`MetaTransaction::ModifyDatabase` (`meta_transaction.cpp:238-262`) throws if a transaction writes to two different attached databases:

```
TransactionContext Error: Attempting to write to database "wb" in a transaction that
has already modified database "wa" - a single transaction can only write to a single
attached database.
```

So with two write-mode mmcif files, each transaction can only INSERT/UPDATE/DELETE one of them — yet COMMIT still rewrites both (problems 1–2), including the untouched one.

### 7. Related smells

- **DETACH also persists unconditionally** (`OnDetach`, `mmcif_catalog.cpp:525-527`) and `CHECKPOINT` persists unconditionally (`mmcif_catalog.cpp:675`) — same pattern as COMMIT, same silent-destructive behavior.
- **No per-transaction isolation**: `write_store` is one shared mutable object per catalog; DML mutates it immediately, and there is no snapshotting. Concurrent write-mode connections to the same file race (last commit wins); the `Transaction` object carries no store state.
- **Non-atomic single-file writes**: `Persist` truncates then writes in place (no temp-file + rename); a crash mid-write leaves a truncated/corrupt `.cif`.
- **Rollback works only when the cache is cold**: `ReloadFromDisk` trusts the stale cache (same `nullptr` context path as #5), so a rollback can materialize content that doesn't match the on-disk committed state if another catalog keeps the index alive.

## Suggested fixes (in rough priority)

1. Only persist when the transaction actually modified the store: track a dirty flag on `MmcifWriteStore` (set by AddRow/DeleteRows/UpdateCell), and have `CommitTransaction` check `transaction.IsReadOnly()` / store-dirty before calling `Persist`. This alone fixes #1, #2, and #5 (no rewrite unless a real write happened).
2. Make `Persist` lossless / non-destructive or reject round-trips it can't preserve: keep comments and save frames, or at minimum detect multi-block files and refuse write-back instead of silently dropping data blocks.
3. Check `ofs.fail()` / use DuckDB's FS for the plain path so commit failures surface (#3).
4. Make `Persist` atomic per file (temp file + rename).
5. Fix the stale-cache path: pass a `ClientContext` (or a context-less mtime/size stat) to `LoadWriteStore` / `ReloadFromDisk`, and invalidate the cache entry after `Persist`.
6. If multi-file atomic commit is desired, buffer all persists and write only after every file validates (e.g., write to temp files, then rename all); or document that COMMIT is per-file atomic only.

## Repros

```bash
# Problem 1/2: two read-only SELECTs rewrite + destroy file content
./build/release/duckdb -c "
ATTACH '/tmp/a.cif' AS wa (TYPE mmcif, READ_WRITE TRUE);
ATTACH '/tmp/b.cif' AS wb (TYPE mmcif, READ_WRITE TRUE);
SELECT count(*) FROM wa.atom_site;
SELECT count(*) FROM wb.atom_site;"

# Problem 3/4: silent plain-file persist failure / non-atomic gzip failure
chmod 444 b.cif   # plain: COMMIT exits 0, no error, b unchanged, a still written
chmod 444 b.cif.gz # gzip: COMMIT errors, but a.cif changes remain on disk

# Problem 5: committed row erased by a later read-only SELECT
./build/release/duckdb -c "
ATTACH '/tmp/b.cif' AS ro (TYPE mmcif);
SELECT count(*) FROM ro.atom_site;                      -- warm shared index cache
ATTACH '/tmp/b.cif' AS wdb (TYPE mmcif, READ_WRITE TRUE);
BEGIN; INSERT INTO wdb.atom_site (...) VALUES ('Z9', 6.6); COMMIT;
DETACH wdb;
ATTACH '/tmp/b.cif' AS wdb2 (TYPE mmcif, READ_WRITE TRUE);
SELECT label_atom_id FROM wdb2.atom_site;               -- stale store; next auto-commit
                                                       -- writes it over the good file
SELECT content FROM read_blob('/tmp/b.cif');            -- Z9 gone from disk"

# Problem 6: cross-file write in one transaction rejected by core
BEGIN; INSERT INTO wa.atom_site (...) VALUES ('X1', 7.7);
INSERT INTO wb.atom_site (...) VALUES ('Y1', 8.8); COMMIT;
```

## Resolution (2026-10-02)

Implemented fixes:

- **#1 + #7 (rewrite on read-only / DETACH / CHECKPOINT)**: `MmcifWriteStore` now tracks a `dirty` flag, set by `AddRow`/`DeleteRows`/`UpdateCell`. `MmcifCatalog::Persist` skips the write-back unless the store is dirty and clears it after a successful persist, so read-only transactions, no-op commits, DETACH, and CHECKPOINT never rewrite the file.
- **#2 (destructive rewrite)**: the parser now records `has_multiple_blocks` / `has_save_frames` (`mmcif_index.cpp`), `Materialize()` copies that onto the store, and write-mode attach (`MmcifCatalog` ctor) throws for such files instead of silently dropping whole blocks on commit. Comments are still dropped (cosmetic). The detection ignores the parser's synthetic `data_zzz_prototype` flush block (`original_text_size` bounds the check), so clean single-block files are unaffected.
- **#3 (silent failures)**: `MmcifFile::Persist` now checks the `ofstream` state and throws `IOException` on open/write failure; the gzip path already surfaced errors via DuckDB's FS. Verified: a read-only *directory* makes COMMIT fail with `cannot open '...tmp' for writing`.
- **#4 (single-file atomicity)**: `Persist` writes to a `<path>.tmp` in the same directory, then `FileSystem::MoveFile` (rename) over the target, with temp cleanup on failure. A crash or failed write no longer leaves a truncated `.cif`. Side effect: because rename needs only directory-write permission, a read-only *target* file is replaced anyway when its directory is writable (verified: chmod 444 `b.cif.gz` -> COMMIT succeeds, file becomes 644).
- **Windows CRLF in the write-back** — fixed 2026-10-05: `MmcifFile::Persist` opened the plain-path `std::ofstream` in *text* mode, so on Windows every `\n` was translated to `\r\n` and the rewritten `.cif` carried carriage returns (caught by `test/sql/mmcif.test:503` on the Windows CI job: splitting the file on `E'\n'` left a `\r` on every line). The stream is now opened with `std::ios::binary`, matching the read paths. The gzip path was never affected (it goes through DuckDB's FS).
- **#5 (stale cache erasing committed data)**: successful `Persist` now calls `MmcifIndex::InvalidateCache(path)` (drops the index + stamp entries), `ReloadFromDisk` passes the transaction's `ClientContext` so the staleness check runs, and the context-free staleness check now stats local files instead of always returning "unchanged". Fixed 2026-10-06: that stat was size-only, so a same-size external rewrite let a cached index be reused, and stamps were only recorded on the first cache hit (so any rewrite after it went unnoticed). `MmcifIndex::Load` now records the stamp when it loads (taken before the read), and the context-free stamp is identity + size + sub-second mtime + change time (`stat` `dev/ino/mtim/ctim` on POSIX, volume/file index + `LastWriteTime`/`ChangeTime` on Windows), so same-size rewrites, restored mtimes, and atomic renames over the path all invalidate it (`[cache]` unit test).

Remaining limitations (DuckDB core / design):

- **Cross-file commit atomicity (#4)**: DuckDB core commits attached databases sequentially in reverse usage order, and each mmcif catalog persists independently. In practice this is now unreachable: dirty tracking means only a database the transaction actually wrote to persists, and DuckDB core already rejects writing two attached databases in one transaction (#6), so at most one file is written per COMMIT. If core ever allowed multi-database writes, a later persist failure could still leave an earlier file's changes on disk.
- **Comment loss (#2)** — fixed 2026-10-05: the parser now records every `#` line in a comments table (`MmcifComment` / `MmcifCommentAnchor` in `src/include/mmcif_comments.hpp`), each with a structural anchor: block header (above `data_`), category (its `loop_` keyword or first tag), single item, loop row, or file trailer. `MmcifIndex::Materialize()` copies it into `MmcifWriteStore::comments` and `MmcifWriteCif` re-emits each line at its anchor. Anchors are structural rather than line numbers because DML changes how many lines a category writes back. A category with no retained comments still gets the generated `# ` separator, so comment-free files emit exactly as before. Verified on `3PLZ.cif`: all 74 comment lines come back byte-identical after an INSERT + COMMIT.
  - Related fix found on the way: a comment inside loop data used to end `LOOP_DATA` in `MmcifIndex::Build()`, so every row after it was dropped from the index — visible on a plain read-only scan, and permanent once written back. Comments no longer end a loop (`MmcifValueCursor` skips them at token boundaries) and the comment is anchored to the row that followed it. Comments anchored to since-deleted rows trail the loop; comments of a category committed empty are written away with it.
- **#6 (two files in one transaction)**: the "single transaction can only write to a single attached database" rule is DuckDB core behavior and is left as-is (verified: the INSERT into the second file fails at statement time, before COMMIT).

## Remaining work (re-verified 2026-10-05)

Every repro above was re-run against `505a1d7` ("Retain comments when writing").
The confirmed problems all now behave as described in the Resolution: the dirty
flag gates the write-back, `3PLZ.cif` round-trips 74/74 `#` lines byte-identical
after an INSERT + COMMIT, the stale-cache repro keeps `Z9` on disk, and a
`DETACH` inside an open transaction is refused by core ("outstanding work")
before any write-back happens. Items 1-7 below are what is still open, in rough
priority.

### 1. Concurrent writes to the same file still collide (the headline case)

`Persist` uses a deterministic temp name — `path + ".tmp"` (`src/mmcif_file.cpp:83`) —
and nothing locks the data file (no `flock` / `FileSystem::TryLockFile` anywhere
in `src/`). Two processes committing the same `.cif` collide on that temp file;
4/4 runs of the race produced:

```
TransactionContext Error: Failed to commit: Could not rename file "/tmp/race.cif.tmp"
to "/tmp/race.cif": No such file or directory
```

The losing writer's rows are gone, and the error names a temp file instead of
saying "another writer holds this file". With different timing there is no error
at all: last writer wins and the other commit's rows vanish silently.

Needs: a unique temp name (pid + counter/random suffix) *and* an exclusive lock
taken at write-mode attach (or at first persist), so contention surfaces as
contention instead of a rename failure or a silent overwrite.

```bash
# two processes, same file, both commit
cp 3PLZ.cif race.cif
for i in 1 2; do
  ./build/release/duckdb -c "ATTACH '$PWD/race.cif' AS w (TYPE mmcif, READ_WRITE TRUE);
    INSERT INTO w.atom_site (label_atom_id, Cartn_x) VALUES ('RACE$i', $i.5);" &
done; wait
grep -c RACE race.cif   # 1, not 2
```

### 2. Same path attached twice READ_WRITE loses one writer silently

Each `ATTACH` builds its own `MmcifWriteStore` from the file as it was at attach
time, and each catalog persists its own store on COMMIT. Nothing detects that two
write-mode catalogs point at the same path, so the second persist overwrites the
first one's committed rows with no error and exit code 0:

```sql
ATTACH 'a.cif' AS w1 (TYPE mmcif, READ_WRITE TRUE);
ATTACH 'a.cif' AS w2 (TYPE mmcif, READ_WRITE TRUE);
INSERT INTO w1.atom_site VALUES (10,'CA',9.9);   -- silently lost
INSERT INTO w2.atom_site VALUES (20,'CB',8.8);   -- only this row lands
```

Needs: reject a second write-mode attach of an already-write-attached resolved
path (compare `RealPath`), or share one catalog/store between the aliases.

### 3. No per-transaction isolation (problem 7 smell, untouched)

`write_store` is one shared mutable object per catalog with no mutex
(`src/include/mmcif_write_store.hpp:37-80`), and the `Transaction` object carries
no store state (`MmcifTransactionManager::StartTransaction`,
`src/mmcif_catalog.cpp:668`). Consequences:

- uncommitted DML from one connection is visible to every other connection
  immediately (no snapshot isolation);
- `RollbackTransaction` → `ReloadFromDisk` (`src/mmcif_catalog.cpp:684-692`)
  replaces the *whole* store, so rolling back in one connection discards another
  connection's uncommitted work;
- concurrent DML from two threads mutates the same `std::vector`s unsynchronised
  — a plain data race.

Needs: per-transaction snapshots of the mutated categories (copy-on-write, apply
on commit), or at minimum a catalog-level lock plus documented
one-writer-per-file-per-session semantics.

### 4. The rename does not preserve the target's identity or permissions

Because the write-back is temp + `MoveFile`, the target's metadata is not carried
over:

| Case | Observed after COMMIT |
| --- | --- |
| target mode `600` | becomes `644` (widened) |
| target `chmod 444`, writable dir | overwritten anyway, becomes `644` (plain `.cif` behaves like the `.gz` case noted above) |
| attach path is a symlink | the **symlink is replaced by a regular file**; the real target is left untouched |

Needs: copy mode (and ownership where permitted) from the old file before the
rename, decide whether the target being read-only should be an error, and resolve
symlinks (`RealPath`) at attach so the link keeps pointing at live data.

### 5. Round-trip fidelity gaps beyond comments

- **Multi-block / save-frame files are refused, not supported.** The "keep them"
  option from fix #2 is still open; today such files cannot be opened READ_WRITE at
  all.
- **CRLF input comes back mixed.** A `\r\n` file is written back with `\r` only on
  the retained comment lines and `\n` on everything the writer generates.
- **Read/write value asymmetry.** The read path returns raw tokens
  (`"abc def"`, `_`) while the write path decodes and re-quotes, so after a
  write-back the same cells read as `'abc def'` and `'_'` — the strings a user
  sees change under them:

  | cell in original file | read before write-back | read after write-back |
  | --- | --- | --- |
  | `"abc def"` | `"abc def"` | `'abc def'` |
  | `_` | `_` | `'_'` |
  | `;\nmulti line\nvalue\n;` | unchanged | unchanged |

  Needs: either decode on the read path too (so both sides agree on the logical
  value), or emit the store's stored form verbatim.

### 6. Cross-file commit atomicity (problem 4) is still documentation-only

Unchanged from the Resolution: harmless today because dirty tracking plus core's
single-database-write rule means at most one file is written per COMMIT, but if
core ever relaxes, a later persist failure can still leave an earlier file's
changes on disk. Implement (write all temp files, then rename all) only if
multi-database transactions become possible.

### 7. Test coverage gaps

`test/sql/mmcif.test` and `test/cpp/mmcif_unit_tests.cpp` cover the writer,
comment anchors, rollback and the gzip round-trip well. Nothing yet covers:

- two-process / two-connection writes to the same file (items 1 and 3),
- the same-path double `READ_WRITE` attach (item 2),
- permission and symlink preservation across a write-back (item 4),
- CRLF input round-trip, and the quoting asymmetry (item 5).
