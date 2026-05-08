#!/usr/bin/env python3
"""Summarize the current Metal/oracle feedback surface.

This is intentionally read-only. It scans recent benchmark artifacts and a
small amount of local tool configuration so a session can start from measured
state instead of memory.
"""

from __future__ import annotations

import argparse
import glob
import json
import os
from pathlib import Path
import re
import subprocess
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
RUNS = ROOT / "benchmark-runs"


def read_json(path: Path) -> dict[str, Any] | None:
    try:
        with path.open("r", encoding="utf-8") as f:
            data = json.load(f)
        return data if isinstance(data, dict) else None
    except (OSError, json.JSONDecodeError):
        return None


def read_text(path: Path, limit: int = 20000) -> str:
    try:
        with path.open("r", encoding="utf-8", errors="replace") as f:
            return f.read(limit)
    except OSError:
        return ""


def latest_dir(pattern: str, require: str | None = None) -> Path | None:
    paths = [Path(p) for p in glob.glob(str(RUNS / pattern))]
    if require:
        paths = [p for p in paths if (p / require).exists()]
    if not paths:
        return None
    return max(paths, key=lambda p: (p.stat().st_mtime, p.name))


def latest_summary(pattern: str) -> tuple[Path, dict[str, Any]] | None:
    d = latest_dir(pattern, "summary.json")
    if not d:
        return None
    data = read_json(d / "summary.json")
    if data is None:
        return None
    return d, data


def xcode_mcp_state() -> dict[str, str]:
    out: dict[str, str] = {}
    try:
        proc = subprocess.run(
            ["xcrun", "--find", "mcpbridge"],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=5,
            check=False,
        )
        out["mcpbridge"] = proc.stdout.strip() if proc.returncode == 0 else "not found"
    except Exception as exc:  # pragma: no cover - defensive reporting only
        out["mcpbridge"] = f"probe failed: {exc}"

    cfg = Path.home() / ".codex" / "config.toml"
    state = "unknown"
    text = read_text(cfg)
    match = re.search(r"(?ms)^\[mcp_servers\.xcode\](.*?)(?:^\[|\Z)", text)
    if match:
        block = match.group(1)
        enabled = re.search(r"(?m)^\s*enabled\s*=\s*(\w+)", block)
        state = f"configured, enabled={enabled.group(1)}" if enabled else "configured"
    out["codex_config"] = state
    return out


def max_frame_changed_pct(summary: dict[str, Any]) -> str:
    frames = summary.get("frames")
    if not isinstance(frames, list) or not frames:
        return "n/a"
    vals = []
    for frame in frames:
        if isinstance(frame, dict) and "changed_pct" in frame:
            try:
                vals.append(float(frame["changed_pct"]))
            except (TypeError, ValueError):
                pass
    return f"{max(vals):.4f}%" if vals else "n/a"


def xbe_status_counts(summary: dict[str, Any]) -> dict[str, int]:
    counts: dict[str, int] = {}
    for row in summary.get("results", []):
        if not isinstance(row, dict):
            continue
        status = str(row.get("status", "unknown"))
        counts[status] = counts.get(status, 0) + 1
    return counts


def emit_dashboard() -> str:
    lines: list[str] = []
    lines.append("# Metal feedback dashboard")
    lines.append("")
    lines.append(f"- repo: `{ROOT}`")
    lines.append(f"- benchmark root: `{RUNS}`")
    lines.append("")

    xemu_bin = ROOT / "dist/xemu.app/Contents/MacOS/xemu"
    swift_pkg = ROOT / "tools/xemu-capture/Package.swift"
    capture_manifest = ROOT / "scripts/apple-silicon/metal-capture-manifest.py"
    kernel_dump = ROOT / "scripts/apple-silicon/xbox-kernel-symbol-dump.py"
    kernel_annotate = ROOT / "scripts/apple-silicon/xbox-kernel-export-annotate.py"
    readback_xbe = ROOT / "scripts/apple-silicon/xbe-tests/controller-readback/main.c"
    retail_smoke = ROOT / "scripts/apple-silicon/retail-oracle-smoke.py"
    readiness_gate = ROOT / "scripts/apple-silicon/metal-tools-readiness.sh"
    xcode_projects = list(ROOT.glob("*.xcodeproj")) + list(ROOT.glob("*.xcworkspace"))
    xcode = xcode_mcp_state()
    lines.append("## Tool surface")
    lines.append(f"- main xemu binary: {'present' if xemu_bin.exists() else 'missing'}")
    lines.append(
        "- main xemu Xcode project: "
        + ("present" if xcode_projects else "none at repo root; main build is Meson/CMake/app-bundle")
    )
    lines.append(f"- Swift capture package: {'present' if swift_pkg.exists() else 'missing'}")
    lines.append(f"- Metal capture manifest tool: {'present' if capture_manifest.exists() else 'missing'}")
    lines.append(f"- Kernel symbol dump tool: {'present' if kernel_dump.exists() else 'missing'}")
    lines.append(f"- Kernel export annotator: {'present' if kernel_annotate.exists() else 'missing'}")
    lines.append(f"- Controller-readback XBE source: {'present' if readback_xbe.exists() else 'missing'}")
    lines.append(f"- Retail-game oracle smoke gate: {'present' if retail_smoke.exists() else 'missing'}")
    lines.append(f"- Tools readiness gate: {'present' if readiness_gate.exists() else 'missing'}")
    lines.append(f"- Xcode mcpbridge: `{xcode.get('mcpbridge', 'unknown')}`")
    lines.append(f"- Codex Xcode MCP config: {xcode.get('codex_config', 'unknown')}")
    lines.append("")

    m15 = latest_summary("m15-gate-*")
    lines.append("## Latest gates")
    if m15:
        path, data = m15
        lines.append(
            f"- M15 visual gate: {data.get('verdict', 'unknown')} "
            f"pass={data.get('pass', '?')} fail={data.get('fail', '?')} `{path}`"
        )
    else:
        lines.append("- M15 visual gate: no summary found")

    oracle = latest_dir("oracle-validate-*", "report.md")
    if oracle:
        report = read_text(oracle / "report.md", 4000)
        pass_match = re.search(r"Pass:\s*(\d+)", report)
        fail_match = re.search(r"Fail:\s*(\d+)", report)
        lines.append(
            f"- Oracle validate: pass={pass_match.group(1) if pass_match else '?'} "
            f"fail={fail_match.group(1) if fail_match else '?'} `{oracle}`"
        )
    else:
        lines.append("- Oracle validate: no report found")

    canary = latest_summary("*canary-regress")
    if canary:
        path, data = canary
        verdict = data.get("verdict", data.get("pass", "unknown"))
        names = []
        for row in data.get("counter_canaries", []):
            if isinstance(row, dict):
                names.append(f"{row.get('name')}={row.get('verdict')}")
        lines.append(f"- Metal canary regress: {verdict} `{path}`")
        if names:
            lines.append(f"  - canaries: {', '.join(names)}")
    else:
        lines.append("- Metal canary regress: no summary found")

    tools = latest_summary("tools-readiness-*")
    if tools:
        path, data = tools
        lines.append(
            f"- Tools readiness: {data.get('verdict', 'unknown')} "
            f"pass={data.get('pass', '?')} warn={data.get('warn', '?')} "
            f"fail={data.get('fail', '?')} `{path}`"
        )
    else:
        lines.append("- Tools readiness: no summary found")

    retail = latest_dir("retail-oracle-smoke-*", "verdict.json")
    if retail:
        data = read_json(retail / "verdict.json") or {}
        lines.append(f"- Retail-game real-Xbox smoke: {data.get('verdict', 'unknown')} `{retail}`")
        reasons = data.get("blocked_reasons")
        if isinstance(reasons, list) and reasons:
            lines.append(f"  - blocker: {reasons[0]}")
    else:
        lines.append("- Retail-game real-Xbox smoke: no verdict found")

    xbe_direct = latest_summary("xbe-*")
    if xbe_direct:
        path, data = xbe_direct
        counts = xbe_status_counts(data)
        counts_text = ", ".join(f"{k}={v}" for k, v in sorted(counts.items()))
        lines.append(f"- Latest standalone XBE matrix: {counts_text or 'no rows'} `{path}`")
    else:
        lines.append("- Latest standalone XBE matrix: no summary found")
    lines.append("")

    lines.append("## Latest paired diffs")
    paired = []
    for p in sorted(RUNS.glob("*metal-gl-compare-*")):
        data = read_json(p / "summary.json")
        if data:
            paired.append((p.stat().st_mtime, p, data))
    if not paired:
        lines.append("- no paired Metal-vs-GL summaries found")
    else:
        for _, path, data in sorted(paired)[-5:]:
            lines.append(
                f"- {data.get('game', path.name)}: {data.get('verdict', 'unknown')} "
                f"max_changed={max_frame_changed_pct(data)} "
                f"snapshot={data.get('snapshot') or '<none>'} "
                f"trigger={data.get('trigger', 'n/a')}:{data.get('trigger_ordinal', 'n/a')} "
                f"`{path}`"
            )
    lines.append("")

    lines.append("## Remaining feedback gaps")
    lines.append("- Xcode MCP is installed but globally disabled by policy; use only for real Xcode/Swift tasks or targeted GPU-trace inspection.")
    lines.append("- `.gputrace` capture now has automatic sidecar manifests; deep draw/resource inspection is still manual in Xcode.")
    lines.append("- Tier-1 real-Xbox diag input is shipped; Tier-2 readback/symbol tools are present; retail-game input + autonomous dashboard return are not proven.")
    lines.append("- Full M15 title gate still needs five-title paired visual/perf coverage, p99 jitter proof, cold shader compile proof, and front-fb fallback policy.")
    lines.append("")
    lines.append("## Suggested next commands")
    lines.append("- `./scripts/apple-silicon/oracle-validate.sh`")
    lines.append("- `./scripts/apple-silicon/metal-tools-readiness.sh --quick`")
    lines.append("- `./scripts/apple-silicon/retail-oracle-smoke.py --game-xbe 'F:\\\\Games\\\\Crimson Skies\\\\default.xbe' --input-csv scripts/apple-silicon/input-scripts/crimson-gameplay.csv`")
    lines.append("- `./scripts/apple-silicon/m15-visual-gate.sh --paired`")
    lines.append("- `./scripts/apple-silicon/metal-gl-compare.sh sc2 --input scripts/apple-silicon/input-scripts/sc2-gameplay.csv --duration 120`")
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", help="Optional path to write the markdown dashboard.")
    args = parser.parse_args()
    text = emit_dashboard()
    if args.out:
        Path(args.out).write_text(text, encoding="utf-8")
    print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
