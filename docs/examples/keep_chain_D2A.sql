-- Keep only chain D (auth_asym_id = 'D') from the mmCIF file 3PLZ.cif.gz, then
-- rename the remaining chain D to chain A.
-- Source file: https://files.rcsb.org/download/3PLZ.cif.gz
-- 3PLZ_D2A.cif.gz is a copy of 3PLZ.cif.gz, so the original is never touched.
-- Create the copy first:  cp 3PLZ.cif.gz 3PLZ_D2A.cif.gz
-- Run from the repository root with:
--   ./build/release/duckdb < docs/examples/keep_chain_D2A.sql
--
-- 3PLZ has four auth chains (A, B, C, D); chain D is a short 14-residue
-- fragment plus 10 waters. After this script runs, 3PLZ_D2A.cif.gz contains
-- only the chain-D atoms, and every 'D' chain id has been rewritten to 'A'
-- in both the auth (auth_asym_id / pdb_strand_id) and label (label_asym_id /
-- asym_id / struct_asym.id) namespaces, so it reads as a plain chain-A file.

-- 0) Discovery: which tables carry a chain id? mmcif_relationships() maps the
--    foreign keys, so we can find every table whose auth-chain column points
--    at atom_site.auth_asym_id, and every table whose label-chain column
--    points at atom_site.label_asym_id / struct_asym.id.
SELECT child_table AS t, child_column AS col, 'auth' AS ns
FROM mmcif_relationships('3PLZ.cif.gz')
WHERE parent_table = 'atom_site' AND parent_column = 'auth_asym_id'
UNION ALL
SELECT child_table, child_column, 'label'
FROM mmcif_relationships('3PLZ.cif.gz')
WHERE parent_table = 'atom_site' AND parent_column = 'label_asym_id'
UNION ALL
SELECT child_table, child_column, 'label'
FROM mmcif_relationships('3PLZ.cif.gz')
WHERE parent_table = 'struct_asym' AND parent_column = 'id'
ORDER BY t;

ATTACH '3PLZ_D2A.cif.gz' AS cif (TYPE mmcif, READ_WRITE TRUE);
USE cif;

BEGIN;

-- Reference sets, computed before anything is deleted.
CREATE TEMP TABLE chain_d_labels AS
SELECT DISTINCT label_asym_id FROM atom_site WHERE auth_asym_id = 'D';

CREATE TEMP TABLE kept_entities AS
SELECT DISTINCT label_entity_id FROM atom_site WHERE auth_asym_id = 'D';

-- atom_site: the chain itself
DELETE FROM atom_site WHERE auth_asym_id <> 'D';

-- Per-atom / per-residue records that carry an auth chain id
DELETE FROM pdbx_unobs_or_zero_occ_atoms      WHERE auth_asym_id <> 'D';
DELETE FROM pdbx_unobs_or_zero_occ_residues   WHERE auth_asym_id <> 'D';
DELETE FROM pdbx_validate_torsion             WHERE auth_asym_id <> 'D';
DELETE FROM struct_site_gen                   WHERE auth_asym_id <> 'D';
DELETE FROM struct_site                       WHERE pdbx_auth_asym_id <> 'D';

-- Segments: drop rows unless both ends are chain D
DELETE FROM struct_conf           WHERE beg_auth_asym_id <> 'D' OR end_auth_asym_id <> 'D';
DELETE FROM struct_sheet_range    WHERE beg_auth_asym_id <> 'D' OR end_auth_asym_id <> 'D';
DELETE FROM pdbx_refine_tls_group WHERE beg_auth_asym_id <> 'D' OR end_auth_asym_id <> 'D';
DELETE FROM pdbx_struct_sheet_hbond
      WHERE range_1_auth_asym_id <> 'D' OR range_2_auth_asym_id <> 'D';

-- Chain-instance schemes (pdb_strand_id is the auth chain)
DELETE FROM pdbx_poly_seq_scheme   WHERE pdb_strand_id <> 'D';
DELETE FROM pdbx_nonpoly_scheme    WHERE pdb_strand_id <> 'D';

-- struct_asym is keyed by label_asym_id
DELETE FROM struct_asym
      WHERE id NOT IN (SELECT label_asym_id FROM chain_d_labels);

-- Assemblies: drop any assembly whose chain list is not entirely chain D,
-- then its dependent rows.
DELETE FROM pdbx_struct_assembly_gen
      WHERE EXISTS (
        SELECT 1 FROM (SELECT UNNEST(string_split(asym_id_list, ',')) AS lab)
        WHERE lab NOT IN (SELECT label_asym_id FROM chain_d_labels));
DELETE FROM pdbx_struct_assembly_prop
      WHERE biol_id NOT IN (SELECT assembly_id FROM pdbx_struct_assembly_gen WHERE assembly_id IS NOT NULL);
DELETE FROM pdbx_struct_assembly
      WHERE id NOT IN (SELECT assembly_id FROM pdbx_struct_assembly_gen WHERE assembly_id IS NOT NULL);
DELETE FROM pdbx_struct_oper_list
      WHERE id NOT IN (SELECT oper_expression FROM pdbx_struct_assembly_gen WHERE oper_expression IS NOT NULL);

-- Parents that may now have no children left
DELETE FROM struct_sheet
      WHERE id NOT IN (SELECT sheet_id FROM struct_sheet_range WHERE sheet_id IS NOT NULL);
DELETE FROM struct_site
      WHERE id NOT IN (SELECT site_id FROM struct_site_gen WHERE site_id IS NOT NULL);
DELETE FROM pdbx_refine_tls
      WHERE id NOT IN (SELECT refine_tls_id FROM pdbx_refine_tls_group WHERE refine_tls_id IS NOT NULL);
DELETE FROM struct_conf_type
      WHERE id NOT IN (SELECT conf_type_id FROM struct_conf WHERE conf_type_id IS NOT NULL);

-- Entity-scoped tables: keep only entities still referenced by the remaining atoms
DELETE FROM entity_name_com       WHERE entity_id NOT IN (SELECT label_entity_id FROM kept_entities);
DELETE FROM entity_poly           WHERE entity_id NOT IN (SELECT label_entity_id FROM kept_entities);
DELETE FROM entity_poly_seq       WHERE entity_id NOT IN (SELECT label_entity_id FROM kept_entities);
DELETE FROM entity_src_gen        WHERE entity_id NOT IN (SELECT label_entity_id FROM kept_entities);
DELETE FROM pdbx_entity_src_syn   WHERE entity_id NOT IN (SELECT label_entity_id FROM kept_entities);
DELETE FROM pdbx_entity_nonpoly   WHERE entity_id NOT IN (SELECT label_entity_id FROM kept_entities);

-- Sequence references follow the entities; then drop any strand rows that
-- still name a removed chain (entity 2 spans auth chains C and D).
DELETE FROM struct_ref            WHERE entity_id NOT IN (SELECT label_entity_id FROM kept_entities);
DELETE FROM struct_ref_seq        WHERE ref_id NOT IN (SELECT id FROM struct_ref WHERE id IS NOT NULL)
                              OR pdbx_strand_id <> 'D';
DELETE FROM struct_ref_seq_dif    WHERE align_id NOT IN (SELECT align_id FROM struct_ref_seq WHERE align_id IS NOT NULL);

DELETE FROM entity
      WHERE id NOT IN (SELECT label_entity_id FROM kept_entities);

-- Chemical components: keep only those used by the remaining atoms or polymer entities
DELETE FROM chem_comp_atom
      WHERE comp_id NOT IN (
        SELECT label_comp_id FROM atom_site WHERE label_comp_id IS NOT NULL
        UNION
        SELECT mon_id FROM entity_poly_seq WHERE mon_id IS NOT NULL);
DELETE FROM chem_comp_bond
      WHERE comp_id NOT IN (
        SELECT label_comp_id FROM atom_site WHERE label_comp_id IS NOT NULL
        UNION
        SELECT mon_id FROM entity_poly_seq WHERE mon_id IS NOT NULL);
DELETE FROM chem_comp
      WHERE id NOT IN (
        SELECT label_comp_id FROM atom_site WHERE label_comp_id IS NOT NULL
        UNION
        SELECT mon_id FROM entity_poly_seq WHERE mon_id IS NOT NULL);

-- entity_poly.pdbx_strand_id lists every auth chain of an entity ("C,D" for
-- entity 2); drop the removed chain C, leaving only the kept chain D, then
-- the rename below turns it into 'A'.
UPDATE entity_poly
      SET pdbx_strand_id = 'D'
      WHERE entity_id IN (SELECT label_entity_id FROM kept_entities);

-- Rename chain D -> A. Every remaining auth chain id is 'D' (and every
-- remaining label id is 'D' or 'L'), so these rewrites are unambiguous.

-- Auth-chain columns
UPDATE atom_site                 SET auth_asym_id          = 'A' WHERE auth_asym_id          = 'D';
UPDATE pdbx_unobs_or_zero_occ_atoms    SET auth_asym_id    = 'A' WHERE auth_asym_id    = 'D';
UPDATE pdbx_unobs_or_zero_occ_residues SET auth_asym_id    = 'A' WHERE auth_asym_id    = 'D';
UPDATE pdbx_validate_torsion     SET auth_asym_id          = 'A' WHERE auth_asym_id          = 'D';
UPDATE struct_site_gen           SET auth_asym_id          = 'A' WHERE auth_asym_id          = 'D';
UPDATE struct_site               SET pdbx_auth_asym_id     = 'A' WHERE pdbx_auth_asym_id     = 'D';
UPDATE struct_conf               SET beg_auth_asym_id      = 'A' WHERE beg_auth_asym_id      = 'D';
UPDATE struct_conf               SET end_auth_asym_id      = 'A' WHERE end_auth_asym_id      = 'D';
UPDATE struct_sheet_range        SET beg_auth_asym_id      = 'A' WHERE beg_auth_asym_id      = 'D';
UPDATE struct_sheet_range        SET end_auth_asym_id      = 'A' WHERE end_auth_asym_id      = 'D';
UPDATE pdbx_refine_tls_group     SET beg_auth_asym_id      = 'A' WHERE beg_auth_asym_id      = 'D';
UPDATE pdbx_refine_tls_group     SET end_auth_asym_id      = 'A' WHERE end_auth_asym_id      = 'D';
UPDATE pdbx_struct_sheet_hbond   SET range_1_auth_asym_id  = 'A' WHERE range_1_auth_asym_id  = 'D';
UPDATE pdbx_struct_sheet_hbond   SET range_2_auth_asym_id  = 'A' WHERE range_2_auth_asym_id  = 'D';
UPDATE pdbx_poly_seq_scheme      SET pdb_strand_id         = 'A' WHERE pdb_strand_id         = 'D';
UPDATE pdbx_nonpoly_scheme       SET pdb_strand_id         = 'A' WHERE pdb_strand_id         = 'D';
UPDATE struct_ref_seq            SET pdbx_strand_id        = 'A' WHERE pdbx_strand_id        = 'D';
UPDATE struct_ref_seq_dif        SET pdbx_pdb_strand_id    = 'A' WHERE pdbx_pdb_strand_id    = 'D';
UPDATE entity_poly               SET pdbx_strand_id        = 'A' WHERE pdbx_strand_id        = 'D';

-- Label-chain columns
UPDATE atom_site                 SET label_asym_id          = 'A' WHERE label_asym_id          = 'D';
UPDATE pdbx_unobs_or_zero_occ_atoms    SET label_asym_id    = 'A' WHERE label_asym_id    = 'D';
UPDATE pdbx_unobs_or_zero_occ_residues SET label_asym_id    = 'A' WHERE label_asym_id    = 'D';
UPDATE struct_site_gen           SET label_asym_id          = 'A' WHERE label_asym_id          = 'D';
UPDATE struct_conf               SET beg_label_asym_id      = 'A' WHERE beg_label_asym_id      = 'D';
UPDATE struct_conf               SET end_label_asym_id      = 'A' WHERE end_label_asym_id      = 'D';
UPDATE struct_sheet_range        SET beg_label_asym_id      = 'A' WHERE beg_label_asym_id      = 'D';
UPDATE struct_sheet_range        SET end_label_asym_id      = 'A' WHERE end_label_asym_id      = 'D';
UPDATE pdbx_refine_tls_group     SET beg_label_asym_id      = 'A' WHERE beg_label_asym_id      = 'D';
UPDATE pdbx_refine_tls_group     SET end_label_asym_id      = 'A' WHERE end_label_asym_id      = 'D';
UPDATE pdbx_struct_sheet_hbond   SET range_1_label_asym_id  = 'A' WHERE range_1_label_asym_id  = 'D';
UPDATE pdbx_struct_sheet_hbond   SET range_2_label_asym_id  = 'A' WHERE range_2_label_asym_id  = 'D';
UPDATE pdbx_poly_seq_scheme      SET asym_id                = 'A' WHERE asym_id                = 'D';
UPDATE pdbx_nonpoly_scheme       SET asym_id                = 'A' WHERE asym_id                = 'D';
UPDATE struct_asym               SET id                     = 'A' WHERE id                     = 'D';

COMMIT;   -- writes the filtered, renamed CifFile back to 3PLZ_D2A.cif.gz
USE memory;
DETACH cif;

-- Verification.
-- NOTE: mmCIF tables that end up with zero rows (pdbx_unobs_or_zero_occ_atoms,
-- struct_site_gen, pdbx_validate_torsion, pdbx_struct_sheet_hbond, ...) are
-- omitted from the written file, so only tables still present are checked.
ATTACH '3PLZ_D2A.cif.gz' AS chk (TYPE mmcif);
USE chk;
SELECT 'atom_site' AS t, count(*) AS n, count(*) FILTER (auth_asym_id <> 'A') AS not_chain_a FROM atom_site
UNION ALL SELECT 'pdbx_unobs_or_zero_occ_residues', count(*), count(*) FILTER (auth_asym_id <> 'A') FROM pdbx_unobs_or_zero_occ_residues
UNION ALL SELECT 'struct_conf', count(*), count(*) FILTER (beg_auth_asym_id <> 'A') FROM struct_conf
UNION ALL SELECT 'pdbx_refine_tls_group', count(*), count(*) FILTER (beg_auth_asym_id <> 'A') FROM pdbx_refine_tls_group
UNION ALL SELECT 'pdbx_poly_seq_scheme', count(*), count(*) FILTER (pdb_strand_id <> 'A') FROM pdbx_poly_seq_scheme
UNION ALL SELECT 'pdbx_nonpoly_scheme', count(*), count(*) FILTER (pdb_strand_id <> 'A') FROM pdbx_nonpoly_scheme
UNION ALL SELECT 'struct_asym', count(*), count(*) FILTER (id NOT IN ('A','L')) FROM struct_asym
UNION ALL SELECT 'entity', count(*), 0 FROM entity;
USE memory;
DETACH chk;
