# AlphaFold DB archive fixture

`AF-A0A009IHW8-F1-model_v6.cif.gz` is an unmodified AlphaFold DB v6 CIF,
extracted on 2026-10-10 from the beginning of EMBL-EBI's
[Swiss-Prot CIF archive](https://ftp.ebi.ac.uk/pub/databases/alphafold/v6/swissprot_cif_v6.tar)
using an HTTP byte-range download. Archive member:
`AF-A0A009IHW8-F1-model_v6.cif.gz`.

SHA-256: `ba5148edf6380cc2287bee37fbd36adefa75c8d0e3a0318cd3f0f23c80095f40`.

The file declares ModelCIF 1.3.9. It is read using the bundled ModelCIF 1.4.9
definitions alongside the current PDBx/mmCIF base. It includes global and local
pLDDT values, model metadata, templates and atomic coordinates.

The regression test checks numeric quality metrics, discovery and catalog
metadata, dictionary documentation ownership, and links to model and metric
identifiers. AlphaFold DB data is distributed under
[CC BY 4.0](https://creativecommons.org/licenses/by/4.0/); attribution and
publication references are included in the CIF's citation categories.
