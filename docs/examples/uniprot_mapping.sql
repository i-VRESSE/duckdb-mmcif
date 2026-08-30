-- UniProt chain-mapping extraction, mirroring protein-quest's
-- structure/uniprot_extraction.py (structure_to_uniprot and friends) and
-- structure/sifts.py, which read _struct_ref / _struct_ref_seq and
-- _pdbx_sifts_unp_segments categories through gemmi.
--
-- Structure: 3PLZ — two protein chains mapped to UniProt: auth A/B ->
-- Q9UEC0 (LRH1), auth C/D -> Q15596 (a short fragment). No
-- _pdbx_sifts_unp_segments category in this entry, so the struct_ref_seq path
-- is demonstrated; the SIFTS alternative is shown in query 5.
-- Source file: https://files.rcsb.org/download/3PLZ.cif.gz
--
-- Run from the repository root with:
--   ./build/release/duckdb < docs/examples/uniprot_mapping.sql

ATTACH '3PLZ.cif.gz' AS cif (TYPE mmcif);
USE cif;

-- 1) Raw _struct_ref rows naming UniProt — the starting point of
--    uniprot_chain_mappings_from_struct_ref_seq().
SELECT id, entity_id, db_name, pdbx_db_accession
FROM struct_ref
WHERE db_name = 'UNP'
ORDER BY id;

-- 2) Raw _struct_ref_seq alignment rows for those references.
SELECT align_id, ref_id, pdbx_strand_id AS chain,
       pdbx_db_accession AS acc,
       db_align_beg, db_align_end
FROM struct_ref_seq
WHERE ref_id IN (SELECT id FROM struct_ref WHERE db_name = 'UNP')
ORDER BY align_id;

-- 3) Flattened per-(accession, chain) records — the FlattenedUniprotChainMapping
--    dataclass: merged start/end, summed aligned residues, sequence identity.
SELECT pdbx_strand_id                AS chain,
       pdbx_db_accession             AS uniprot_accession,
       min(db_align_beg)             AS uniprot_start,
       max(db_align_end)             AS uniprot_end,
       sum(db_align_end - db_align_beg + 1) AS aligned_residues,
       round(
         sum(db_align_end - db_align_beg + 1)
         / (max(db_align_end) - min(db_align_beg) + 1), 3
       )                             AS sequence_identity
FROM struct_ref_seq
WHERE ref_id IN (SELECT id FROM struct_ref WHERE db_name = 'UNP')
GROUP BY 1, 2
ORDER BY 1;

-- 4) Best UniProt per chain — best_uniprot_per_chain() picks the accession
--    with the highest aligned-residue count (ties alphabetically).
WITH flat AS (
    SELECT pdbx_strand_id AS chain,
           pdbx_db_accession AS acc,
           sum(db_align_end - db_align_beg + 1) AS aligned
    FROM struct_ref_seq
    WHERE ref_id IN (SELECT id FROM struct_ref WHERE db_name = 'UNP')
    GROUP BY 1, 2
)
SELECT chain, acc, aligned
FROM flat
QUALIFY row_number() OVER (PARTITION BY chain ORDER BY aligned DESC, acc) = 1
ORDER BY chain;

-- 5) SIFTS alternative: when the file carries a _pdbx_sifts_unp_segments
--    category (most modern RCSB entries do), the same mappings come straight
--    from that single table — the path used by sifts.py. Columns as defined
--    by the mmCIF dictionary: asym_id, unp_acc, unp_start, unp_end,
--    best_mapping, identity, seq_id_start, seq_id_end, ...
--    (3PLZ has no such category, so this query is commented out; uncomment
--    and point it at a file that does.)
-- SELECT asym_id, unp_acc,
--        min(unp_start) AS start, max(unp_end) AS finish
-- FROM pdbx_sifts_unp_segments
-- WHERE best_mapping = 'y'
-- GROUP BY 1, 2;

-- 6) Injection in write mode: append missing UniProt accessions as
--    _struct_ref / _struct_ref_seq rows, mirroring
--    uniprot_injection.py::_append_uniprot_to_structure. Here we add a
--    fictional accession Q00000 for chain A on a copy file.
--    Create the copy first:  cp 3PLZ.cif.gz 3PLZ_unp.cif.gz
ATTACH '3PLZ_unp.cif.gz' AS wdb (TYPE mmcif, READ_WRITE TRUE);
USE wdb;

BEGIN;
-- one _struct_ref row per (entity, accession), then one _struct_ref_seq row
INSERT INTO struct_ref (id, entity_id, db_name, db_code, pdbx_db_accession, pdbx_db_isoform)
    VALUES ('99', '1', 'UNP', NULL, 'Q00000', NULL);
INSERT INTO struct_ref_seq
    (align_id, ref_id, pdbx_strand_id, pdbx_db_accession, db_align_beg, db_align_end)
    VALUES ('99', '99', 'A', 'Q00000', 300, 541);
COMMIT;   -- writes the updated CifFile back to 3PLZ_unp.cif.gz
USE memory;
DETACH wdb;

-- 7) Verification of the injected mapping.
ATTACH '3PLZ_unp.cif.gz' AS chk (TYPE mmcif);
USE chk;
SELECT align_id, pdbx_strand_id, pdbx_db_accession, db_align_beg, db_align_end
FROM struct_ref_seq WHERE align_id = '99';
USE memory;
DETACH chk;
