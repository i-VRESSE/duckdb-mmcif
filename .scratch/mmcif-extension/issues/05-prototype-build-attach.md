# Prototype: RCSB library build + ATTACH feasibility

Type: prototype
Status: resolved
Blocked by: 01, 02

## Question

Before committing the route, verify the two biggest risks with a cheap, rough prototype: (1) does the RCSB C++ mmcif library build cleanly inside the DuckDB extension build (flex/bison, CMake wiring), and (2) does an ATTACH-style custom extension type actually expose tables such that `USE`, `SHOW TABLES`, and `.schema` see them? Produce a minimal working slice (e.g. `mmcif('1amb_updated.cif', '_atom_site')` or a stub ATTACH) to react to, and report what works and what blocks.

## Answer

Resolved via the throwaway prototype. Both risks were probed; the verdicts:

**Q1 — RCSB C++ mmcif lib builds cleanly inside the DuckDB extension build: YES.**
Wired flex/bison + CMake into `CMakeLists.txt` as an OBJECT lib `mmciflib`
(flex_target/bison_target for `CifScanner.l`/`DICScanner.l`/`CifParser.y`/`DICParser.y`,
`CXX_STANDARD 11`, PIC ON). It compiles cleanly — only `std::binary_function`/
`std::unary_function` deprecation warnings from `cpp-common/include/GenString.h`.
Two gotchas: (a) `mmciflib` must be an OBJECT lib compiled into both extension targets —
linking it into the extension fails `install(EXPORT DuckDBExports)` because it carries
build-dir include paths; (b) RCSB sources include `mapped_vector.C` so `cpp-common/src`
must be on the include path.

**Q2 — ATTACH-style custom type exposes tables: PARTIAL / BLOCKED.**
A table-function slice works fully and generically:
`SELECT * FROM mmcif_scan('/abs/1amb_updated.cif', 'atom_site')` returns 438 rows x 21
cols for `atom_site`, and works for other categories (`pdbx_poly_seq_scheme`, `struct`);
`?`/`.` cells map to NULL. This proves the lib parse + column/row extraction path end to
end. But the cheap ATTACH shortcut (StorageExtension::Register("mmcif", ...) whose attach
returns a `DuckCatalog` + `DuckTransactionManager` + DefaultGenerator) does NOT work for a
non-DuckDB file: `ATTACH '1amb_updated.cif' AS db (TYPE mmcif)` fails with
`The file ... is not a valid DuckDB database file!` because DuckCatalog's storage
(DuckDBStorageManager) tries to open the cif as a DuckDB database. So ATTACH needs a real
custom `Catalog` subclass (SQLite-style `SQLiteCatalog` + custom TransactionManager), not a
reuse of DuckCatalog — that is the remaining work.

Key parser quirk (must be handled): the RCSB parser never flushes a loop that is the LAST
loop in a file (here `atom_site`), so it must be forced by appending a dummy trailing
`data_zzz_prototype` block and using `ParseString`; the first data block is then kept and
the dummy ignored. Also: table names are stored WITHOUT the leading `_` (category
`_atom_site` is table `atom_site`), and the `CifFile(bool)` virtual-mode constructor (no
sdb file) must be used explicitly (`make_uniq<CifFile>(true)`), not the create-mode ctor.

Prototype assets: `src/mmcif_prototype.cpp` (mmcif_scan + ATTACH stub), `CMakeLists.txt`,
`modules/` (5 RCSB submodules), `.gitmodules`. Throwaway — do not promote to the real route
as-is.
