#!/usr/bin/env python3
"""Render an entity/relationship diagram from mmcif_relationships().

Runs the duckdb shell with the mmcif extension, queries
mmcif_relationships('<file>'), and emits either a Mermaid erDiagram or a
Graphviz DOT diagram to stdout.

Usage:
    python3 scripts/mmcif_relationships_diagram.py <structure.cif> [-f mermaid|dot] [-o out] [--duckdb path]

Examples:
    python3 scripts/mmcif_relationships_diagram.py test/data/1amb_updated.cif
    python3 scripts/mmcif_relationships_diagram.py test/data/1amb_updated.cif -f dot | dot -Tsvg -o docs/diagrams/pdbx.svg
"""

import argparse
import csv
import io
import json
import os
import shutil
import subprocess
import sys
from collections import defaultdict
from html import escape

from generate_type_index import parse_dict


def find_duckdb(explicit):
    if explicit:
        return explicit
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    for candidate in (
        os.path.join(repo_root, "build", "release", "duckdb"),
        os.path.join(repo_root, "build", "debug", "duckdb"),
    ):
        if os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return candidate
    found = shutil.which("duckdb")
    if found:
        return found
    sys.exit("error: duckdb shell not found; pass --duckdb <path>")


def query_records(duckdb_bin, sql):
    proc = subprocess.run(
        [duckdb_bin, "-csv", "-noheader"],
        input=sql,
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0:
        sys.exit(f"error: duckdb query failed:\n{proc.stderr}")
    return list(csv.reader(io.StringIO(proc.stdout)))


def query_relationships(duckdb_bin, cif_path):
    path = cif_path.replace("'", "''")
    sql = f"SELECT DISTINCT parent_table, parent_column, child_table, child_column FROM mmcif_relationships('{path}') ORDER BY ALL;"
    rows = []
    for record in query_records(duckdb_bin, sql):
        if len(record) == 4 and all(record):
            rows.append(tuple(record))
    if not rows:
        sys.exit("error: no relationships returned")
    return rows


def group_edges(rows):
    edges = defaultdict(list)
    for pt, pc, ct, cc in rows:
        pair = (pc, cc)
        if pair not in edges[(pt, ct)]:
            edges[(pt, ct)].append(pair)
    return edges


def column_labels(rows):
    cols = defaultdict(set)
    for pt, pc, ct, cc in rows:
        cols[pt].add(pc)
        cols[ct].add(cc)
    return cols


def render_mermaid(rows, tables=None):
    edges = group_edges(rows)
    print("erDiagram")
    for (pt, ct), pairs in sorted(edges.items()):
        label = ", ".join(f"{p} = {c}" for p, c in pairs)
        print(f'    {pt} ||--o{{ {ct} : "{label}"')
    print()
    cols = column_labels(rows)
    for table in sorted(cols if tables is None else tables):
        print(f"    {table} {{")
        for col in sorted(cols[table]):
            print(f"        string {col}")
        print("    }")


def render_dot(rows, tables=None):
    edges = group_edges(rows)
    tables = sorted({t for pair in rows for t in pair[::2]} if tables is None else tables)
    print("digraph mmcif_relationships {")
    print("    rankdir=LR")
    print("    graph [fontname=Helvetica, nodesep=0.4, ranksep=1.2]")
    print("    node [shape=none, margin=0, fontname=Helvetica]")
    print("    edge [fontname=Helvetica, fontsize=10, arrowhead=crow]")
    for table in tables:
        print(f"    {json.dumps(table)} [label=<")
        print('      <table border="0" cellspacing="0" cellborder="1">')
        print(f'        <tr><td bgcolor="lightgrey" ><b>{escape(table)}</b></td></tr>')
        for col in sorted(column_labels(rows)[table]):
            print(f'        <tr><td port="{escape(col)}">{escape(col)}</td></tr>')
        print("      </table>")
        print("    >]")
    for (pt, ct), pairs in sorted(edges.items()):
        for pc, cc in pairs:
            print(f"    {json.dumps(pt)}:{json.dumps(pc)} -> {json.dumps(ct)}:{json.dumps(cc)}")
    print("}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0] if __doc__ else argparse.SUPPRESS)
    parser.add_argument("cif", help="mmCIF file passed to mmcif_relationships()")
    parser.add_argument("-f", "--format", choices=["mermaid", "dot"], default="mermaid")
    parser.add_argument("-o", "--output", help="output file (default: stdout)")
    parser.add_argument("--duckdb", help="path to the duckdb shell binary")
    parser.add_argument("--dictionary", help="restrict categories to those defined in this source dictionary (.dic)")
    parser.add_argument("--category-prefix", help="restrict categories to this prefix (case-insensitive)")
    args = parser.parse_args()

    duckdb_bin = find_duckdb(args.duckdb)
    rows = query_relationships(duckdb_bin, args.cif)
    tables = None
    if args.dictionary or args.category_prefix:
        path = args.cif.replace("'", "''")
        tables = {
            record[0]
            for record in query_records(
                duckdb_bin, f"SELECT table_name FROM mmcif_tables('{path}') ORDER BY table_name;"
            )
            if len(record) == 1 and record[0]
        }
        if args.dictionary:
            _, categories, _, _ = parse_dict(args.dictionary)
            tables = {table for table in tables if table.lower() in categories}
        if args.category_prefix:
            tables = {table for table in tables if table.lower().startswith(args.category_prefix.lower())}
        if not tables:
            sys.exit("error: no matching categories in the CIF file")
        allowed = {table.lower() for table in tables}
        rows = [row for row in rows if row[0].lower() in allowed and row[2].lower() in allowed]

    out = open(args.output, "w") if args.output else sys.stdout
    old = sys.stdout
    sys.stdout = out
    try:
        if args.format == "mermaid":
            render_mermaid(rows, tables)
        else:
            render_dot(rows, tables)
    finally:
        sys.stdout = old
        if out is not sys.stdout:
            out.close()


if __name__ == "__main__":
    main()
