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

C++ unit tests live in `./test/cpp` and use the vendored Catch header (`duckdb/third_party/catch/catch.hpp`), like DuckDB's own core tests. They cover the parser / index / write-store / write-back patch core paths the SQL tests can't reach. Run them with:

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
- `scripts/` — helper scripts (e.g. `mmcif_relationships_diagram.py`, which regenerates `docs/diagrams/rel.svg`)
- `docs/examples/` — ready-to-run example scripts
- `duckdb/`, `extension-ci-tools/` — git submodules

## Updating the DuckDB target version

See [docs/UPDATING.md](docs/UPDATING.md).

## Updating the bundled dictionaries

The extension embeds combined type, relationship and documentation indexes for
PDBx/mmCIF v5, IHMCIF, flrCIF and 3DEM. Builds and queries never download or parse
source dictionaries. `dict/sources.json` pins the source versions, URLs and
SHA-256 checksums; the generator uses only Python's standard library.

1. Download all sources in the manifest to a local directory:

   ```sh
   mkdir -p .scratch/dictionaries
   python3 - <<'PYTHON'
   import json
   from pathlib import Path
   from urllib.request import urlopen

   for source in json.loads(Path("dict/sources.json").read_text())["sources"]:
       with urlopen(source["url"]) as response:
           Path(".scratch/dictionaries", source["filename"]).write_bytes(response.read())
   PYTHON
   ```

2. When intentionally updating a source, review its changes and update its
   `version` and `sha256` in the manifest. The version is `_dictionary.version`
   in the source; obtain the checksum with `sha256sum`. If its location or
   dictionary browser changes, also update `url` or `documentation_url`.
   Downloads from wwPDB can change; checksum mismatches intentionally stop
   generation until the new source has been reviewed.

3. Generate all three artifacts:

   ```sh
   python3 scripts/generate_type_index.py .scratch/dictionaries
   ```

   Saveframes, scalar fields and loop fields are parsed, including both
   `_item_linked` and `_pdbx_item_linked_group_list` relationships. Duplicate
   items and links are merged case-insensitively. Documentation ownership
   follows the manifest order: base, IHMCIF, flrCIF, 3DEM. This keeps base URLs
   for shared definitions while linking extension-only items to their source.

   Differing DuckDB types stop generation unless an exact resolution is
   recorded in `type_conflicts`. Each resolution names every source's exposed
   type, the source to use and the reason. The ten current conflicts retain the
   PDBx/mmCIF v5 types over the standalone 3DEM definitions. Changed conflicts
   and stale resolutions fail rather than silently changing behavior.

4. Run `make test_dictionary`, `make test` and `make test_cpp`, then commit the
   manifest and all three artifacts together. Generated gzip files omit
   timestamps and local paths so regeneration is reproducible.

## AI Declaration

See [aidecl.yaml](aidecl.yaml) for details on the AI tools used in this repository.
