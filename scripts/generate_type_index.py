#!/usr/bin/env python3
"""Generate the mmcif dictionary type index and relationship artifacts.

Reads mmcif_pdbx_v50.dic (an mmcif file) and emits compact gzip'd, sorted TSV
artifacts:

  * type index: each dictionary item `_category.item` -> its DuckDB column type
  * optional category metadata: keys, row limits, descriptions, groups and mandatory codes
  * relationships: parent_item -> child_item pairs from
    `_pdbx_item_linked_group_list` (each item is a full `_category.item` key, so
    both the table and its column are carried per side)

Run off-repo / in CI on a version bump (rare); the artifacts are checked in and
shipped with the extension. The build never downloads or parses the dictionary.

Usage:
    python3 scripts/generate_type_index.py <mmcif_pdbx_v50.dic> <type.tsv.gz> <rel.tsv.gz> [<categories.tsv.gz>]
"""

import sys
import gzip

SOURCE_URL = "https://mmcif.wwpdb.org/dictionaries/ascii/mmcif_pdbx_v50.dic"
DIC_VERSION = "5.417"

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
    relationships: set of (parent_item, child_item) from the
    _pdbx_item_linked_group_list loop, where each item is a full `_category.item`
    key carrying both the category (table) and the data item (column).
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
                        "child_name": rel_header.index("_pdbx_item_linked_group_list.child_name"),
                        "parent_name": rel_header.index("_pdbx_item_linked_group_list.parent_name"),
                    }
                    rel_ncols = len(rel_header)
                if rel_loop_cols:
                    rel_buf += tokenize(line)
                    while len(rel_buf) >= rel_ncols:
                        row = rel_buf[:rel_ncols]
                        rel_buf = rel_buf[rel_ncols:]
                        child = row[rel_loop_cols["child_name"]]
                        parent = row[rel_loop_cols["parent_name"]]
                        if parent != "." and child != "." and parent != "?" and child != "?":
                            relationships.add((parent, child))
    return items, relationships


def cif_tokens(path):
    """Yield (value, is_bare) tokens, including quoted and semicolon text."""
    with open(path, encoding="utf-8") as source:
        lines = iter(source)
        for line in lines:
            if line.startswith(";"):
                text = [line[1:].rstrip("\r\n")]
                for continuation in lines:
                    if continuation.startswith(";"):
                        break
                    text.append(continuation.rstrip("\r\n"))
                else:
                    raise ValueError("Unterminated semicolon text")
                yield "\n".join(text), False
                continue
            i = 0
            while i < len(line):
                if line[i].isspace():
                    i += 1
                    continue
                if line[i] == "#":
                    break
                start = i
                if line[i] in "\"'":
                    quote = line[i]
                    i += 1
                    start = i
                    while i < len(line):
                        if line[i] == quote and (i + 1 == len(line) or line[i + 1].isspace()):
                            break
                        i += 1
                    if i == len(line):
                        raise ValueError("Unterminated quoted token")
                    yield line[start:i], False
                    i += 1
                else:
                    while i < len(line) and not line[i].isspace():
                        i += 1
                    yield line[start:i], True


def dictionary_frames(path):
    """Read scalar and looped values without confusing quoted tags with headers."""
    tokens = list(cif_tokens(path))
    frame = {}
    i = 0

    def control(token):
        value, bare = token
        return bare and (value.startswith(("_", "save_", "data_")) or value in {"loop_", "stop_"})

    while i < len(tokens):
        value, bare = tokens[i]
        i += 1
        if bare and value.startswith("save_"):
            yield frame
            frame = {}
        elif bare and value == "loop_":
            headers = []
            while i < len(tokens) and tokens[i][1] and tokens[i][0].startswith("_"):
                headers.append(tokens[i][0].lower())
                i += 1
            if not headers:
                raise ValueError("Empty loop header")
            row = 0
            while i < len(tokens) and not control(tokens[i]):
                frame.setdefault(headers[row % len(headers)], []).append(tokens[i][0])
                row += 1
                i += 1
            if row % len(headers):
                raise ValueError("Incomplete dictionary loop row")
        elif bare and value.startswith("_"):
            if i == len(tokens) or control(tokens[i]):
                raise ValueError("Missing value for " + value)
            frame.setdefault(value.lower(), []).append(tokens[i][0])
            i += 1
    yield frame


def parse_categories(path):
    """Retain category metadata and derive row limits from complete keys/links.

    A complete key fixed by entry (possibly via another single-row category)
    proves at most one row. A defined, independent key item permits multiple
    rows. Missing keys, unresolved links and cycles remain unknown.
    """
    categories = {}
    parents = {}
    items = set()
    for frame in dictionary_frames(path):
        items.update(v.lower() for v in frame.get("_item.name", []) if v not in {".", "?"})
        for prefix in ("_item_linked", "_pdbx_item_linked_group_list"):
            for parent, child in zip(frame.get(prefix + ".parent_name", []), frame.get(prefix + ".child_name", [])):
                if parent not in {".", "?"} and child not in {".", "?"}:
                    parents.setdefault(child.lower(), set()).add(parent.lower())
        for category in frame.get("_category.id", []):
            if category in {".", "?"}:
                continue
            def available(tag):
                return [v for v in frame.get(tag, []) if v not in {".", "?"}]
            mandatory = available("_category.mandatory_code")
            descriptions = available("_category.description")
            categories[category.lower()] = {
                "keys": sorted(set(v.lower() for v in available("_category_key.name"))),
                "description": descriptions[0].strip() if descriptions else None,
                "groups": sorted(set(available("_category_group.id"))) or None,
                "is_mandatory": {"yes": True, "no": False}.get(mandatory[0].lower()) if mandatory else None,
                "is_single_row": None,
            }

    def fixed(item, single, visiting):
        if item == "_entry.id" and categories.get("entry", {}).get("keys") == ["_entry.id"]:
            return True
        category = item.lstrip("_").split(".")[0]
        if item in items and category in single:
            return True
        if item in visiting:
            return False
        return any(fixed(parent, single, visiting | {item}) for parent in parents.get(item, []))

    single = set()
    while True:
        added = {name for name, meta in categories.items()
                 if meta["keys"] and all(fixed(key, single, set()) for key in meta["keys"])} - single
        if not added:
            break
        single.update(added)
    for name, meta in categories.items():
        if name in single:
            meta["is_single_row"] = True
        elif meta["keys"] and all(key in items for key in meta["keys"]) and any(
                key not in parents and not fixed(key, single, set()) for key in meta["keys"]):
            meta["is_single_row"] = False
    return categories


def category_rows(categories):
    """Long-form TSV; escape text so descriptions cannot introduce rows/columns."""
    def escape(value):
        return value.replace("\\", "\\\\").replace("\t", "\\t").replace("\n", "\\n").replace("\r", "\\r")
    rows = []
    for name, meta in sorted(categories.items()):
        rows.append(name + "\tcategory\t" + name)
        for field in ("is_single_row", "description", "is_mandatory"):
            value = meta[field]
            if value is not None:
                value = str(value).lower() if isinstance(value, bool) else value
                rows.append(name + "\t" + field + "\t" + escape(value))
        for field, values in (("key", meta["keys"]), ("group", meta["groups"] or [])):
            rows.extend(name + "\t" + field + "\t" + escape(value) for value in values)
    return rows


def write_gz(out_path, header, rows):
    with gzip.open(out_path, "wt", encoding="utf-8", newline="") as gz:
        gz.write(header)
        for row in rows:
            gz.write("%s\n" % row)
    print("wrote %s: %d rows" % (out_path, len(rows)))


def main():
    if len(sys.argv) not in (4, 5):
        print(__doc__)
        return 1
    dict_path, type_out, rel_out = sys.argv[1:4]

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
    rel_header += "# column: <parent_item (_category.item)>\t<child_item (_category.item)>\n"
    write_gz(rel_out, rel_header, rel_rows)
    if len(sys.argv) == 5:
        write_gz(sys.argv[4], "# category metadata: category\tfield\tvalue (backslash escaped)\n"
                 + "# source: %s v%s\n" % (SOURCE_URL, DIC_VERSION),
                 category_rows(parse_categories(dict_path)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
