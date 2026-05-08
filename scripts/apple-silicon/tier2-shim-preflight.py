#!/usr/bin/env python3
"""Read-only live preflight for the Tier-2 retail-input shim.

This tool reads the hook candidate export-table slot and verifies that it
still points at the original kernel function. It never calls unsafe-enable and
never writes memory. A future Tier-2 installer must require this check before
patching the slot.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
import sys
from typing import Any


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
DEFAULT_ANALYSIS = ROOT / "benchmark-runs/tier2-shim-analysis-20260507T2310Z/summary.json"

_spec = importlib.util.spec_from_file_location("oracle_client", HERE / "oracle-client.py")
oc = importlib.util.module_from_spec(_spec)  # type: ignore[arg-type]
assert _spec and _spec.loader
_spec.loader.exec_module(oc)  # type: ignore[union-attr]


def parse_hex(value: str) -> int:
    return int(value, 16)


def load_analysis(path: Path) -> tuple[int, int, dict[str, Any]]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("verdict") != "viable-prior-art-match":
        raise ValueError(f"{path}: analysis verdict is {data.get('verdict')!r}")
    match = data.get("nkpatcher_igr_match") or {}
    preflight = data.get("preflight_mem_read") or {}
    slot = parse_hex(match["ke_raise_export_slot_va"])
    expected = parse_hex(preflight["expected_current_le32"])
    return slot, expected, data


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="192.168.0.200")
    parser.add_argument("--port", type=int, default=9001)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--analysis", type=Path, default=DEFAULT_ANALYSIS)
    parser.add_argument("--out", type=Path,
                        default=ROOT / "benchmark-runs/tier2-shim-preflight/summary.json")
    args = parser.parse_args()

    summary: dict[str, Any] = {
        "schema": "tier2-shim-preflight-v1",
        "host": args.host,
        "port": args.port,
        "analysis": str(args.analysis),
        "out": str(args.out),
    }
    try:
        slot, expected, _analysis = load_analysis(args.analysis)
        summary["slot_va"] = f"0x{slot:08x}"
        summary["expected_le32"] = f"0x{expected:08x}"
    except Exception as exc:
        summary["verdict"] = "blocked"
        summary["error"] = str(exc)
    else:
        try:
            with oc.OracleClient(args.host, args.port, args.timeout) as client:
                raw = client.mem_read(slot, 4)
            observed = int.from_bytes(raw, "little")
            summary["observed_le32"] = f"0x{observed:08x}"
            summary["observed_raw_hex"] = raw.hex()
            summary["verdict"] = "ok" if observed == expected else "fail"
            if observed != expected:
                summary["error"] = (
                    "export slot does not contain the original function RVA; "
                    "another patch may already own this hook boundary"
                )
        except Exception as exc:
            summary["verdict"] = "blocked"
            summary["error"] = str(exc)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n",
                        encoding="utf-8")
    print(f"wrote {args.out}")
    if summary["verdict"] != "ok":
        print(json.dumps(summary, indent=2, sort_keys=True), file=sys.stderr)
        return 1
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
