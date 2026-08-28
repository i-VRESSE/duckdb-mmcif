#!/usr/bin/env python3
"""Render an entity/relationship diagram from mmcif_relationships().

Runs the duckdb shell with the mmcif extension, queries
mmcif_relationships('<file>'), and emits either a Mermaid erDiagram or a
Graphviz DOT diagram to stdout.

Usage:
    python3 scripts/mmcif_relationships_diagram.py <structure.cif> [-f mermaid|dot] [-o out] [--duckdb path]

Examples:
    python3 scripts/mmcif_relationships_diagram.py test/data/1amb_updated.cif
    python3 scripts/mmcif_relationships_diagram.py test/data/1amb_updated.cif -f dot | dot -Tsvg -o rel.svg
"""

import argparse
import csv
import io
import os
import shutil
import subprocess
import sys
from collections import defaultdict


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


def query_relationships(duckdb_bin, cif_path):
    sql = f"SELECT DISTINCT parent_table, parent_column, child_table, child_column FROM mmcif_relationships('{cif_path}') ORDER BY ALL;"
    proc = subprocess.run(
        [duckdb_bin, "-csv", "-noheader"],
        input=sql,
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0:
        sys.exit(f"error: duckdb query failed:\n{proc.stderr}")
    rows = []
    for record in csv.reader(io.StringIO(proc.stdout)):
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


def render_mermaid(rows):
    edges = group_edges(rows)
    print("erDiagram")
    for (pt, ct), pairs in sorted(edges.items()):
        label = ", ".join(f"{p} = {c}" for p, c in pairs)
        print(f'    {pt} ||--o{{ {ct} : "{label}"')
    print()
    cols = column_labels(rows)
    for table in sorted(cols):
        print(f"    {table} {{")
        for col in sorted(cols[table]):
            print(f"        string {col}")
        print("    }")


def render_dot(rows):
    edges = group_edges(rows)
    tables = sorted({t for pair in rows for t in pair[::2]})
    print("digraph mmcif_relationships {")
    print("    rankdir=LR")
    print("    graph [fontname=Helvetica, nodesep=0.4, ranksep=1.2]")
    print('    node [shape=none, margin=0, fontname=Helvetica]')
    print('    edge [fontname=Helvetica, fontsize=10, arrowhead=crow]')
    for table in tables:
        print(f'    {table} [label=<')
        print("      <table border=\"0\" cellspacing=\"0\" cellborder=\"1\">")
        print(f"        <tr><td bgcolor=\"lightgrey\" ><b>{table}</b></td></tr>")
        for col in sorted(column_labels(rows)[table]):
            print(f"        <tr><td port=\"{col}\">{col}</td></tr>")
        print("      </table>")
        print("    >]")
    for (pt, ct), pairs in sorted(edges.items()):
        for pc, cc in pairs:
            print(f"    {pt}:{pc} -> {ct}:{cc}")
    print("}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0] if __doc__ else argparse.SUPPRESS)
    parser.add_argument("cif", help="mmCIF file passed to mmcif_relationships()")
    parser.add_argument("-f", "--format", choices=["mermaid", "dot"], default="mermaid")
    parser.add_argument("-o", "--output", help="output file (default: stdout)")
    parser.add_argument("--duckdb", help="path to the duckdb shell binary")
    args = parser.parse_args()

    rows = query_relationships(find_duckdb(args.duckdb), args.cif)

    out = open(args.output, "w") if args.output else sys.stdout
    old = sys.stdout
    sys.stdout = out
    try:
        if args.format == "mermaid":
            render_mermaid(rows)
        else:
            render_dot(rows)
    finally:
        sys.stdout = old
        if out is not sys.stdout:
            out.close()


if __name__ == "__main__":
    main()
