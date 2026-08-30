-- Metadata extraction for a structure, mirroring protein-quest's
-- structure/metadata.py::structure_metadata() (which uses gemmi.Structure).
--
-- Gemmi reads this from the parsed structure object:
--   * structure.name            -> entry.id
--   * structure.resolution      -> refine.ls_d_res_high   (X-ray/EM)
--   * structure.info["_exptl.method"] -> exptl.method
--   * structure.meta.software   -> software.name
--   * get_label2auth_chains()   -> atom_site (label_asym_id / auth_asym_id)
--   * nr_of_residues_in_total() -> distinct residues across all chains
--   * UniProt accession         -> struct_ref + struct_ref_seq
--
-- Structure: 3PLZ — human LRH1 ligand-binding domain. Two protein chains
-- (auth A and B) plus a short fragment chain (auth C) and waters, so the
-- label/auth chain mapping here is non-trivial (label C -> auth B etc.).
-- Source file: https://files.rcsb.org/download/3PLZ.cif.gz
--
-- Run from the repository root with:
--   ./build/release/duckdb < docs/examples/metadata.sql

ATTACH '3PLZ.cif.gz' AS cif (TYPE mmcif);
USE cif;

-- 1) Entry-level summary — the fields StructureMetadata carries.
SELECT id                                  AS id,
       (SELECT ls_d_res_high FROM refine)  AS resolution,
       (SELECT method FROM exptl)          AS method,
       (SELECT list(name) FROM software)   AS software,
       (SELECT count(*) FILTER (name IN ('AlphaFold','alphafill'))
          FROM software) > 0               AS is_alphafold
FROM entry;

-- 2) Total residue count — nr_of_residues_in_total(). Count distinct
--    (chain, seq) pairs so multi-model files are not double counted.
SELECT count(DISTINCT label_asym_id || ':' || label_seq_id) AS total_residues
FROM atom_site
WHERE group_PDB = 'ATOM';

-- 3) label -> auth chain mapping — get_label2auth_chains(). One row per
--    label chain with its author chain id, taken from the first ATOM row
--    (first-seen wins, matching the gemmi helper).
SELECT label_asym_id            AS label_chain,
       min(auth_asym_id)        AS auth_chain,
       count(*)                 AS atoms
FROM atom_site
WHERE group_PDB = 'ATOM'
GROUP BY label_asym_id
ORDER BY label_asym_id;

-- 4) Per-chain residue counts — len(chain) for each auth chain, the value
--    used for StructureMetadata.chain_length.
SELECT auth_asym_id             AS auth_chain,
       count(DISTINCT label_seq_id) AS chain_length
FROM atom_site
WHERE group_PDB = 'ATOM' AND label_comp_id NOT IN ('HOH')
GROUP BY auth_asym_id
ORDER BY auth_asym_id;

-- 5) UniProt accession(s) — structure_to_uniprot(). One row per chain from
--    _struct_ref_seq, filtered to _struct_ref rows with db_name = 'UNP'.
SELECT pdbx_strand_id          AS auth_chain,
       pdbx_db_accession       AS uniprot_accession,
       min(db_align_beg)       AS uniprot_start,
       max(db_align_end)       AS uniprot_end,
       sum(db_align_end - db_align_beg + 1) AS aligned_residues
FROM struct_ref_seq
WHERE ref_id IN (SELECT id FROM struct_ref WHERE db_name = 'UNP')
GROUP BY 1, 2
ORDER BY 1;

-- 6) Combined per-chain metadata, one row per auth chain — a single SQL
--    replacement for the loop over chains in structure_metadata().
WITH chains AS (
    SELECT auth_asym_id,
           count(DISTINCT label_seq_id) AS chain_length
    FROM atom_site
    WHERE group_PDB = 'ATOM' AND label_comp_id NOT IN ('HOH')
    GROUP BY auth_asym_id
),
unp AS (
    SELECT pdbx_strand_id AS chain, pdbx_db_accession AS acc
    FROM struct_ref_seq
    WHERE ref_id IN (SELECT id FROM struct_ref WHERE db_name = 'UNP')
)
SELECT c.auth_asym_id              AS auth_chain,
       c.chain_length,
       u.acc                       AS uniprot_accession,
       (SELECT label_asym_id FROM atom_site
         WHERE auth_asym_id = c.auth_asym_id AND group_PDB = 'ATOM'
         LIMIT 1)                  AS label_chain
FROM chains c
LEFT JOIN unp u ON u.chain = c.auth_asym_id
ORDER BY c.auth_asym_id;
