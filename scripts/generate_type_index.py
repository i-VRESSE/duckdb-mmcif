#!/usr/bin/env python3
"""Generate the combined dictionary artifacts without build/runtime dependencies.

Usage:
    python3 scripts/generate_type_index.py <dictionary-directory> [--output-dir dict]

Download the sources listed in dict/sources.json into dictionary-directory first.
Source checksums and versions are verified before any artifacts are written.
Artifacts contain types, links, documentation and category metadata (complete
keys, row limits, descriptions, groups and mandatory codes).
"""

import argparse
import gzip
import hashlib
import io
import json
from pathlib import Path

# Ranges and comma-separated lists are textual values, not scalar numbers.
NUMERIC_FLOAT = {"float"}
NUMERIC_INT = {"int", "positive_int"}


def duckdb_type(code):
    if code in NUMERIC_FLOAT:
        return "DOUBLE"
    if code in NUMERIC_INT:
        return "BIGINT"
    return "VARCHAR"


def tokens(text):
    """Yield (value, quoted) CIF tokens, excluding comments and text delimiters."""
    i = 0
    while i < len(text):
        if text[i].isspace():
            i += 1
        elif text[i] == "#":
            end = text.find("\n", i)
            i = len(text) if end < 0 else end + 1
        elif text[i] == ";" and (i == 0 or text[i - 1] == "\n"):
            end = text.find("\n;", i + 1)
            if end < 0:
                raise ValueError("Unterminated CIF text field")
            yield text[i + 1 : end], True
            i = end + 2
        elif text[i] in "\"'":
            quote, start = text[i], i + 1
            i = start
            while i < len(text):
                if text[i] == quote and (i + 1 == len(text) or text[i + 1].isspace()):
                    break
                i += 1
            if i == len(text):
                raise ValueError("Unterminated CIF quoted value")
            yield text[start:i], True
            i += 1
        else:
            start = i
            while i < len(text) and not text[i].isspace():
                i += 1
            yield text[start:i], False


def is_control(token):
    value, quoted = token
    return not quoted and (
        value.startswith("_")
        or value.lower() in {"loop_", "stop_", "global_"}
        or value.lower().startswith(("save_", "data_"))
    )


def dictionary_frames(path):
    """Parse the top-level block and saveframes for all dictionary metadata."""
    stream = list(tokens(Path(path).read_text(encoding="utf-8")))
    top = {}
    frames = [("", top)]
    frame = top
    i = 0
    while i < len(stream):
        value, quoted = stream[i]
        lower = value.lower()
        i += 1
        if not quoted and lower.startswith("save_"):
            name = value[5:]
            if name:
                frame = {}
                frames.append((name, frame))
            else:
                frame = top
        elif not quoted and lower == "loop_":
            headers = []
            while i < len(stream) and not stream[i][1] and stream[i][0].startswith("_"):
                headers.append(stream[i][0].lower())
                i += 1
            if not headers:
                raise ValueError(f"Empty loop in {path}")
            values = []
            while i < len(stream) and not is_control(stream[i]):
                values.append(stream[i][0])
                i += 1
            if len(values) % len(headers):
                raise ValueError(f"Incomplete loop {headers} in {path}")
            for col, header in enumerate(headers):
                frame.setdefault(header, []).extend(values[col :: len(headers)])
        elif not quoted and value.startswith("_"):
            if i == len(stream) or is_control(stream[i]):
                raise ValueError(f"Missing value for {value} in {path}")
            frame.setdefault(lower, []).append(stream[i][0])
            i += 1

    return frames


def parse_dict(path):
    """Read scalar/loop fields in the dictionary block and each saveframe.

    Items are identified by their saveframe name: _item.name can itself be a
    loop listing both the defined item and related items. Links may be stored
    in top-level linked-group loops or scalar/loop _item_linked fields.
    """
    frames = dictionary_frames(path)
    items, categories, relationships = {}, {}, set()
    for name, fields in frames:
        if name.startswith("_") and "." in name:
            codes = fields.get("_pdbx_item_type.code", fields.get("_item_type.code", []))
            dtype = duckdb_type(codes[0] if codes else None)
            if name in items and items[name] != dtype:
                raise ValueError(f"Conflicting saveframes for {name} in {path}")
            items[name] = dtype
        for category in fields.get("_category.id", []):
            categories[category.lower()] = category
        for prefix in ("_pdbx_item_linked_group_list", "_item_linked"):
            parents = fields.get(prefix + ".parent_name", [])
            children = fields.get(prefix + ".child_name", [])
            if len(parents) != len(children):
                raise ValueError(f"Unpaired links in {path}: {name}")
            relationships.update(
                (parent, child)
                for parent, child in zip(parents, children)
                if parent not in {".", "?"} and child not in {".", "?"}
            )
    version = frames[0][1].get("_dictionary.version", [None])[0]
    return items, categories, relationships, version


def combine(sources, resolutions):
    """Merge exposed types and links, using explicit documentation ownership.

    sources are in the manifest's documented priority order. Type disagreement
    requires an exact, reviewed resolution naming every source and its type.
    """
    definitions, categories, relationships = {}, {}, set()
    for source, items, cats, links in sources:
        for item, dtype in items.items():
            definitions.setdefault(item.lower(), []).append((item, dtype, source))
        for key, category in cats.items():
            categories.setdefault(key, (category, source["documentation_url"]))
        relationships.update((parent.lower(), child.lower()) for parent, child in links)

    merged, documentation, used_resolutions = {}, {}, set()
    for key, entries in definitions.items():
        chosen = entries[0]
        types = {entry[1] for entry in entries}
        if len(types) > 1:
            candidates = {entry[2]["filename"]: entry[1] for entry in entries}
            resolution = resolutions.get(key)
            if not resolution or resolution["types"] != candidates:
                raise ValueError(f"Unresolved type conflict for {key}: {candidates}")
            if resolution["use"] not in candidates:
                raise ValueError(f"Invalid resolution source for {key}: {resolution['use']}")
            chosen = next(entry for entry in entries if entry[2]["filename"] == resolution["use"])
            used_resolutions.add(key)
        merged[chosen[0]] = chosen[1]
        documentation[chosen[0]] = chosen[2]["documentation_url"]
    unused = resolutions.keys() - used_resolutions
    if unused:
        raise ValueError(f"Stale type conflict resolutions: {sorted(unused)}")
    canonical = {item.lower(): item for item in merged}
    relationships = {(canonical.get(parent, parent), canonical.get(child, child)) for parent, child in relationships}
    for category, url in categories.values():
        documentation[category] = url
    return merged, documentation, relationships


def read_category_metadata(path):
    """Retain category metadata, complete keys and parent links.

    A complete key fixed by entry (possibly via another single-row category)
    proves at most one row. A defined, independent key item permits multiple
    rows. Missing keys, unresolved links and cycles remain unknown.
    """
    categories = {}
    parents = {}
    items = set()
    for name, frame in dictionary_frames(path):
        if name.startswith("_") and "." in name:
            items.add(name.lower())
        items.update(v.lower() for v in frame.get("_item.name", []) if v not in {".", "?"})
        for prefix in ("_item_linked", "_pdbx_item_linked_group_list"):
            for parent, child in zip(frame.get(prefix + ".parent_name", []), frame.get(prefix + ".child_name", [])):
                if child not in {".", "?"}:
                    parents.setdefault(child.lower(), set()).add(parent.lower())
        for category in frame.get("_category.id", []):
            if category in {".", "?"}:
                continue

            def available(tag):
                return [v for v in frame.get(tag, []) if v not in {".", "?"}]

            mandatory = available("_category.mandatory_code")
            descriptions = available("_category.description")
            categories[category.lower()] = {
                "keys": sorted(set(v.lower() for v in frame.get("_category_key.name", []))),
                "description": descriptions[0].strip() if descriptions else None,
                "groups": sorted(set(available("_category_group.id"))) or None,
                "is_mandatory": {"yes": True, "no": False}.get(mandatory[0].lower()) if mandatory else None,
                "is_single_row": None,
            }

    return categories, parents, items


def parse_categories(path):
    categories, parents, items = read_category_metadata(path)
    derive_row_constraints(categories, parents, items)
    return categories


def derive_row_constraints(categories, parents, items):
    """Prove singleton keys first; only defined independent keys prove multi-row."""
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


def write_gz(path, header, rows):
    # No timestamps or local paths in gzip headers: identical inputs give
    # byte-identical artifacts on regeneration.
    content = header + "".join(row + "\n" for row in sorted(rows))
    buffer = io.BytesIO()
    with gzip.GzipFile(filename="", mode="wb", fileobj=buffer, compresslevel=9, mtime=0) as gz:
        gz.write(content.encode("utf-8"))
    Path(path).write_bytes(buffer.getvalue())
    print(f"wrote {path}: {len(rows)} rows")


def generate(directory, output_dir, manifest_path):
    manifest = json.loads(Path(manifest_path).read_text())
    sources = []
    category_metadata = {}
    metadata_parents = {}
    provenance = "# Combined PDBx/mmCIF v5 + IHMCIF + flrCIF + 3DEM\n"
    for source in manifest["sources"]:
        path = Path(directory) / source["filename"]
        if hashlib.sha256(path.read_bytes()).hexdigest() != source["sha256"]:
            raise ValueError(f"Checksum mismatch: {path}; review source metadata before regenerating")
        items, categories, relationships, version = parse_dict(path)
        if version != source["version"]:
            raise ValueError(f"Version mismatch: {path}: {version}")
        sources.append((source, items, categories, relationships))
        metadata_categories, category_parents, _ = read_category_metadata(path)
        for name, metadata in metadata_categories.items():
            category_metadata.setdefault(name, metadata)
        for child, parents in category_parents.items():
            metadata_parents.setdefault(child, set()).update(parents)
        provenance += f"# {source['filename']} v{version}: {source['url']} sha256={source['sha256']}\n"
    types, documentation, relationships = combine(sources, manifest["type_conflicts"])
    derive_row_constraints(category_metadata, metadata_parents, {item.lower() for item in types})
    Path(output_dir).mkdir(parents=True, exist_ok=True)
    for filename, columns, rows in (
        ("mmcif_categories.tsv.gz", "category\tfield\tvalue (backslash escaped)",
         category_rows(category_metadata)),
        (
            "mmcif_type_index.tsv.gz",
            "item\tDuckDB type",
            [f"{item}\t{dtype}" for item, dtype in types.items()],
        ),
        (
            "mmcif_documentation.tsv.gz",
            "category or item\tdictionary documentation URL",
            [f"{key}\t{url}" for key, url in documentation.items()],
        ),
        (
            "mmcif_relationships.tsv.gz",
            "parent item\tchild item",
            [f"{parent}\t{child}" for parent, child in relationships],
        ),
    ):
        write_gz(Path(output_dir) / filename, provenance + f"# columns: {columns}\n", rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--output-dir", type=Path, default=Path("dict"))
    parser.add_argument(
        "--manifest",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "dict/sources.json",
    )
    args = parser.parse_args()
    generate(args.directory, args.output_dir, args.manifest)


if __name__ == "__main__":
    main()
