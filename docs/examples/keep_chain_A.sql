-- Keep only chain A (auth_asym_id = 'A') from the mmCIF file 3PLZ_A.cif.gz.
-- Source file: https://files.rcsb.org/download/3PLZ.cif.gz
-- 3PLZ_A.cif.gz is a copy of 3PLZ.cif.gz, so the original is never touched.
-- Create the copy first:  cp 3PLZ.cif.gz 3PLZ_A.cif.gz
-- Run from the repository root with:
--   ./build/release/duckdb < docs/examples/keep_chain_A.sql

ATTACH '3PLZ_A.cif.gz' AS cif (TYPE mmcif, READ_WRITE TRUE);
USE cif;

BEGIN;

-- Reference sets, computed before anything is deleted.
CREATE TEMP TABLE chain_a_labels AS
SELECT DISTINCT label_asym_id FROM atom_site WHERE auth_asym_id = 'A';

CREATE TEMP TABLE kept_entities AS
SELECT DISTINCT label_entity_id FROM atom_site WHERE auth_asym_id = 'A';

-- atom_site: the chain itself
DELETE FROM atom_site WHERE auth_asym_id <> 'A';

-- Per-atom / per-residue records that carry an auth chain id
DELETE FROM pdbx_unobs_or_zero_occ_atoms      WHERE auth_asym_id <> 'A';
DELETE FROM pdbx_unobs_or_zero_occ_residues   WHERE auth_asym_id <> 'A';
DELETE FROM pdbx_validate_torsion             WHERE auth_asym_id <> 'A';
DELETE FROM struct_site_gen                   WHERE auth_asym_id <> 'A';
DELETE FROM struct_site                       WHERE pdbx_auth_asym_id <> 'A';

-- Segments: drop rows unless both ends are chain A
DELETE FROM struct_conf           WHERE beg_auth_asym_id <> 'A' OR end_auth_asym_id <> 'A';
DELETE FROM struct_sheet_range    WHERE beg_auth_asym_id <> 'A' OR end_auth_asym_id <> 'A';
DELETE FROM pdbx_refine_tls_group WHERE beg_auth_asym_id <> 'A' OR end_auth_asym_id <> 'A';
DELETE FROM pdbx_struct_sheet_hbond
      WHERE range_1_auth_asym_id <> 'A' OR range_2_auth_asym_id <> 'A';

-- Chain-instance schemes (pdb_strand_id is the auth chain)
DELETE FROM pdbx_poly_seq_scheme   WHERE pdb_strand_id <> 'A';
DELETE FROM pdbx_nonpoly_scheme    WHERE pdb_strand_id <> 'A';

-- struct_asym is keyed by label_asym_id
DELETE FROM struct_asym
      WHERE id NOT IN (SELECT label_asym_id FROM chain_a_labels);

-- Assemblies: drop any assembly whose chain list is not entirely chain A,
-- then its dependent rows.
DELETE FROM pdbx_struct_assembly_gen
      WHERE EXISTS (
        SELECT 1 FROM (SELECT UNNEST(string_split(asym_id_list, ',')) AS lab)
        WHERE lab NOT IN (SELECT label_asym_id FROM chain_a_labels));
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

-- Sequence references follow the entities
DELETE FROM struct_ref            WHERE entity_id NOT IN (SELECT label_entity_id FROM kept_entities);
DELETE FROM struct_ref_seq        WHERE ref_id NOT IN (SELECT id FROM struct_ref WHERE id IS NOT NULL);
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

COMMIT;   -- writes the filtered CifFile back to 3PLZ_A.cif.gz
USE memory;
DETACH cif;

-- Verification
ATTACH '3PLZ_A.cif.gz' AS chk (TYPE mmcif);
USE chk;
SELECT 'atom_site' AS t, count(*) AS n, count(*) FILTER (auth_asym_id <> 'A') AS not_chain_a FROM atom_site
UNION ALL SELECT 'pdbx_unobs_or_zero_occ_atoms', count(*), count(*) FILTER (auth_asym_id <> 'A') FROM pdbx_unobs_or_zero_occ_atoms
UNION ALL SELECT 'pdbx_unobs_or_zero_occ_residues', count(*), count(*) FILTER (auth_asym_id <> 'A') FROM pdbx_unobs_or_zero_occ_residues
UNION ALL SELECT 'struct_site_gen', count(*), count(*) FILTER (auth_asym_id <> 'A') FROM struct_site_gen
UNION ALL SELECT 'struct_asym', count(*), count(*) FILTER (id NOT IN ('A','E','F','I')) FROM struct_asym
UNION ALL SELECT 'entity', count(*), 0 FROM entity;
USE memory;
DETACH chk;
