-- Spatial analysis of atom_site coordinates with the DuckDB spatial extension.
--
-- Structure: 3PLZ — human LRH1 ligand-binding domain bound to the inhibitor
-- GR470 (chem comp "470"); two protein chains (auth A and B), each with one
-- inhibitor, plus EDO cryoprotectant and waters.
-- Source file: https://files.rcsb.org/download/3PLZ.cif.gz
--
-- Run from the repository root with:
--   ./build/release/duckdb < docs/examples/spatial_atoms.sql
-- (first time only: INSTALL spatial;)
--
-- A NOTE ON DIMENSIONS
-- All spatial extension distance/predicate functions are planar: they ignore
-- the Z coordinate (st_distance('POINT Z(0 0 0)', 'POINT Z(3 4 5)') = 5.0,
-- not sqrt(50)). So this example:
--   * builds true 3D points with st_point3d(Cartn_x, Cartn_y, Cartn_z) and
--     casts them to GEOMETRY for the spatial functions,
--   * measures real Ångström distances with the d3d macro below, built on the
--     st_x/st_y/st_z accessors,
--   * uses st_dwithin (XY projection) as a cheap *superset* pre-filter for
--     3D neighbour searches, since the projected distance can never exceed
--     the true 3D distance.

LOAD spatial;

ATTACH '3PLZ.cif.gz' AS cif (TYPE mmcif);
USE cif;

-- One row per atom with a 3D geometry column
CREATE TEMP TABLE atoms AS
SELECT auth_asym_id,
       label_comp_id,
       auth_seq_id,
       label_atom_id,
       type_symbol,
       st_point3d(Cartn_x, Cartn_y, Cartn_z)::GEOMETRY AS geom
FROM atom_site
WHERE Cartn_x IS NOT NULL;

-- Exact 3D Euclidean distance in Ångström
CREATE TEMP MACRO d3d(a, b) AS
    sqrt(power(st_x(a) - st_x(b), 2)
       + power(st_y(a) - st_y(b), 2)
       + power(st_z(a) - st_z(b), 2));

-- 1) Overall size of the structure: bounding box (spatial aggregate for XY,
--    st_z accessors for the depth).
WITH bb AS (
    SELECT st_extent_agg(geom) AS box,
           min(st_z(geom))     AS zmin,
           max(st_z(geom))     AS zmax
    FROM atoms
)
SELECT round(st_xmax(box) - st_xmin(box), 1) AS width_x,
       round(st_ymax(box) - st_ymin(box), 1) AS width_y,
       round(zmax - zmin, 1)                 AS depth_z
FROM bb;

-- 2) Binding pocket of the chain-A inhibitor: every amino-acid residue of
--    chain A with an atom within 4.0 Å of any atom of "470", ranked by
--    minimum distance. st_dwithin (planar XY) cheaply prunes candidate pairs
--    before the exact 3D cutoff is applied.
WITH lig AS (SELECT geom FROM atoms WHERE label_comp_id = '470' AND auth_asym_id = 'A'),
     contacts AS (
    SELECT a.auth_seq_id, a.label_comp_id, a.label_atom_id, d3d(l.geom, a.geom) AS d
    FROM lig l
    CROSS JOIN atoms a
    WHERE a.auth_asym_id = 'A' AND a.label_comp_id NOT IN ('470', 'HOH')
      AND st_dwithin(l.geom, a.geom, 4.0)   -- planar superset pre-filter
      AND d3d(l.geom, a.geom) <= 4.0        -- exact 3D cutoff
)
SELECT auth_seq_id              AS seq,
       label_comp_id            AS res,
       count(DISTINCT label_atom_id) AS atoms_in_contact,
       round(min(d), 2)         AS min_dist
FROM contacts
GROUP BY 1, 2
ORDER BY min_dist;

-- 3) For each pocket residue, which inhibitor atom does it touch most closely?
--    (first row of each residue ordered by distance, via QUALIFY)
WITH lig AS (SELECT geom, label_atom_id FROM atoms WHERE label_comp_id = '470' AND auth_asym_id = 'A'),
     contacts AS (
    SELECT a.auth_seq_id,
           a.label_comp_id,
           l.label_atom_id  AS ligand_atom,
           d3d(l.geom, a.geom) AS d
    FROM lig l
    CROSS JOIN atoms a
    WHERE a.auth_asym_id = 'A' AND a.label_comp_id NOT IN ('470', 'HOH')
      AND st_dwithin(l.geom, a.geom, 4.0)
      AND d3d(l.geom, a.geom) <= 4.0
)
SELECT auth_seq_id AS seq, label_comp_id AS res, ligand_atom, round(d, 2) AS dist
FROM contacts
QUALIFY row_number() OVER (PARTITION BY auth_seq_id ORDER BY d) = 1
ORDER BY d;

-- 4) Chain A / chain B interface: residue pairs from the two copies whose atoms
--    are within 4.0 Å of each other. st_dwithin (planar) prunes most candidate
--    pairs first; d3d then applies the exact 3D cutoff.
SELECT p.label_comp_id || p.auth_seq_id AS res_a,
       q.label_comp_id || q.auth_seq_id AS res_b,
       count(*)                         AS atom_pairs,
       round(min(d3d(p.geom, q.geom)), 2) AS min_dist
FROM atoms p
CROSS JOIN atoms q
WHERE p.auth_asym_id = 'A' AND q.auth_asym_id = 'B'
  AND p.label_comp_id NOT IN ('470', 'HOH')
  AND q.label_comp_id NOT IN ('470', 'HOH')
  AND st_dwithin(p.geom, q.geom, 4.0)   -- planar superset pre-filter
  AND d3d(p.geom, q.geom) <= 4.0        -- exact 3D check
GROUP BY 1, 2
ORDER BY min_dist;

-- 5) First hydration shell of the chain-A inhibitor: ordered waters within
--    3.5 Å.
WITH lig AS (SELECT geom FROM atoms WHERE label_comp_id = '470' AND auth_asym_id = 'A'),
     hydrated AS (
    SELECT w.auth_seq_id, d3d(l.geom, w.geom) AS d
    FROM lig l
    CROSS JOIN atoms w
    WHERE w.label_comp_id = 'HOH' AND w.auth_asym_id = 'A'
      AND st_dwithin(l.geom, w.geom, 3.5)
      AND d3d(l.geom, w.geom) <= 3.5
)
SELECT auth_seq_id      AS water_seq,
       count(*)         AS ligand_atom_contacts,
       round(min(d), 2) AS min_dist
FROM hydrated
GROUP BY 1
ORDER BY min_dist;

-- 6) Rigid-body geometry: move the chain-A inhibitor's centroid to the origin
--    and rotate it 90° about the Z axis, in one ST_Affine call (3x3 matrix +
--    translation; Z is preserved). Useful for superposition workflows.
WITH lig AS (SELECT geom, label_atom_id FROM atoms WHERE label_comp_id = '470' AND auth_asym_id = 'A'),
     centre AS (
    SELECT avg(st_x(geom)) AS cx, avg(st_y(geom)) AS cy, avg(st_z(geom)) AS cz FROM lig
),
xform AS (
    SELECT label_atom_id,
           geom AS before,
           -- R_z(90°) applied after translating the centroid to the origin:
           st_affine(geom,
                     0, -1, 0,
                     1,  0, 0,
                     0,  0, 1,
                     cy, -cx, -cz) AS after
    FROM lig, centre
)
SELECT label_atom_id,
       st_astext(before) AS before,
       st_astext(after)  AS after
FROM xform
LIMIT 5;

-- 7) Export-ready geometry: the C-alpha trace of each protein chain as a
--    single LINESTRING (projected to 2D, since st_makeline rejects Z), ready
--    for st_asgeojson / a map viewer, plus per-chain atom counts.
SELECT auth_asym_id,
       count(*)                                          AS ca_atoms,
       st_npoints(st_makeline(list(st_force2d(geom) ORDER BY auth_seq_id)))    AS ca_trace_points
FROM atoms
WHERE label_atom_id = 'CA' AND label_comp_id NOT IN ('HOH')
GROUP BY 1
ORDER BY ca_atoms DESC;

USE memory;
DETACH cif;
