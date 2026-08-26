#!/usr/bin/env python3
"""Generate the mmcif dictionary type index and relationship artifacts.

Reads mmcif_pdbx_v50.dic (an mmcif file) and emits two compact gzip'd, sorted TSV
artifacts:

  * type index: each dictionary item `_category.item` -> its DuckDB column type
  * relationships: parent_category_id -> child_category_id pairs from
    `_pdbx_item_linked_group_list`

Run off-repo / in CI on a version bump (rare); the artifacts are checked in and
shipped with the extension. The build never downloads or parses the dictionary.

Usage:
    python3 scripts/generate_type_index.py <mmcif_pdbx_v50.dic> <type.tsv.gz> <rel.tsv.gz>
"""

import sys
import gzip

SOURCE_URL = "https://mmcif.wwpdb.org/dictionaries/ascii/mmcif_pdbx_v50.dic"
DIC_VERSION = "5.416"

# dictionary type code -> DuckDB type (research 03 / ticket 03)
NUMERIC_FLOAT = {"float", "float-range"}
NUMERIC_INT = {"int", "positive_int", "int_list", "int-range"}
# everything else -> VARCHAR; unknown (no _item_type.code) -> VARCHAR


def duckdb_type(code):
    if code in NUMERIC_FLOAT:
        return "DOUBLE"
    if code in NUMERIC_INT:
        return "BIGINT"
    return "VARCHAR"


def tokenize(line):
    """Split an mmcif line into whitespace-delimited tokens, respecting double quotes."""
    tokens = []
    i = 0
    n = len(line)
    while i < n:
        if line[i] in " \t":
            i += 1
            continue
        if line[i] == '"':
            # quoted token
            j = i + 1
            while j < n and line[j] != '"':
                j += 1
            tokens.append(line[i + 1 : j])
            i = j + 1
            continue
        j = i
        while j < n and line[j] not in " \t":
            j += 1
        tokens.append(line[i:j])
        i = j
    return tokens


def parse_dict(path):
    """Return (items, relationships).

    items: list of (item_name, type_code) for every save_<item> saveframe.
    relationships: set of (parent_category_id, child_category_id) from the
    _pdbx_item_linked_group_list loop.
    """
    item_type = None  # current saveframe's _item_type.code
    pdbx_item_type = None  # current saveframe's _pdbx_item_type.code
    item_name = None  # current saveframe's _item.name
    items = []
    in_save = False

    rel_header = []  # column names of the _pdbx_item_linked_group_list loop
    in_rel_loop = False
    rel_loop_cols = None  # index of each column in the loop header
    relationships = set()
    rel_buf = []  # running token buffer for the current data rows
    rel_ncols = 0

    with open(path, encoding="utf-8") as f:
        for raw in f:
            line = raw.strip()
            if line == "save_":
                if item_name:
                    # pdbx_item_type override wins over item_type (getTypeCodeAlt priority)
                    code = pdbx_item_type if pdbx_item_type else item_type
                    items.append((item_name, code))
                in_save = False
                continue
            if line.startswith("save_"):
                in_save = True
                item_type = None
                pdbx_item_type = None
                item_name = None
                continue
            if in_save:
                if line.startswith("_item.name"):
                    parts = line.split(None, 1)
                    item_name = parts[1].strip().strip('"') if len(parts) > 1 else None
                elif line.startswith("_pdbx_item_type.code"):
                    pdbx_item_type = line.split(None, 1)[1].strip()
                elif line.startswith("_item_type.code"):
                    item_type = line.split(None, 1)[1].strip()
                continue

            # outside saveframes: the _pdbx_item_linked_group_list loop lives
            # at the top level of the dictionary block
            if line == "loop_":
                in_rel_loop = True
                rel_header = []
                rel_loop_cols = None
                rel_buf = []
                rel_ncols = 0
                continue
            if in_rel_loop and line.startswith("_pdbx_item_linked_group_list."):
                rel_header.append(line)
                continue
            if in_rel_loop and line.startswith("_"):
                # a different loop_ header starts; abort relationship parse
                in_rel_loop = False
                rel_header = []
                rel_buf = []
                continue
            if in_rel_loop and line == "#":
                # end of the loop data
                in_rel_loop = False
                rel_header = []
                rel_loop_cols = None
                rel_buf = []
                continue
            if in_rel_loop:
                # data row(s) of the relationship loop; accumulate tokens
                if rel_header and not rel_loop_cols:
                    rel_loop_cols = {
                        "child_category_id": rel_header.index("_pdbx_item_linked_group_list.child_category_id"),
                        "parent_category_id": rel_header.index("_pdbx_item_linked_group_list.parent_category_id"),
                    }
                    rel_ncols = len(rel_header)
                if rel_loop_cols:
                    rel_buf += tokenize(line)
                    while len(rel_buf) >= rel_ncols:
                        row = rel_buf[:rel_ncols]
                        rel_buf = rel_buf[rel_ncols:]
                        child = row[rel_loop_cols["child_category_id"]]
                        parent = row[rel_loop_cols["parent_category_id"]]
                        if parent != "." and child != "." and parent != "?" and child != "?":
                            relationships.add((parent, child))
    return items, relationships


def write_gz(out_path, header, rows):
    with gzip.open(out_path, "wt", encoding="utf-8", newline="") as gz:
        gz.write(header)
        for row in rows:
            gz.write("%s\n" % row)
    print("wrote %s: %d rows" % (out_path, len(rows)))


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        return 1
    dict_path, type_out, rel_out = sys.argv[1], sys.argv[2], sys.argv[3]

    items, relationships = parse_dict(dict_path)

    type_rows = []
    for item_name, code in items:
        type_rows.append("%s\t%s" % (item_name, duckdb_type(code) if code else "VARCHAR"))
    type_rows.sort()

    type_header = "# mmcif_pdbx_v50.dic v%s type index\n" % DIC_VERSION
    type_header += "# source: %s\n" % SOURCE_URL
    type_header += "# column: <dictionary item>\\t<DuckDB type>\n"
    write_gz(type_out, type_header, type_rows)

    rel_rows = sorted("%s\t%s" % (parent, child) for parent, child in relationships)
    rel_header = "# mmcif_pdbx_v50.dic v%s parent->child relationships\n" % DIC_VERSION
    rel_header += "# source: %s\n" % SOURCE_URL
    rel_header += "# column: <parent_category_id>\\t<child_category_id>\n"
    write_gz(rel_out, rel_header, rel_rows)
    return 0


if __name__ == "__main__":
    sys.exit(main())
