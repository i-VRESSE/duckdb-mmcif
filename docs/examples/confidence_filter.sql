-- AlphaFold confidence (pLDDT) filtering, mirroring protein-quest's
-- alphafold/confidence.py::filter_structure_on_confidence.
--
-- Gemmi reads pLDDT from the isotropic B-factor column of the first atom of
-- each residue: res[0].b_iso. In mmCIF that is atom_site.B_iso_or_equiv.
-- AlphaFoldDB *.cif files store pLDDT there (0..100); X-ray files store a
-- real B-factor. The queries below work on either — just set the threshold.
--
-- NOTE: 3PLZ is X-ray (B-factors 9.7..46.5), used here only to show the SQL
-- mechanics. For a real AlphaFold file, threshold e.g. 70 and the column
-- values are pLDDT.
-- Source file: https://files.rcsb.org/download/3PLZ.cif.gz
--
-- Run from the repository root with:
--   ./build/release/duckdb < docs/examples/confidence_filter.sql

ATTACH '3PLZ.cif.gz' AS cif (TYPE mmcif);
USE cif;

-- 1) Count of high-confidence residues per chain — find_high_confidence_residues().
--    A residue counts as high-confidence when its maximum B_iso_or_equiv
--    (over all atoms) exceeds the threshold.
CREATE TEMP TABLE per_res AS
SELECT auth_asym_id, label_seq_id, max(B_iso_or_equiv) AS b
FROM atom_site
WHERE group_PDB = 'ATOM' AND label_comp_id NOT IN ('HOH')
GROUP BY 1, 2;

SELECT auth_asym_id AS chain,
       count(*) FILTER (b > 70)   AS high_conf_residues,
       count(*)                   AS total_residues
FROM per_res
GROUP BY auth_asym_id
ORDER BY auth_asym_id;

-- 2) Overall pass/fail — the ConfidenceFilterQuery min/max residue thresholds.
WITH stats AS (
    SELECT count(*) FILTER (b > 70) AS count,
           count(*)                 AS total
    FROM per_res
)
SELECT count AS high_confidence_residues,
       (count BETWEEN 200 AND 10000000) AS passed   -- min/max residues
FROM stats;

DROP TABLE per_res;

-- 3) Write mode: drop every residue whose atoms are all low-confidence, on a
--    copy so the original is never touched.
--    Create the copy first:  cp 3PLZ.cif.gz 3PLZ_AF.cif.gz
--
--    (A production AlphaFold filter also cleans the dependent per-residue
--    categories pdbx_unobs_or_zero_occ_atoms / _residues and
--    pdbx_validate_torsion, exactly as in docs/examples/keep_chain_A.sql.)

ATTACH '3PLZ_AF.cif.gz' AS wdb (TYPE mmcif, READ_WRITE TRUE);
USE wdb;

SELECT 'before' AS phase, count(*) AS atoms FROM atom_site;

BEGIN;
-- Residues with no atom above the threshold; a same-table subquery in DELETE
-- is supported (the binder keeps the table entry alive across bindings).
DELETE FROM atom_site
WHERE group_PDB = 'ATOM' AND label_comp_id NOT IN ('HOH')
  AND label_seq_id IN (
    SELECT label_seq_id FROM atom_site
    WHERE group_PDB = 'ATOM' AND label_comp_id NOT IN ('HOH')
    GROUP BY label_seq_id
    HAVING max(B_iso_or_equiv) <= 70
  );
COMMIT;   -- writes the filtered CifFile back to 3PLZ_AF.cif.gz

SELECT 'after' AS phase, count(*) AS atoms FROM atom_site;

USE memory;
DETACH wdb;

-- 4) Verification: re-attach read-only and check per-chain residue counts.
ATTACH '3PLZ_AF.cif.gz' AS chk (TYPE mmcif);
USE chk;
SELECT auth_asym_id AS chain,
       count(DISTINCT label_seq_id) AS residues
FROM atom_site
WHERE group_PDB = 'ATOM' AND label_comp_id NOT IN ('HOH')
GROUP BY 1 ORDER BY 1;
USE memory;
DETACH chk;
