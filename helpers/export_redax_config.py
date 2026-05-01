#!/usr/bin/env python3
"""
Export a Redax mode document (and its includes) from MongoDB into local JSON
files for board__test. 

Usage example:
  python helpers/export_redax_config.py \
    --uri mongodb://daq:${MONGO_PASSWORD_DAQ}@192.168.131.1:27020/admin\
    --db daq \
    --mode <Base Mode to download> \
    --out-dir ./board_stress_test/config_from_mongo \
    --write-merged
"""

import argparse
import datetime as dt
import json
import os
import sys
from typing import Any, Dict, List, Optional

from pymongo import MongoClient


def as_config_filename(name: str) -> str:
    if name.endswith(".json"):
        return name
    return f"{name}.json"


def strip_internal_fields(doc: Dict[str, Any]) -> Dict[str, Any]:
    out = dict(doc)
    out.pop("_id", None)
    return out


def to_json_safe(value: Any) -> Any:
    if isinstance(value, dict):
        return {str(k): to_json_safe(v) for k, v in value.items()}
    if isinstance(value, list):
        return [to_json_safe(v) for v in value]
    if isinstance(value, (str, int, float, bool)) or value is None:
        return value
    if isinstance(value, dt.datetime):
        return value.isoformat()
    return str(value)


def write_json(path: str, data: Dict[str, Any]) -> None:
    with open(path, "w", encoding="utf-8") as fout:
        json.dump(to_json_safe(data), fout, indent=2, sort_keys=True)
        fout.write("\n")


def merge_shallow(docs: List[Dict[str, Any]]) -> Dict[str, Any]:
    merged: Dict[str, Any] = {}
    for doc in docs:
        merged.update(doc)
    return merged


def load_doc_or_die(coll, name: str) -> Dict[str, Any]:
    doc = coll.find_one({"name": name})
    if doc is None:
        print(f"ERROR: options doc '{name}' not found", file=sys.stderr)
        sys.exit(2)
    return strip_internal_fields(doc)


def load_latest_override(coll, mode: str, host: Optional[str]) -> Optional[Dict[str, Any]]:
    query: Dict[str, Any] = {
        "command": "arm",
        "mode": mode,
        "options_override": {"$exists": True},
    }
    if host:
        query["host"] = host

    doc = coll.find_one(query, sort=[("_id", -1)])
    if doc is None:
        return None
    try:
        override = doc["options_override"]
    except KeyError:
        return None
    if not isinstance(override, dict):
        return None
    return strip_internal_fields(override)


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Export Redax mode and includes from MongoDB to local JSON files "
            "usable by board_stress_test."
        )
    )
    parser.add_argument(
        "--uri",
        type=str,
        default=os.environ.get("REDAX_MONGO_URI", ""),
        help="Mongo URI. Defaults to $REDAX_MONGO_URI if set.",
    )
    parser.add_argument(
        "--db",
        type=str,
        default="daq",
        help="Mongo database name (default: daq).",
    )
    parser.add_argument(
        "--mode",
        type=str,
        required=True,
        help="Mode name (options.name) to export.",
    )
    parser.add_argument(
        "--out-dir",
        type=str,
        default="./board_stress_test/config_from_mongo",
        help="Output directory for exported JSON files.",
    )
    parser.add_argument(
        "--root-name",
        type=str,
        default="",
        help="Optional output root filename stem (default: mode name).",
    )
    parser.add_argument(
        "--write-merged",
        action="store_true",
        help=(
            "Also write a shallow merged config file equivalent to Redax/board_stress_test "
            "include merging."
        ),
    )
    parser.add_argument(
        "--override-host",
        type=str,
        default="",
        help=(
            "If set, also export the latest arm command's options_override for this host "
            "and mode from db.control."
        ),
    )
    args = parser.parse_args()

    if not args.uri:
        print("ERROR: --uri is required (or set REDAX_MONGO_URI)", file=sys.stderr)
        return 2

    os.makedirs(args.out_dir, exist_ok=True)

    with MongoClient(args.uri) as client:
        db = client[args.db]
        options_coll = db["options"]
        control_coll = db["control"]

        root_doc = load_doc_or_die(options_coll, args.mode)
        include_names = root_doc.get("includes", [])
        if include_names is None:
            include_names = []
        if not isinstance(include_names, list):
            print("ERROR: root 'includes' field exists but is not a list", file=sys.stderr)
            return 2

        include_docs: List[Dict[str, Any]] = []
        for name in include_names:
            if not isinstance(name, str):
                print("ERROR: includes must be a list of strings", file=sys.stderr)
                return 2
            include_doc = load_doc_or_die(options_coll, name)
            include_docs.append(include_doc)
            include_path = os.path.join(args.out_dir, as_config_filename(name))
            write_json(include_path, include_doc)

        root_stem = args.root_name if args.root_name else args.mode
        root_path = os.path.join(args.out_dir, as_config_filename(root_stem))
        write_json(root_path, root_doc)

        merged_path = ""
        if args.write_merged:
            merged_doc = merge_shallow(include_docs + [root_doc])
            merged_stem = f"{root_stem}.merged"
            merged_path = os.path.join(args.out_dir, as_config_filename(merged_stem))
            write_json(merged_path, merged_doc)

        override_path = ""
        if args.override_host:
            override_doc = load_latest_override(control_coll, args.mode, args.override_host)
            if override_doc is not None:
                override_stem = f"{root_stem}.override"
                override_path = os.path.join(args.out_dir, as_config_filename(override_stem))
                write_json(override_path, override_doc)

        print("Export complete")
        print(f"  mode: {args.mode}")
        print(f"  out_dir: {os.path.abspath(args.out_dir)}")
        print(f"  root: {root_path}")
        print(f"  includes: {len(include_docs)}")
        if merged_path:
            print(f"  merged: {merged_path}")
        if override_path:
            print(f"  override: {override_path}")
        elif args.override_host:
            print(
                "  override: none found "
                f"(mode={args.mode}, host={args.override_host}, command=arm)"
            )

        print("\nboard_stress_test usage:")
        cmd = f"./board_stress_test --config {root_path}"
        if override_path:
            cmd += f" --config-override-file {override_path}"
        print(f"  {cmd}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

