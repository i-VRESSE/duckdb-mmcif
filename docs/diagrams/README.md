# Dictionary extension ER diagrams

The SVGs show the relationships returned by `mmcif_relationships()` for real
archive examples: IHMCIF (8ZZE), flrCIF (9A08) and EMDB/3DEM (EMD-4404).
They include linked categories from the base dictionary and other extensions.
As with the existing PDBx/mmCIF diagram, nodes list relationship columns and
edges connect parent and child columns. Categories without links are omitted.

EMD-4404's EM categories are already included in the current base dictionary;
the diagram illustrates current EM archive data rather than definitions unique
to the standalone 3DEM dictionary.

To regenerate, build the extension and install Graphviz, then run from the
repository root:

```sh
mkdir -p .scratch/real-dictionaries
curl --fail --location https://pdb-ihm.org/cif/8zze.cif \
    --output .scratch/real-dictionaries/8zze.cif
curl --fail --location https://pdb-ihm.org/cif/9a08.cif \
    --output .scratch/real-dictionaries/9a08.cif

python3 scripts/mmcif_relationships_diagram.py \
    .scratch/real-dictionaries/8zze.cif -f dot \
    | dot -Tsvg -o docs/diagrams/ihm.svg
python3 scripts/mmcif_relationships_diagram.py \
    .scratch/real-dictionaries/9a08.cif -f dot \
    | dot -Tsvg -o docs/diagrams/flr.svg
python3 scripts/mmcif_relationships_diagram.py \
    test/data/emd-4404.cif.gz -f dot \
    | dot -Tsvg -o docs/diagrams/3dem.svg
```

The EMDB file is the unmodified archive fixture checked into `test/data`;
its download URL and checksum are recorded in
[the fixture documentation](../../test/data/emd-4404.README.md).
The existing `rel.svg` and its README presentation are maintained separately.
