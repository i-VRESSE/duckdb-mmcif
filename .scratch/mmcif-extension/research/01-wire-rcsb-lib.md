# Research: Wiring the RCSB C++ mmcif library into the DuckDB extension build

Ticket: `.scratch/mmcif-extension/issues/01-wire-rcsb-lib.md`
Date: 2026-08-25
Method: cloned the RCSB C++ submodules (shallow) and py-mmcif, read primary sources, and ran a from-scratch compile+link+parse proof against a sample cif file. Every claim below cites the owning source.

## TL;DR / answer

1. **Minimal submodule set for read-only cif-data-file parsing = 5 repos**: `cpp-cif-file`, `cpp-cif-parser`, `cpp-common`, `cpp-tables`, `cc-regex`. `cpp-cif-file-util` and `cpp-dict-obj-file` are **not** needed for data-file parsing.
2. **Parser generation** happens **inside CMake** (no separate step): py-mmcif uses `find_package(FLEX)`/`find_package(BISON)` + the CMake `flex_target`/`bison_target`/`add_flex_bison_dependency` macros, which run flex/bison as build-time custom commands. Modern flex (2.6.x) and bison (3.x) accept the exact flags. flex and bison are **already installed in DuckDB's extension CI Docker images**.
3. **DuckDB's build is C++11, not C++17** (contrary to the ticket premise) — so the old C++11 library compiles cleanly. Only `std::binary_function` (removed in C++17) hard-blocks a forced C++17 build.
4. **License**: py-mmcif (the aggregator) is Apache-2.0; `cc-regex` is Henry Spencer's permissive regex; the C++ modules carry no LICENSE files.

---

## 1. Minimal submodule set and dependency graph

### Evidence
py-mmcif's `CMakeLists.txt` builds 7 object libraries and combines them into a static `mmciflib-all`:
`common`, `tables`, `regex`, `cif-file`, `cif-file-util`, `dict-obj-file`, `cif-parser`
(source: `rcsb/py-mmcif` `CMakeLists.txt`, `add_library("mmciflib-all" STATIC ...)`).

py-mmcif's `.gitmodules` lists 8 submodules:
`modules/cc-regex`, `modules/cpp-cif-file`, `modules/cpp-cif-file-util`, `modules/cpp-cif-parser`, `modules/cpp-common`, `modules/cpp-dict-obj-file`, `modules/cpp-tables`, `modules/pybind11`
(source: `rcsb/py-mmcif` `.gitmodules`).

### Include dependency graph (read from source `#include`s)
- `cpp-cif-file/src/CifFile.C` includes `GenString.h`, `RcsbFile.h`, `CifString.h` (common), `regex.h` (cc-regex), `CifParentChild.h`; `CifParentChild.C`/`ParentChild.C` add `GenCont.h` (common) and `ISTable.h`/`TableFile.h` (tables).
- `cpp-cif-parser/src/CifParserBase.C` includes `Exceptions.h`, `GenString.h`, `RcsbFile.h`, `CifString.h` (all common); `CifScannerBase.C` includes the bison-generated `CifParser.h`.
- `cpp-tables/src/*.C` include `Exceptions.h` + `GenString.h` (common).
- `cpp-common` and `cc-regex` have no deps (their Makefiles list empty `ALL_DEP_LIBS`).

Each module's legacy `Makefile` declares its own link deps:
- `cpp-cif-file/Makefile`: `ALL_DEP_LIBS = $(TABLES_LIB) $(REGEX_LIB)` → depends on tables + regex.
- `cpp-cif-parser/Makefile`: `ALL_DEP_LIBS = $(TABLES_LIB) $(COMMON_LIB)` → depends on tables + common.
- `cpp-tables/Makefile`: `ALL_DEP_LIBS = $(COMMON_LIB)` → depends on common.
- `cpp-common/Makefile` and `cc-regex/Makefile`: no deps.

### Transitive closure → 5 repos
```
cc-regex        (leaf, no deps)
cpp-common      (leaf, no deps)
cpp-tables      → cpp-common
cpp-cif-file    → cpp-common, cpp-tables, cc-regex
cpp-cif-parser  → cpp-common, cpp-tables
```
So read-only parsing of a cif **data** file needs exactly:
`cpp-cif-file`, `cpp-cif-parser`, `cpp-common`, `cpp-tables`, `cc-regex`.

### What is NOT needed
- `cpp-cif-file-util` (`CifFileUtil.C`/`CifCorrector.C`) — dictionary/file-util helpers, not data-file reading (`cpp-cif-file-util/src/CifFileUtil.C` includes `DicFile.h`, `DICParserBase.h`).
- `cpp-dict-obj-file` — dictionary object files only.
- Within `cpp-cif-file`/`cpp-cif-parser`, the dictionary sources `DicFile.C`, `DICParser.y/.l`, `DICScanner.y/.l`, `DIC*Base.C` are used only for dictionary parsing, **not** data-file reading. A from-scratch proof built the data-file reader from **30 objects** (dropping `DicFile.o` and all `DIC*` objects) and it still compiled, linked, and parsed a sample cif file (blocks=1, table `atom_site`). Keeping them (py-mmcif's full-module approach) is simpler and harmless.

### Verified build proof
Cloned all repos, generated `CifParser.c/h`, `CifScanner.c`, `DICParser.c/h`, `DICScanner.c` with bison/flex, compiled all 34 module sources as `-std=c++11` (only deprecation warnings: `std::binary_function` in `GenString.h`, old-style definitions in cc-regex), archived to a static lib, and linked a reader using the canonical API:
```cpp
CifFile* fobjR = new CifFile(CREATE_MODE, sdbFile, true);
CifParser* p = new CifParser(fobjR, CifFileReadDef(), fobjR->GetVerbose());
p->Parse(inFile, diags);
```
It parsed a sample cif and listed table `atom_site` correctly (API from `py-mmcif` test `CifReader.C`).

---

## 2. Flex/bison parser generation: how it works, tools/versions, CMake vs separate step

### How it works
- `cpp-cif-parser/src/` ships the grammar sources `CifScanner.l` (flex) and `CifParser.y` (bison), plus `DICScanner.l`/`DICParser.y` for dictionaries.
- py-mmcif's `CMakeLists.txt` runs them with:
```cmake
find_package(FLEX)
find_package(BISON)
flex_target(flextarget1 "${SOURCE_DIR_7}/CifScanner.l"  "${BUILD_SOURCE_DIR}/CifScanner.c" COMPILE_FLAGS "-Cfr -L -Pcifparser_")
bison_target(bisontarget1 "${SOURCE_DIR_7}/CifParser.y"  "${BUILD_SOURCE_DIR}/CifParser.c" COMPILE_FLAGS "-d -v -l -p cifparser_")
add_flex_bison_dependency(flextarget1 bisontarget1)
```
- `flex_target`/`bison_target`/`add_flex_bison_dependency` are macros provided by CMake's standard `FindFLEX`/`FindBISON` modules. They create build-time custom commands, so **generation is integrated into CMake** — no separate pre-generation step is required.
- Generated files are **plain C** (`CifParser.c`, `CifScanner.c`) compiled as C and linked into the C++ static lib. The scanner/parser are `extern "C"`-compatible; symbol prefixes match: flex `-Pcifparser_` renames `yylex`→`cifparser_lex`, bison `-p cifparser_` calls `cifparser_lex` (verified via `nm` on the built archive).
- Bison must run before flex (the scanner `#include "CifParser.h"`), which `add_flex_bison_dependency` guarantees.
- The generated `CifParser.h` and `CifScanner.c` must be on the include path for `CifScannerBase.C` (py-mmcif adds `${BUILD_SOURCE_DIR}` via `target_include_directories`).

### Tools/versions
- No pinned versions anywhere in py-mmcif or the modules. Verified **flex 2.6.4** and **bison 3.8.2** accept the exact flags:
  `bison -d -v -l -p cifparser_ -o CifParser.c CifParser.y` → produced `CifParser.c` + `CifParser.h`; `flex -Cfr -L -Pcifparser_ -t CifScanner.l` → `CifScanner.c` (3075 lines). Both generated files compiled as C (`-std=gnu11`).
- The scanner needs POSIX `fileno()`; DuckDB does not pin `CMAKE_C_STANDARD`, so C files compile with the compiler default (gnu) where `fileno` is declared. (A strict `-std=c11` hides it, but DuckDB doesn't use strict c11.)

### Does it build inside DuckDB's CMake?
Yes. The extension's `CMakeLists.txt` is included as a CMake subdirectory (`add_subdirectory(... extension/mmcif)` in `duckdb/extension/extension_build_tools.cmake`), and the `find_package` + `flex_target`/`bison_target` pattern is just CMake — so it runs inside DuckDB's build with no separate step.

### Tool availability in DuckDB CI
flex and bison are already installed in DuckDB's extension build environments:
- Linux Docker images: `yum install -y bison flex` / `apk add -qq bison flex` (`extension-ci-tools/docker/linux_amd64/Dockerfile`, `linux_arm64/Dockerfile`, and musl variants).
- macOS: `brew install bison flex` (`extension-ci-tools/.github/workflows/_extension_distribution.yml:637`).
- Windows: `choco install winflexbison3` (`_extension_distribution.yml:844`).
DuckDB core itself does not use flex/bison (no matches in duckdb CMake), but the CI images install them for extension builds.

---

## 3. CMake wiring into CMakeLists.txt / extension_config.cmake, static vs loadable, vcpkg, C++ flags

### Current template wiring (this repo)
- `extension_config.cmake` registers the extension: `duckdb_extension_load(mmcif SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR})` — DuckDB's `register_extension` then does `add_subdirectory(... extension/mmcif)` (`duckdb/extension/extension_build_tools.cmake`).
- Top-level `CMakeLists.txt` builds two targets from `EXTENSION_SOURCES`:
  - `build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})` → `${NAME}_extension` **STATIC** lib linking `duckdb_static`.
  - `build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})` → `${NAME}_loadable_extension` **SHARED** lib (`build_loadable_extension_directory` in `extension_build_tools.cmake`).
- Loadable distribution uses `EXTENSION_STATIC_BUILD=1` by default (Makefile `EXTENSION_STATIC_BUILD ?= 1`), building the shared ext with `duckdb_static` + hidden visibility + `-Wl,--gc-sections -Wl,--exclude-libs,ALL`.

### Recommended concrete wiring
Add the mmcif sources as a separate static lib and link it into both extension targets, e.g.:
```cmake
add_library(mmciflib STATIC
  <cpp-common/src/*.C> <cpp-tables/src/*.C> <cc-regex/src/*.c>
  <cpp-cif-file/src/*.C> <cpp-cif-parser/src/*.C>)
target_include_directories(mmciflib PUBLIC
  <cpp-common/include> <cpp-tables/include> <cc-regex/include>
  <cpp-cif-file/include> <cpp-cif-parser/include> <cpp-common/src>
  ${CMAKE_CURRENT_BINARY_DIR}/mmcif-gen)
# flex/bison generation (from py-mmcif):
find_package(FLEX) ; find_package(BISON)
flex_target(...) ; bison_target(...) ; add_flex_bison_dependency(...)
target_link_libraries(${EXTENSION_NAME} mmciflib)
target_link_libraries(${LOADABLE_EXTENSION_NAME} mmciflib)
```
Notes:
- `cpp-common/src` must be on the include path because `cpp-tables/include/ISTable.h` and `TableFile.h` do `#include "mapped_vector.C"` / `#include "mapped_ptr_vector.C"` directly (template implementations; include-guarded, safe to include in multiple TUs). py-mmcif copies those `.C` files into the include dir; adding `cpp-common/src` to the include path is simpler.
- The generated parser/scanner `.c` files and the bison `CifParser.h` must be in the build dir and on the include path (mirror py-mmcif's `${BUILD_SOURCE_DIR}`).
- **Static vs loadable**: static is straightforward (both targets link `mmciflib`). For the loadable target, hidden visibility (`CXX_VISIBILITY_PRESET hidden`) + `--exclude-libs,ALL` already hide internal symbols; the mmcif lib compiles with `-fPIC` because DuckDB sets `CMAKE_POSITION_INDEPENDENT_CODE ON` globally — required for linking static objects into the shared ext. No ABI/export concerns for internal mmcif symbols.
- **vcpkg**: the mmcif library has **no external dependencies** beyond flex/bison (build tools) and the standard library — so **no vcpkg packages are needed**; `vcpkg.json` needs no additions. flex/bison are installed at the system/CI level, not via vcpkg (DuckDB's CI already installs them). The existing `openssl` dep in the template is unrelated (can stay or be dropped).

### C++ standard / flags conflict (corrects the ticket premise)
**DuckDB's build defaults to C++11, not C++17.**
`duckdb/CMakeLists.txt:54` sets `set(CMAKE_CXX_STANDARD "11" CACHE STRING ...)` with `CMAKE_CXX_STANDARD_REQUIRED ON` (lines 54–57). The extension template's `set(CMAKE_CXX_STANDARD "17" CACHE STRING ...)` is a non-`FORCE` cache set that the already-set `11` cache entry wins over — so the extension and the mmcif lib compile at **C++11** by default. (The template `Makefile`/DuckDB `Makefile` allow `CXX_STANDARD` to override.)
- py-mmcif compiles the lib with `-std=c++11 -flto -fno-common -fvisibility=hidden -fvisibility-inlines-hidden` plus defines `-DBIG_ENDIAN_PLATFORM -DHAVE_STRCASECMP -DINCL_TEMPLATE_SRC -DHAVE_PLACEMENT_NEW` (`py-mmcif/CMakeLists.txt`). Those defines are **vestigial** — none are referenced by the current C++ sources (only appear in py-mmcif CMake and the Eclipse `.cproject`); they can be omitted.
- **C++17 conflict (real but avoidable)**: `cpp-common/include/GenString.h:80` uses `std::binary_function`, which was **removed in C++17** (deprecated in C++11, so it's only a warning at the default C++11 build). If a user forces `-DCMAKE_CXX_STANDARD=17`, the mmcif C++ sources fail to compile. Mitigations: pin the mmcif target to C++11 (`set_target_properties(mmciflib PROPERTIES CXX_STANDARD 11)` or `target_compile_options(... -std=c++11)`), or patch `GenString.h`. No `register` keyword is present in the C++ sources (the only C++17-removed constructs are `binary_function`); cc-regex is C and unaffected.
- py-mmcif requires GCC ≥ 4.8 or Clang ≥ 8.1 and explicitly **rejects MSVC** (`FATAL_ERROR "No current support for MSVC!"`). DuckDB CI builds Windows via mingw (`winflexbison3` + mingw) — MSVC builds of the mmcif lib are unsupported upstream.

---

## 4. License / provenance

- `py-mmcif` (the aggregator/package) ships an **Apache License 2.0** `LICENSE` file (`py-mmcif/LICENSE`).
- The C++ submodules carry **no LICENSE files**: `cpp-cif-file`, `cpp-cif-parser`, `cpp-common`, `cpp-cif-file-util`, `cpp-dict-obj-file` have no LICENSE/COPYRIGHT; `cpp-tables/LICENSE` is empty (0 bytes). Headers/sources use a `//$$LICENSE$$` template marker (e.g. `cpp-cif-file/src/CifFile.C`, `cpp-cif-parser/src/CifParserBase.C`) — a build/export substitution placeholder, so no inline license text is present.
- `cc-regex/COPYRIGHT` is **Henry Spencer's regex** (permissive public-domain-style terms: use freely, credit in docs, mark altered versions, keep notice).
- Practical provenance for vendoring/submoduling: the code is RCSB's public C++ mmcif core, distributed via GitHub under the `rcsb` org; py-mmcif is Apache-2.0, `cc-regex` carries Spencer's permissive notice. Record both in any NOTICE/THIRD-PARTY file if the extension is distributed. No external/third-party code beyond Spencer's regex and the standard library; no commercial restrictions observed.

---

## Sources (primary)
- `rcsb/py-mmcif` — `CMakeLists.txt`, `.gitmodules`, `LICENSE`, `modules/cpp-mmciflib-test/src/CifReader.C`, `README.md` (github.com/rcsb/py-mmcif)
- `rcsb/cpp-cif-file` — `src/CifFile.C`, `include/CifFile.h`, `Makefile` (github.com/rcsb/cpp-cif-file)
- `rcsb/cpp-cif-parser` — `src/CifParserBase.C`, `src/CifParser.y`, `src/CifScanner.l`, `include/CifParserInt.h`, `include/CifScannerInt.h`, `Makefile` (github.com/rcsb/cpp-cif-parser)
- `rcsb/cpp-common` — `src/*.C`, `include/GenString.h`, `include/mapped_vector.h`, `Makefile` (github.com/rcsb/cpp-common)
- `rcsb/cpp-tables` — `src/*.C`, `include/ISTable.h`, `include/TableFile.h`, `Makefile` (github.com/rcsb/cpp-tables)
- `rcsb/cc-regex` — `src/*.c`, `COPYRIGHT` (github.com/rcsb/cc-regex)
- `rcsb/cpp-cif-file-util`, `rcsb/cpp-dict-obj-file` — confirmed not required for data-file parsing
- This repo `duckdb/CMakeLists.txt` (lines 54–57: C++11), `duckdb/extension/extension_build_tools.cmake`, `extension_config.cmake`, `CMakeLists.txt`, `vcpkg.json`
- `duckdb/extension-ci-tools` — `makefiles/duckdb_extension.Makefile`, `docker/linux_amd64/Dockerfile` (+ arm64/musl), `.github/workflows/_extension_distribution.yml` (flex/bison installs)
- Local verification: bison 3.8.2 + flex 2.6.4 generated and compiled all parser/scanner files; full minimal closure archived and parsed a sample cif.
