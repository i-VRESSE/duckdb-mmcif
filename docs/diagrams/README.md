# Dictionary extension ER diagrams

The SVGs show the relationships returned by `mmcif_relationships()` for real
archive examples: IHMCIF (8ZZE), flrCIF (9A08) and EMDB/3DEM (EMD-4404).
Each extension SVG is restricted to categories listed in its source dictionary
and present in the example file. The flrCIF SVG additionally selects `flr_*`
categories, excluding the IHM definitions repeated in that dictionary. Edges
connect parent and child columns only when both categories are selected.
Categories without an internal relationship are shown as isolated nodes.
The PDBx/mmCIF diagram retains its original categories and presentation.

EMD-4404's EM categories are already included in the current base dictionary;
the 3DEM SVG includes only those EM categories also listed in the standalone
3DEM dictionary, even when their runtime definitions come from the base.

To regenerate, build the extension and install Graphviz, then run from the
repository root:

```sh
mkdir -p .scratch/real-dictionaries
curl --fail --location https://pdb-ihm.org/cif/8zze.cif \
    --output .scratch/real-dictionaries/8zze.cif
curl --fail --location https://pdb-ihm.org/cif/9a08.cif \
    --output .scratch/real-dictionaries/9a08.cif

mkdir -p .scratch/dictionaries
curl --fail --location https://mmcif.wwpdb.org/dictionaries/ascii/mmcif_ihm_ext.dic \
    --output .scratch/dictionaries/mmcif_ihm_ext.dic
curl --fail --location https://mmcif.wwpdb.org/dictionaries/ascii/mmcif_ihm_flr_ext.dic \
    --output .scratch/dictionaries/mmcif_ihm_flr_ext.dic
curl --fail --location https://mmcif.wwpdb.org/dictionaries/ascii/mmcif_em.dic \
    --output .scratch/dictionaries/mmcif_em.dic

python3 scripts/mmcif_relationships_diagram.py \
    .scratch/real-dictionaries/8zze.cif -f dot \
    --dictionary .scratch/dictionaries/mmcif_ihm_ext.dic \
    | dot -Tsvg -o docs/diagrams/ihm.svg
python3 scripts/mmcif_relationships_diagram.py \
    .scratch/real-dictionaries/9a08.cif -f dot \
    --dictionary .scratch/dictionaries/mmcif_ihm_flr_ext.dic \
    --category-prefix flr_ \
    | dot -Tsvg -o docs/diagrams/flr.svg
python3 scripts/mmcif_relationships_diagram.py \
    test/data/emd-4404.cif.gz -f dot \
    --dictionary .scratch/dictionaries/mmcif_em.dic \
    | dot -Tsvg -o docs/diagrams/3dem.svg
```

Source dictionary versions and checksums used for these diagrams match
[`dict/sources.json`](../../dict/sources.json).

The EMDB file is the unmodified archive fixture checked into `test/data`;
its download URL and checksum are recorded in
[the fixture documentation](../../test/data/emd-4404.README.md).
The existing PDBx/mmCIF diagram, `pdbx.svg`, is stored here alongside the
extension diagrams and remains expanded in the main README. To regenerate it:

```sh
python3 scripts/mmcif_relationships_diagram.py \
    test/data/1amb_updated.cif -f dot \
    | dot -Tsvg -o docs/diagrams/pdbx.svg
```
