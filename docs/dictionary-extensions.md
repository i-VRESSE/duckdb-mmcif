# Dictionary extensions

The extension bundles the current PDBx/mmCIF v5 dictionary together with
**IHMCIF**, **flrCIF**, **3DEM** and **ModelCIF**. All are available by default:
no dictionary selection or downloads are needed when opening a file.

```sql
ATTACH 'integrative_model.cif' AS model (TYPE mmcif);
SHOW TABLES FROM model;
SELECT * FROM model.ihm_model_list;
-- Or discover and scan categories without attaching:
SELECT * FROM mmcif_tables('integrative_model.cif');
SELECT * FROM mmcif_scan('integrative_model.cif', 'ihm_model_list');
```

Only categories and columns present in the selected data block are exposed.
The combined dictionary supplies types, documentation links and relationships,
including links between extension categories and base categories. Catalog
comments link to the dictionary defining each category or item. Unknown items
keep the `VARCHAR` fallback. Dictionary ranges and comma-separated integer
lists also remain `VARCHAR` so values such as `3-174` are preserved.

Shared definitions keep the current base dictionary's types and documentation;
reviewed type conflicts are recorded in [`dict/sources.json`](../dict/sources.json).
That manifest also records bundled dictionary versions and source checksums.
Older and alternative base dictionaries are not bundled.

## Reading the ER diagrams

The diagrams below show categories defined by each extension and present in
its example file. They show relationships returned by `mmcif_relationships()`
when both categories belong to that extension. Nodes list columns participating
in those relationships; categories without internal relationships appear as
isolated nodes. These examples do not cover every category in a dictionary.

The runtime also exposes relationships to base categories and other extensions;
those links are omitted from these diagrams to keep the focus on each extension.
See [diagram generation instructions](diagrams/README.md) to regenerate them.
The [PDBx/mmCIF diagram](../README.md#table-functions) remains in the main README.

## IHMCIF

IHMCIF describes structural models obtained with integrative/hybrid methods.
Its `ihm_*` categories cover models, assemblies, datasets, restraints and
ensembles. It extends PDBx/mmCIF and is used by the PDB-IHM archive.

Bundled version: **1.28**. [Dictionary documentation](https://mmcif.wwpdb.org/dictionaries/mmcif_ihm_ext.dic).

Example: [PDB-IHM entry 8ZZE](https://pdb-ihm.org/cif/8zze.cif).

![IHMCIF relationships in PDB-IHM entry 8ZZE](diagrams/ihm.svg)

## flrCIF

flrCIF describes fluorescence and FRET data used in structural models. It builds
on PDBx/mmCIF and IHMCIF, adding `flr_*` categories for fluorescence experiments,
FRET measurements and their interpretation in models. Its dictionary repeats
IHM definitions; the diagram below includes only `flr_*` categories.

Bundled version: **0.03**. [Dictionary documentation](https://mmcif.wwpdb.org/dictionaries/mmcif_ihm_flr_ext.dic).

Example: [FRET entry 9A08 in PDB-IHM](https://pdb-ihm.org/cif/9a08.cif).

![flrCIF relationships in FRET entry 9A08](diagrams/flr.svg)

## 3DEM

The 3DEM dictionary describes three-dimensional electron microscopy data using
`em_*` categories, including sample preparation, imaging and reconstruction.
Much of this dictionary has been incorporated into PDBx/mmCIF, where further
development of EM definitions takes place. Shared definitions use the bundled
current base dictionary's types and documentation.

Bundled version: **0.015**. [Dictionary documentation](https://mmcif.wwpdb.org/dictionaries/mmcif_em.dic).

Example: [EMDB entry EMD-4404 experimental metadata](https://ftp.ebi.ac.uk/pub/databases/emdb/structures/EMD-4404/metadata/emd-4404.cif.gz).
This entry's EM categories are already in the current PDBx/mmCIF base dictionary.
The diagram includes only those also listed in the standalone 3DEM dictionary.

![3DEM relationships in EMDB entry EMD-4404](diagrams/3dem.svg)

## ModelCIF

ModelCIF describes computationally predicted structural models, including the
models distributed by AlphaFold DB. Its `ma_*` categories cover model metadata,
templates, protocols and quality metrics such as global and per-residue pLDDT.
It extends PDBx/mmCIF; all four extension dictionaries are available together.

Bundled version: **1.4.9**. [Dictionary documentation](https://mmcif.wwpdb.org/dictionaries/mmcif_ma.dic/Index/).

Example: [AlphaFold DB model AF-A0A009IHW8-F1](https://alphafold.ebi.ac.uk/entry/AF-A0A009IHW8-F1),
from the [EMBL-EBI Swiss-Prot v6 CIF archive](https://ftp.ebi.ac.uk/pub/databases/alphafold/v6/swissprot_cif_v6.tar).
The unmodified gzip is included as a [test fixture](../test/data/AF-A0A009IHW8-F1.README.md).
The diagram selects only `ma_*` categories, excluding base definitions repeated
in the ModelCIF dictionary.

```sql
ATTACH 'AF-A0A009IHW8-F1-model_v6.cif.gz' AS prediction (TYPE mmcif);
SELECT m.name, q.metric_value
FROM prediction.ma_qa_metric_global q
JOIN prediction.ma_qa_metric m ON q.metric_id = m.id;
-- pLDDT, 84.99
```

![ModelCIF relationships in AlphaFold DB model AF-A0A009IHW8-F1](diagrams/ma.svg)
