# mmcif DuckDB extension — wayfinder map

Label: wayfinder:map

## Destination

A working, loadable DuckDB extension named `mmcif` that ATTACHes a cif file and exposes each mmcif category present in the file as a normal DuckDB table — read-only v1 — with columns typed from the `mmcif_pdbx_v50.dic` dictionary (precomputed type index), plus `mmcif_relationships(file)` and `mmcif_tables(file)` metadata tables filtered to tables present in the file.

## Notes

- Domain: PDBx/mmCIF data files + DuckDB extension API. Consult `docs/agents/domain.md` (CONTEXT.md/adr absent — proceed silently).
- Skills every session should consult: grilling, research, prototype, implement, tdd, to-spec.
- Standing preferences: read-only v1; RCSB C++ mmcif library as git submodules; ATTACH-style API with explicit db name (`AS`); precomputed dictionary type index; no enforced foreign keys.

## Decisions so far

- [Implement the mmcif ATTACH extension type](issues/02-attach-type.md) : DuckDB exposes `TYPE mmcif` via `StorageExtension::Register` (attach+create_transaction_manager callbacks); attached category tables are real `TableCatalogEntry`s surfaced via a custom Catalog/SchemaEntry `Scan(TABLE_ENTRY)` (sqlite model); `mmcif_relationships(file)`/`mmcif_tables(file)` are global table functions registered on Load; DESCRIBE types come from the entry `ColumnList` (tables) or bind `return_types` (functions), both fed from the dictionary type index. Findings: `.scratch/mmcif-extension/research/02-attach-type.md`.
- [Wire the RCSB C++ mmcif lib](issues/01-wire-rcsb-lib.md) : submodule the 5-repo minimal set `cpp-cif-file`, `cpp-cif-parser`, `cpp-common`, `cpp-tables`, `cc-regex`; flex/bison generation inside CMake (CI already installs the tools); build as an OBJECT lib `mmciflib` compiled into both static+loadable extension targets (linking it fails install/EXPORT — OBJECT lib required); `cpp-common/src` must be on the include path (`mapped_vector.C`); no vcpkg additions; DuckDB builds at C++11 so no flag conflict. Findings: `.scratch/mmcif-extension/research/01-wire-rcsb-lib.md`.
- [Dictionary type index](issues/03-dict-type-index.md) : source the pinned `mmcif_pdbx_v50.dic` (v5.416, ~5.9 MB / 0.6 MB gz, CC0-ish public data) off-repo; extract `_category.item → type` from `_pdbx_item_type.code` (override) then `_item_type.code`; checked-in gzip'd sorted TSV artifact (~40 KB) shipped with the extension; full mapping table (float→DOUBLE, int→BIGINT, everything else→VARCHAR, unknown→VARCHAR, `?`/`.`→NULL). Findings: `.scratch/mmcif-extension/research/03-dict-type-index.md`.
- [Data-model mapping](issues/04-data-model-mapping.md) : parse with CifFile/CifParser; only the first `data_` block (warn if `GetNumBlocks()>1`); one DuckDB table per category, columns = item suffixes (always double-quoted); non-loop pairs → 1-row tables, loop_ → N-row; map `"?"`/`"."`/`""` → NULL. Findings: `.scratch/mmcif-extension/research/04-data-model-mapping.md`.
- [Prototype: build + ATTACH feasibility](issues/05-prototype-build-attach.md) : Q1 YES (RCSB lib builds in DuckDB ext build); Q2 PARTIAL (mmcif_scan table function works fully; ATTACH requires a real custom Catalog subclass — DuckCatalog/DefaultGenerator fails for non-DuckDB files). Parser quirks: last-loop-not-flushed (append dummy trailing `data_zzz_prototype` block + ParseString), table names drop leading `_`, use `CifFile(true)` virtual-mode ctor. Prototype assets: `src/mmcif_prototype.cpp`, `CMakeLists.txt`, `modules/`, `.gitmodules`.

## Not yet specified

- None — every decision is resolved and the route is clear. The remaining work is the implementation effort (convert the throwaway prototype into the real extension), not a wayfinding decision.

## Out of scope

- Writing back to cif files (add struct ref, drop chain, rotate atoms): future possibility only, not v1. See ticket 05-future-write.md (closed).
- Enforced foreign keys: v1 surfaces relationships as a `mmcif_relationships` table, not constraints.
