-- Secondary-structure statistics, mirroring protein-quest's
-- filters/ss.py::nr_of_residues_in_helix / nr_of_residues_in_sheet, which
-- walk gemmi.Structure.helices and gemmi.Structure.sheets.
--
-- Gemmi builds those lists from the mmCIF categories:
--   * helices: struct_conf rows with conf_type_id = 'HELX_P' (alpha helix),
--     each spanning beg_label_seq_id .. end_label_seq_id
--   * sheets:   struct_sheet_range rows grouped by sheet_id (one strand each)
--     each spanning beg_label_seq_id .. end_label_seq_id
-- and counts residues as (end - start + 1), the same arithmetic done here.
--
-- Structure: 3PLZ — two protein chains (auth A and B) plus a short fragment
-- chain (auth C). 26 HELX_P helices, 2 beta sheets.
-- Source file: https://files.rcsb.org/download/3PLZ.cif.gz
--
-- Run from the repository root with:
--   ./build/release/duckdb < docs/examples/secondary_structure.sql

ATTACH '3PLZ.cif.gz' AS cif (TYPE mmcif);
USE cif;

-- 1) Helix residues — sum over struct_conf rows of conf_type HELX_P.
--    (AlphaFold cif files may leave helix length unset; gemmi falls back to
--    resid arithmetic, so end-start+1 is the portable count.)
SELECT count(*)                          AS nr_helices,
       sum(CAST(end_label_seq_id AS BIGINT)
           - CAST(beg_label_seq_id AS BIGINT) + 1) AS nr_helix_residues
FROM struct_conf
WHERE conf_type_id = 'HELX_P';

-- 2) Sheet residues — one struct_sheet_range row per strand; sum the spans.
SELECT count(*)                          AS nr_strands,
       sum(CAST(end_label_seq_id AS BIGINT)
           - CAST(beg_label_seq_id AS BIGINT) + 1) AS nr_sheet_residues
FROM struct_sheet_range;

-- 3) Per-helix / per-strand spans — what gemmi's Structure.helices/sheets
--    expose, as plain rows (beg/end label seq ids).
SELECT 'helix' AS kind,
       beg_label_asym_id AS chain,
       beg_label_seq_id  AS start,
       end_label_seq_id  AS finish
FROM struct_conf WHERE conf_type_id = 'HELX_P'
UNION ALL
SELECT 'strand', beg_label_asym_id, beg_label_seq_id, end_label_seq_id
FROM struct_sheet_range
ORDER BY kind, chain, start;

-- 4) Secondary-structure ratios — the SecondaryStructureStats dataclass:
--    helix_ratio / sheet_ratio against the total residue count.
WITH totals AS (
    SELECT count(DISTINCT label_asym_id || ':' || label_seq_id) AS total
    FROM atom_site WHERE group_PDB = 'ATOM'
),
helix AS (
    SELECT sum(CAST(end_label_seq_id AS BIGINT)
               - CAST(beg_label_seq_id AS BIGINT) + 1) AS n
    FROM struct_conf WHERE conf_type_id = 'HELX_P'
),
sheet AS (
    SELECT sum(CAST(end_label_seq_id AS BIGINT)
               - CAST(beg_label_seq_id AS BIGINT) + 1) AS n
    FROM struct_sheet_range
)
SELECT total                       AS nr_residues,
       h.n                         AS nr_helix_residues,
       s.n                         AS nr_sheet_residues,
       round(h.n / total, 3)       AS helix_ratio,
       round(s.n / total, 3)       AS sheet_ratio
FROM totals, helix h, sheet s;

-- 5) Apply the SecondaryStructureFilterQuery thresholds in SQL — e.g. keep
--    structures with >= 200 helix residues and <= 0.5 sheet ratio. (Shown on
--    3PLZ; a single boolean to filter a directory of files.)
WITH totals AS (
    SELECT count(DISTINCT label_asym_id || ':' || label_seq_id) AS total
    FROM atom_site WHERE group_PDB = 'ATOM'
),
helix AS (
    SELECT sum(CAST(end_label_seq_id AS BIGINT)
               - CAST(beg_label_seq_id AS BIGINT) + 1) AS n
    FROM struct_conf WHERE conf_type_id = 'HELX_P'
),
sheet AS (
    SELECT sum(CAST(end_label_seq_id AS BIGINT)
               - CAST(beg_label_seq_id AS BIGINT) + 1) AS n
    FROM struct_sheet_range
)
SELECT (h.n >= 200 AND s.n / total <= 0.5) AS passed
FROM totals, helix h, sheet s;
