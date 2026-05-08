#!/usr/bin/env python3
"""Read-only viability analyzer for the retail-input Tier-2 shim.

This script does not contact the Xbox and never writes memory. It joins our
live annotated kernel export dump with an optional NKPatcher source checkout
to answer one narrow question:

    Does this running kernel match a known NKPatcher IGR hook recipe?

That matters because NKPatcher's IGR path is prior art for a software-only
retail-game controller hook. It redirects the KeRaiseIrqlToDpcLevel export
slot, then observes the XInputGetState return path to decide whether the IGR
combo was pressed. If our kernel matches that recipe, Tier 2 should start from
that boundary rather than from a blind USB/OHCI probe.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_DUMP = (
    ROOT.parent
    / "xbox-oracle-backup/2026-05-06/kernel-symbols/"
    / "xboxkrnl-0x80010000-exports-annotated.json"
)
DEFAULT_NKPATCHER = Path(
    "/tmp/xbox-tier2-refs/Rocky5-Xbox-Softmodding-Tool/"
    "App Sources/NKPatcher/Softmod Save NKP11/nkpatcher.asm"
)

REQUIRED_EXPORTS = {
    "KeRaiseIrqlToDpcLevel": 129,
    "HalReturnToFirmware": 49,
    "HalWriteSMBusValue": 50,
    "LaunchDataPage": 164,
    "MmAllocateContiguousMemory": 165,
    "MmPersistContiguousMemory": 178,
}

IGR_RE = re.compile(
    r"\bigr\s+"
    r"([0-9A-Fa-f]+)h,\s*"
    r"([0-9A-Fa-f]+)h,\s*"
    r"([0-9A-Fa-f]+)h,\s*"
    r"([0-9A-Fa-f]+)h,\s*"
    r"([0-9A-Fa-f]+)h,\s*"
    r"([0-9A-Fa-f]+)h,\s*"
    r"([0-9A-Fa-f]+)h"
)
PATCHER_RE = re.compile(r"^\s*(patcher_[0-9A-Za-z_]+):")


def load_symbols(path: Path) -> dict[str, dict[str, Any]]:
    data = json.loads(path.read_text(encoding="utf-8"))
    return {sym["name"]: sym for sym in data["symbols"]}


def parse_nkpatcher_igr(path: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    current = "<unknown>"
    for line_no, raw in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), start=1):
        label = PATCHER_RE.match(raw)
        if label:
            current = label.group(1)
        match = IGR_RE.search(raw)
        if not match:
            continue
        args = [int(v, 16) for v in match.groups()]
        rows.append({
            "line": line_no,
            "patcher": current,
            "ke_raise_export_slot_va": args[0],
            "ke_raise_va": args[1],
            "hal_return_va": args[2],
            "hal_write_smbus_va": args[3],
            "launch_data_page_va": args[4],
            "mm_allocate_contiguous_memory_va": args[5],
            "mm_persist_contiguous_memory_va": args[6],
            "raw": raw.strip(),
        })
    return rows


def hexva(value: int | None) -> str | None:
    return None if value is None else f"0x{value:08x}"


def find_matching_igr(symbols: dict[str, dict[str, Any]],
                      rows: list[dict[str, Any]]) -> dict[str, Any] | None:
    expected = {
        "ke_raise_va": symbols["KeRaiseIrqlToDpcLevel"]["va"],
        "hal_return_va": symbols["HalReturnToFirmware"]["va"],
        "hal_write_smbus_va": symbols["HalWriteSMBusValue"]["va"],
        "launch_data_page_va": symbols["LaunchDataPage"]["va"],
        "mm_allocate_contiguous_memory_va": symbols["MmAllocateContiguousMemory"]["va"],
        "mm_persist_contiguous_memory_va": symbols["MmPersistContiguousMemory"]["va"],
    }
    for row in rows:
        if all(row[k] == v for k, v in expected.items()):
            return row
    return None


def make_report(payload: dict[str, Any]) -> str:
    match = payload.get("nkpatcher_igr_match")
    verdict = payload["verdict"]
    lines = [
        "# Tier-2 shim viability analysis",
        "",
        f"- verdict: `{verdict}`",
        f"- kernel dump: `{payload['kernel_dump']}`",
        f"- NKPatcher source: `{payload.get('nkpatcher_source') or '<not provided>'}`",
        "",
        "## Export evidence",
        "",
    ]
    for row in payload["required_exports"]:
        lines.append(
            f"- {row['name']} ordinal {row['ordinal']}: `{row['va']}`"
        )
    lines.append("")
    if match:
        lines.extend([
            "## NKPatcher IGR match",
            "",
            f"- patcher: `{match['patcher']}`",
            f"- source line: `{match['line']}`",
            f"- KeRaiseIrqlToDpcLevel export-slot VA: `{match['ke_raise_export_slot_va']}`",
            f"- expected current slot value before install: `{payload['preflight_mem_read']['expected_current_le32']}`",
            "",
            "Interpretation: this kernel matches an existing software IGR "
            "recipe that hooks the KeRaiseIrqlToDpcLevel export slot and "
            "observes the title-facing input state path. Tier 2 should begin "
            "with a read-only/diagnostic adaptation of that boundary.",
            "",
        ])
    else:
        lines.extend([
            "## NKPatcher IGR match",
            "",
            "No exact NKPatcher IGR recipe matched the current export VAs.",
            "Tier 2 is still possible, but it needs either a different prior-art "
            "recipe or a live, read-only call-context probe before patching.",
            "",
        ])
    lines.extend([
        "## Candidate ranking",
        "",
        "| Candidate | Confidence | Reason |",
        "| --- | --- | --- |",
        "| KeRaiseIrqlToDpcLevel export-slot hook | High if the NKPatcher match is present | Prior art shows this boundary sees retail XInputGetState return context for IGR. |",
        "| IofCompleteRequest / IRP completion hook | Medium | Generic kernel boundary is exported, but we do not yet have call-context evidence for XID input reports. |",
        "| OHCI/XID internal routine hook | Medium-low | Conceptually closest to hardware reports, but no exported symbol and higher reverse-engineering cost. |",
        "| Per-title XInput patch | Low for production | Local title scans show inconsistent static XAPI layouts and strings. |",
        "",
    ])
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel-dump", type=Path, default=DEFAULT_DUMP)
    parser.add_argument("--nkpatcher-asm", type=Path, default=DEFAULT_NKPATCHER,
                        help="Path to NKPatcher nkpatcher.asm. If missing, only export evidence is reported.")
    parser.add_argument("--out-json", type=Path)
    parser.add_argument("--out-md", type=Path)
    args = parser.parse_args()

    symbols = load_symbols(args.kernel_dump)
    missing = [
        f"{name}@{ordinal}"
        for name, ordinal in REQUIRED_EXPORTS.items()
        if name not in symbols or symbols[name]["ordinal"] != ordinal
    ]
    nk_rows: list[dict[str, Any]] = []
    match = None
    if args.nkpatcher_asm.exists():
        nk_rows = parse_nkpatcher_igr(args.nkpatcher_asm)
        if not missing:
            match = find_matching_igr(symbols, nk_rows)

    payload: dict[str, Any] = {
        "schema": "tier2-shim-analyze-v1",
        "kernel_dump": str(args.kernel_dump),
        "nkpatcher_source": str(args.nkpatcher_asm) if args.nkpatcher_asm.exists() else None,
        "missing_required_exports": missing,
        "required_exports": [
            {
                "name": name,
                "ordinal": symbols.get(name, {}).get("ordinal"),
                "va": hexva(symbols.get(name, {}).get("va")),
            }
            for name in REQUIRED_EXPORTS
        ],
        "nkpatcher_igr_recipes_found": len(nk_rows),
        "nkpatcher_igr_match": None,
        "preflight_mem_read": None,
        "verdict": "blocked",
    }
    if missing:
        payload["blocked_reason"] = "required exports missing or ordinal mismatch"
    elif not args.nkpatcher_asm.exists():
        payload["blocked_reason"] = "NKPatcher source not available for prior-art match"
    elif match:
        payload["verdict"] = "viable-prior-art-match"
        payload["nkpatcher_igr_match"] = {
            **match,
            "ke_raise_export_slot_va": hexva(match["ke_raise_export_slot_va"]),
            "ke_raise_va": hexva(match["ke_raise_va"]),
            "hal_return_va": hexva(match["hal_return_va"]),
            "hal_write_smbus_va": hexva(match["hal_write_smbus_va"]),
            "launch_data_page_va": hexva(match["launch_data_page_va"]),
            "mm_allocate_contiguous_memory_va": hexva(match["mm_allocate_contiguous_memory_va"]),
            "mm_persist_contiguous_memory_va": hexva(match["mm_persist_contiguous_memory_va"]),
        }
        payload["preflight_mem_read"] = {
            "address": hexva(match["ke_raise_export_slot_va"]),
            "expected_current_le32": f"0x{symbols['KeRaiseIrqlToDpcLevel']['rva']:08x}",
            "meaning": (
                "Before installing a hook, read this export-table slot. It "
                "should still point at KeRaiseIrqlToDpcLevel. If it differs, "
                "another patch already owns the boundary and Tier 2 must chain "
                "or stop."
            ),
        }
    else:
        payload["blocked_reason"] = "no exact NKPatcher IGR recipe matched export VAs"

    if args.out_json:
        args.out_json.parent.mkdir(parents=True, exist_ok=True)
        args.out_json.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n",
                                 encoding="utf-8")
    if args.out_md:
        args.out_md.parent.mkdir(parents=True, exist_ok=True)
        args.out_md.write_text(make_report(payload), encoding="utf-8")

    if args.out_json or args.out_md:
        if args.out_json:
            print(f"wrote {args.out_json}")
        if args.out_md:
            print(f"wrote {args.out_md}")
    else:
        print(json.dumps(payload, indent=2, sort_keys=True))
    return 0 if payload["verdict"].startswith("viable") else 1


if __name__ == "__main__":
    raise SystemExit(main())
