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
- `scripts/` — helper scripts (e.g. `mmcif_relationships_diagram.py`, which regenerates `rel.svg`)
- `docs/examples/` — ready-to-run example scripts
- `duckdb/`, `extension-ci-tools/` — git submodules

## Updating the DuckDB target version

See [docs/UPDATING.md](docs/UPDATING.md).
