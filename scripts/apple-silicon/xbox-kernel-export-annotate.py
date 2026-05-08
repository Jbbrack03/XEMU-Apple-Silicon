#!/usr/bin/env python3
"""Annotate a live Xbox kernel ordinal export dump with nxdk names.

Retail Xbox kernel exports are ordinal-only on the running image. The oracle
symbol dumper therefore records `ordinal_N` entries, which are safe but hard
to use during Tier-2 input research. This tool joins that live dump with
nxdk's `xboxkrnl.exe.def` ordinal table and writes named JSON/TSV artifacts.
"""

from __future__ import annotations

import argparse
import csv
import json
import re
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_DEF = ROOT.parent / "nxdk/lib/xboxkrnl/xboxkrnl.exe.def"
DEFAULT_DUMP_DIR = ROOT.parent / "xbox-oracle-backup/2026-05-06/kernel-symbols"


def parse_def(path: Path) -> dict[int, dict[str, Any]]:
    out: dict[int, dict[str, Any]] = {}
    line_re = re.compile(r"^\s*(\S+)\s+@\s*(\d+)\s+NONAME(?:\s+(DATA))?\s*$")
    for line_no, raw in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), start=1):
        line = raw.split(";", 1)[0].strip()
        if not line:
            continue
        m = line_re.match(line)
        if not m:
            continue
        decorated, ordinal_s, data_flag = m.groups()
        ordinal = int(ordinal_s)
        undecorated = decorated
        if undecorated.startswith("@"):
            undecorated = undecorated[1:]
        undecorated = re.sub(r"@\d+$", "", undecorated)
        out[ordinal] = {
            "ordinal": ordinal,
            "decorated_name": decorated,
            "name": undecorated,
            "is_data": bool(data_flag),
            "def_line": line_no,
        }
    return out


def load_symbols(path: Path) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    data = json.loads(path.read_text(encoding="utf-8"))
    exports = data.get("exports", {})
    if not isinstance(exports, dict):
        raise ValueError(f"{path}: expected exports object")
    symbols = exports.get("symbols", [])
    if not isinstance(symbols, list):
        raise ValueError(f"{path}: expected exports.symbols list")
    return data, symbols


def annotate(dump: Path, def_path: Path) -> dict[str, Any]:
    def_by_ordinal = parse_def(def_path)
    dump_data, symbols = load_symbols(dump)
    annotated: list[dict[str, Any]] = []
    for sym in symbols:
        if not isinstance(sym, dict):
            continue
        ordinal = int(sym.get("ordinal", 0))
        named = def_by_ordinal.get(ordinal, {})
        row = dict(sym)
        row["ordinal_name"] = sym.get("name")
        row["name"] = named.get("name", sym.get("name"))
        row["decorated_name"] = named.get("decorated_name")
        row["is_data"] = named.get("is_data")
        row["def_line"] = named.get("def_line")
        row["named"] = ordinal in def_by_ordinal
        annotated.append(row)
    return {
        "schema": "xbox-kernel-exports-annotated-v1",
        "source_dump": str(dump),
        "source_def": str(def_path),
        "base": dump_data.get("base"),
        "export_name": dump_data.get("exports", {}).get("export_name"),
        "ordinal_base": dump_data.get("exports", {}).get("ordinal_base"),
        "function_count": dump_data.get("exports", {}).get("function_count"),
        "named_count": sum(1 for r in annotated if r.get("named")),
        "symbols": annotated,
    }


def latest_dump() -> Path:
    candidates = sorted(DEFAULT_DUMP_DIR.glob("xboxkrnl-*-exports.json"))
    if not candidates:
        raise FileNotFoundError(f"no xboxkrnl export dumps under {DEFAULT_DUMP_DIR}")
    return max(candidates, key=lambda p: (p.stat().st_mtime, p.name))


def write_tsv(data: dict[str, Any], path: Path) -> None:
    fields = ["ordinal", "name", "decorated_name", "va", "rva", "is_data", "named", "def_line"]
    with path.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fields, delimiter="\t", extrasaction="ignore")
        writer.writeheader()
        for row in data["symbols"]:
            writer.writerow(row)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dump", default=None, help="Live export dump JSON. Default: latest backup dump.")
    parser.add_argument("--def-file", default=str(DEFAULT_DEF))
    parser.add_argument("--out-json", default=None)
    parser.add_argument("--out-tsv", default=None)
    parser.add_argument("--grep", default=None, help="Print rows whose name contains this case-insensitive substring.")
    args = parser.parse_args()

    dump = Path(args.dump) if args.dump else latest_dump()
    def_path = Path(args.def_file)
    data = annotate(dump, def_path)

    out_json = Path(args.out_json) if args.out_json else dump.with_name(dump.stem.replace("-exports", "-exports-annotated") + ".json")
    out_tsv = Path(args.out_tsv) if args.out_tsv else out_json.with_suffix(".tsv")
    out_json.write_text(json.dumps(data, indent=2, sort_keys=True), encoding="utf-8")
    write_tsv(data, out_tsv)

    print(f"annotated {len(data['symbols'])} exports; named={data['named_count']}")
    print(f"json: {out_json}")
    print(f"tsv : {out_tsv}")
    if args.grep:
        needle = args.grep.lower()
        for row in data["symbols"]:
            hay = " ".join(str(row.get(k) or "") for k in ("name", "decorated_name", "ordinal_name")).lower()
            if needle in hay:
                print(f"{row['ordinal']:3d} 0x{int(row['va']):08x} {row['name']} {row.get('decorated_name') or ''}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
