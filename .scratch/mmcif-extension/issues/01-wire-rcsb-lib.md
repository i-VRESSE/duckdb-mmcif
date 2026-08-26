# Wire the RCSB C++ mmcif library into the DuckDB extension build

Type: research
Status: resolved
Blocked by:

## Question

How should we integrate the RCSB C++ mmcif library into the DuckDB extension build? We decided to submodule it (py-mmcif's underlying C++ core: `cpp-cif-file`, `cpp-cif-parser`, `cpp-common`, `cpp-tables`, `cc-regex`, `cpp-cif-file-util`), but the route needs the concrete integration approach:

- Which submodules are actually required for read-only parsing of a cif data file (CifFile, tables, parser), and their transitive deps.
- How the parser build works: flex/bison generation (CifScanner.l / CifParser.y) — what tools/versions, whether it builds inside DuckDB's CMake/extension-config or needs a separate step.
- CMake wiring: how to add the library to `CMakeLists.txt` / `extension_config.cmake`, static vs loadable, vcpkg involvement, and any conflicts with DuckDB's C++17/build flags (the library is old C++11).
- License and provenance notes for vendoring/submoduling.

## Answer

Resolved. Full findings: `.scratch/mmcif-extension/research/01-wire-rcsb-lib.md` (gist: see below).

**Resolution summary**

- **Minimal submodule set (5 repos)** for read-only cif-data-file parsing: `cpp-cif-file`, `cpp-cif-parser`, `cpp-common`, `cpp-tables`, `cc-regex`. Deps: cif-file→common+tables+regex; cif-parser→common+tables; tables→common. `cpp-cif-file-util` and `cpp-dict-obj-file` are NOT needed for data-file parsing; the DIC/DicFile sources inside cif-parser/cif-file are dictionary-only and can be dropped (verified: data-file reader built from 30 objects, dropped `DicFile`+all `DIC*`).
- **Flex/bison generation runs inside CMake** (no separate step): py-mmcif uses `find_package(FLEX)`/`find_package(BISON)` + CMake's `flex_target`/`bison_target`/`add_flex_bison_dependency` macros (`CifScanner.l`→`CifScanner.c` with `-Cfr -L -Pcifparser_`; `CifParser.y`→`CifParser.c` with `-d -v -l -p cifparser_`). Modern flex 2.6.x and bison 3.x accept the flags (verified 2.6.4/3.8.2). flex+bison are already installed in DuckDB's extension CI images (`yum/apk` Linux, `brew` macOS, `winflexbison3` Windows).
- **CMake wiring**: add a static `mmciflib` target (all module sources) + include dirs (incl. `cpp-common/src` for the `#include "mapped_vector.C"` template pattern) + flex/bison custom commands, then `target_link_libraries(${EXTENSION_NAME} mmciflib)` and `${LOADABLE_EXTENSION_NAME} mmciflib`. Static is straightforward; the loadable target's hidden-visibility + `--exclude-libs,ALL` already internalizes mmcif symbols, and DuckDB's global `POSITION_INDEPENDENT_CODE ON` gives the needed `-fPIC`. **No vcpkg packages needed** — mmcif has no external deps beyond flex/bison (build tools, CI-installed).
- **C++ flag conflict is mostly a non-issue**: DuckDB's build sets `CMAKE_CXX_STANDARD 11` (duckdb/CMakeLists.txt:54), so the extension + mmcif lib compile at C++11 by default — the template's `set(... 17 CACHE)` is a no-op. py-mmcif's extra defines (`BIG_ENDIAN_PLATFORM` etc.) are vestigial. The one real C++17 blocker is `std::binary_function` in `GenString.h` (removed in C++17, only a warning at C++11); pin the mmcif target to C++11 if a user forces C++17. Upstream rejects MSVC (py-mmcif CMake: `FATAL_ERROR`); use mingw for Windows.
- **License/provenance**: py-mmcif (aggregator) is Apache-2.0; `cc-regex` is Henry Spencer's permissive regex; the C++ modules carry no LICENSE files (headers use `$$LICENSE$$` template markers). Record both in a NOTICE/THIRD-PARTY file on distribution.

Context pointer — findings file: `.scratch/mmcif-extension/research/01-wire-rcsb-lib.md` (this ticket's Answer is the gist; the file holds the full citation-backed detail).
