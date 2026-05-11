#!/usr/bin/env python3
"""Summarize the M15 default-on evidence bundle from benchmark artifacts.

This is intentionally read-only: it answers "what evidence do we have,
and what is still missing?" without starting xemu or touching the Xbox.
Use it before long benchmark sessions so the next run closes a real gap.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any


REQUIRED_RETAIL = ("crimson", "rainbow", "pgr2")
REQUIRED_PAIRED = ("pgr2", "rainbow", "crimson", "sc2", "halo")
REQUIRED_JITTER = ("pgr2", "rainbow", "crimson")


@dataclass
class Check:
    name: str
    status: str
    detail: str
    artifact: str | None = None


def load_json(path: Path) -> dict[str, Any] | None:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return None


def latest(paths: list[Path]) -> Path | None:
    if not paths:
        return None
    return max(paths, key=lambda p: (p.stat().st_mtime, str(p)))


def verdict_ok(value: Any) -> bool:
    if isinstance(value, bool):
        return value
    if value is None:
        return False
    return str(value).lower() in {"ok", "pass", "passed", "true", "1"}


def find_latest_gate(root: Path, prefix: str) -> tuple[Path | None, dict[str, Any] | None]:
    summaries = sorted(root.glob(f"{prefix}*/summary.json"))
    path = latest(summaries)
    if path is None:
        return None, None
    return path, load_json(path)


def find_latest_oracle_validate(root: Path) -> tuple[Path | None, dict[str, Any] | None]:
    paths = list(root.glob("oracle-validate-*/summary.json"))
    paths.extend(root.glob("tools-readiness-*/oracle-validate/summary.json"))
    paths.extend(root.glob("tools-readiness-*/oracle-validate-full/summary.json"))
    path = latest(paths)
    if path is None:
        return None, None
    return path, load_json(path)


def find_latest_workflow(root: Path, title: str) -> tuple[Path | None, dict[str, Any] | None]:
    paths = sorted(root.glob(f"retail-oracle-workflow-{title}*/workflow.json"))
    path = latest(paths)
    if path is None:
        return None, None
    return path, load_json(path)


def paired_summaries(root: Path) -> dict[str, tuple[Path, dict[str, Any]]]:
    out: dict[str, tuple[Path, dict[str, Any]]] = {}
    for path in root.glob("*metal-gl-compare-*/summary.json"):
        data = load_json(path)
        if not data:
            continue
        game = str(data.get("game") or "").strip()
        if not game:
            m = re.search(r"metal-gl-compare-([A-Za-z0-9_-]+)", str(path.parent))
            game = m.group(1) if m else ""
        if not game:
            continue
        prev = out.get(game)
        if prev is None or path.stat().st_mtime > prev[0].stat().st_mtime:
            out[game] = (path, data)
    return out


def paired_summary_history(root: Path) -> dict[str, list[tuple[Path, dict[str, Any]]]]:
    out: dict[str, list[tuple[Path, dict[str, Any]]]] = {}
    for path in root.glob("*metal-gl-compare-*/summary.json"):
        data = load_json(path)
        if not data:
            continue
        game = str(data.get("game") or "").strip()
        if not game:
            m = re.search(r"metal-gl-compare-([A-Za-z0-9_-]+)", str(path.parent))
            game = m.group(1) if m else ""
        if not game:
            continue
        out.setdefault(game, []).append((path, data))

    for items in out.values():
        items.sort(key=lambda item: item[0].stat().st_mtime, reverse=True)
    return out


def parse_summary_text(text: str) -> dict[str, str]:
    out: dict[str, str] = {}
    for line in text.splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        out[key.strip()] = value.strip()
    return out


def read_summary_kv(run_dir: str | None, extract: Path | None = None) -> dict[str, str]:
    if not run_dir:
        return {}
    path = Path(run_dir) / "perf-summary.txt"
    if not path.exists():
        if extract is None or not extract.exists():
            return {}
        try:
            result = subprocess.run(
                [str(extract), run_dir],
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                text=True,
                timeout=10,
            )
        except Exception:
            return {}
        if result.returncode != 0:
            return {}
        return parse_summary_text(result.stdout)
    return parse_summary_text(path.read_text(encoding="utf-8", errors="replace"))


def parse_float(data: dict[str, str], key: str) -> float | None:
    try:
        return float(data[key])
    except Exception:
        return None


def is_gameplay_visual_evidence(data: dict[str, Any]) -> bool:
    """Return true only for paired evidence that intentionally covers gameplay.

    Static boot/menu/loading canaries are useful capture-plumbing and
    regression evidence, but they must not close the M15 title-level visual
    parity gate. New harness summaries should mark gameplay runs with either
    evidence_class=gameplay or gameplay_evidence=true.
    """
    if verdict_ok(data.get("gameplay_evidence")):
        return True
    evidence_class = str(data.get("evidence_class") or "").strip().lower()
    return evidence_class in {"gameplay", "gameplay-route", "route-gameplay"}


def latest_parseable_jitter(
    history: dict[str, list[tuple[Path, dict[str, Any]]]],
    game: str,
    extract: Path,
) -> tuple[Path, dict[str, Any], float, float] | None:
    for path, data in history.get(game, []):
        gl = read_summary_kv(data.get("gl_run_dir"), extract)
        metal = read_summary_kv(data.get("metal_run_dir"), extract)
        gl_p99 = parse_float(gl, "post_load_mspf_max_p99")
        metal_p99 = parse_float(metal, "post_load_mspf_max_p99")
        if gl_p99 is not None and metal_p99 is not None and gl_p99 != 0:
            return path, data, gl_p99, metal_p99
    return None


def build_checks(root: Path) -> list[Check]:
    checks: list[Check] = []
    repo = root.parent
    extract = repo / "scripts/apple-silicon/extract-perf-summary.sh"

    gate_path, gate = find_latest_gate(root, "m15-gate-")
    if gate_path and gate:
        checks.append(Check(
            "composite visual/oracle gate",
            "ok" if verdict_ok(gate.get("verdict")) and int(gate.get("fail", 1)) == 0 else "fail",
            f"pass={gate.get('pass', '?')} fail={gate.get('fail', '?')}",
            str(gate_path),
        ))
    else:
        checks.append(Check("composite visual/oracle gate", "missing", "no m15-gate summary found"))

    oracle_path, oracle = find_latest_oracle_validate(root)
    if oracle_path and oracle:
        checks.append(Check(
            "oracle production gate",
            "ok" if int(oracle.get("fail", 1)) == 0 else "fail",
            f"pass={oracle.get('pass', '?')} fail={oracle.get('fail', '?')}",
            str(oracle_path),
        ))
    else:
        checks.append(Check(
            "oracle production gate",
            "missing",
            "no oracle-validate summary found; rerun oracle-validate.sh with the current summary writer",
        ))

    for title in REQUIRED_RETAIL:
        path, data = find_latest_workflow(root, title)
        if path and data:
            checks.append(Check(
                f"retail oracle workflow: {title}",
                "ok" if verdict_ok(data.get("status")) else "fail",
                f"status={data.get('status', '?')}",
                str(path),
            ))
        else:
            checks.append(Check(f"retail oracle workflow: {title}", "missing", "no workflow.json found"))

    paired = paired_summaries(root)
    paired_history = paired_summary_history(root)
    for game in REQUIRED_PAIRED:
        item = paired.get(game)
        if item is None:
            checks.append(Check(f"paired gameplay Metal-vs-GL diff: {game}", "missing", "no metal-gl-compare summary found"))
            continue
        path, data = item
        frames = data.get("frames") or []
        max_changed = max((float(f.get("changed_pct", 0)) for f in frames), default=0.0)
        if verdict_ok(data.get("verdict")) and max_changed <= 1.0 and not is_gameplay_visual_evidence(data):
            evidence_class = str(data.get("evidence_class") or "unmarked/non-gameplay")
            checks.append(Check(
                f"paired gameplay Metal-vs-GL diff: {game}",
                "missing",
                f"latest PASS is {evidence_class} evidence only; needs matched gameplay keyframes",
                str(path),
            ))
            continue
        checks.append(Check(
            f"paired gameplay Metal-vs-GL diff: {game}",
            "ok" if verdict_ok(data.get("verdict")) and max_changed <= 1.0 else "fail",
            f"verdict={data.get('verdict', '?')} max_changed_pct={max_changed:.4f}",
            str(path),
        ))

    for game in REQUIRED_JITTER:
        item = latest_parseable_jitter(paired_history, game, extract)
        if item is None:
            if game in paired:
                checks.append(Check(
                    f"p99 jitter delta: {game}",
                    "missing",
                    "latest paired runs do not have parseable post-load p99 data",
                    str(paired[game][0]),
                ))
            else:
                checks.append(Check(f"p99 jitter delta: {game}", "missing", "no paired run"))
            continue
        path, _data, gl_p99, metal_p99 = item
        improvement = (gl_p99 - metal_p99) / gl_p99 * 100.0
        checks.append(Check(
            f"p99 jitter delta: {game}",
            "ok" if improvement >= 20.0 else "fail",
            f"gl={gl_p99:.2f}ms metal={metal_p99:.2f}ms improvement={improvement:.2f}%",
            str(path),
        ))

    checks.append(Check(
        "cold shader compile proof",
        "missing",
        "needs a fresh-cache Metal run with METAL_SHADER_COMPILE_* and METAL_SHADER_CACHE_* counters recorded",
    ))
    checks.append(Check(
        "front-fb fallback policy",
        "missing",
        "needs a decision-log entry accepting fallback default-on or a faithful CRTC publish fix",
    ))
    return checks


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--json", action="store_true", help="emit machine-readable JSON")
    args = parser.parse_args()

    root = args.root
    bench = root / "benchmark-runs"
    checks = build_checks(bench)
    ok = sum(1 for c in checks if c.status == "ok")
    fail = sum(1 for c in checks if c.status == "fail")
    missing = sum(1 for c in checks if c.status == "missing")
    verdict = "ok" if fail == 0 and missing == 0 else "incomplete"

    payload = {
        "schema": "m15-bundle-status-v1",
        "repo": str(root),
        "benchmark_root": str(bench),
        "verdict": verdict,
        "ok": ok,
        "fail": fail,
        "missing": missing,
        "checks": [c.__dict__ for c in checks],
    }
    if args.json:
        json.dump(payload, sys.stdout, indent=2, sort_keys=True)
        print()
    else:
        print("# M15 bundle status")
        print()
        print(f"verdict={verdict} ok={ok} fail={fail} missing={missing}")
        print()
        for c in checks:
            artifact = f" ({c.artifact})" if c.artifact else ""
            print(f"{c.status.upper():8s} {c.name}: {c.detail}{artifact}")
    return 0 if verdict == "ok" else 1


if __name__ == "__main__":
    raise SystemExit(main())
