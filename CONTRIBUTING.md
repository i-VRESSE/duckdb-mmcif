# Contributing to duckdb-mmcif

Thanks for your interest in improving the extension! The user-facing docs live in [README.md](README.md).

## Building

Requires DuckDB and the extension-ci-tools submodules; fetch them with `--recurse-submodules` when cloning.

```sh
make
```

The main binaries built are:

```sh
./build/release/duckdb                                # duckdb shell with the extension pre-loaded
./build/release/test/unittest                         # DuckDB test runner (extension linked in)
./build/release/extension/mmcif/mmcif.duckdb_extension   # loadable extension binary
```

To speed up rebuilds install [ccache](https://ccache.dev/) and [ninja](https://ninja-build.org/) and build with `GEN=ninja make`.

## Running the tests

SQL tests live in `./test/sql` (see `test/sql/mmcif.test`). Run them with:

```sh
make test
```

C++ unit tests live in `./test/cpp` and use the vendored Catch header (`duckdb/third_party/catch/catch.hpp`), like DuckDB's own core tests. They cover the parser / index / write-store / writer core paths the SQL tests can't reach. Run them with:

```sh
make test_cpp
```

This builds the `mmcif_catch_tests` binary and runs it. The target is excluded from the default build (`make`/`make release`), so it only compiles when you run `make test_cpp`.

## Trying things out

Start the shell (the extension is pre-loaded):

```sh
./build/release/duckdb
```

The SQL tests and the examples in the README rely on the fixture `test/data/1amb_updated.cif`. Download it with:

```sh
curl -o test/data/1amb_updated.cif https://www.ebi.ac.uk/pdbe/entry-files/download/1amb_updated.cif
```

## Repository layout

- `src/` — extension sources
- `modules/` — vendored RCSB `cpp-cif-parser` / `cpp-cif-file` core libraries
- `dict/` — mmCIF dictionary type index used for column type inference
- `test/sql/`, `test/data/` — SQLLogic tests and fixtures
- `test/cpp/` — C++ Catch unit tests (run with `make test_cpp`)
- `scripts/` — helper scripts (e.g. `mmcif_relationships_diagram.py`, which regenerates `rel.svg`)
- `docs/examples/` — ready-to-run example scripts
- `duckdb/`, `extension-ci-tools/` — git submodules

## Updating the DuckDB target version

See [docs/UPDATING.md](docs/UPDATING.md).

## Updating the mmCIF dictionary

When a new [PDBx/mmCIF dictionary](https://mmcif.wwpdb.org/pdbx-mmcif-home-page.html) version is released:

1. Open the [wwPDB dictionary downloads page](https://mmcif.wwpdb.org/dictionaries/downloads.html).
   Under **PDB Exchange Dictionary (PDBx/mmCIF) Version 5.0**, select
   **Dictionary Text** (not the gzipped download) and save the file as
   `mmcif_pdbx_v50.dic`. Alternatively, download it from the command line:

   ```sh
   curl --fail --location \
       --output mmcif_pdbx_v50.dic \
       https://mmcif.wwpdb.org/dictionaries/ascii/mmcif_pdbx_v50.dic
   ```

2. Update `DIC_VERSION` in `scripts/generate_type_index.py` and, if the download
   location changed, update `SOURCE_URL` as well.
3. Regenerate both checked-in dictionary artifacts:

   ```sh
   python3 scripts/generate_type_index.py \
       mmcif_pdbx_v50.dic \
       dict/mmcif_pdbx_v50_type_index.tsv.gz \
       dict/mmcif_pdbx_v50_relationships.tsv.gz
   ```

4. Review the generated files and run `make test` and `make test_cpp` before
   committing the script metadata and both files under `dict/` together.

## AI Declaration

See [aidecl.yaml](aidecl.yaml) for details on the AI tools used in this repository.
