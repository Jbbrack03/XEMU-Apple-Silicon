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
from typing import Any, Iterable


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


def _iter_paired_summary_paths(root: Path) -> Iterable[tuple[Path, dict[str, Any], str]]:
    """Yield (path, data, game) for paired-evidence summaries.

    Two discovery patterns are supported:

    1. `*metal-gl-compare-*/summary.json` — the legacy metal-gl-compare.sh
       harness output (paired visual diff + perf).
    2. `m15-gameplay-*/<subdir>/summary.json` — the M15 gameplay evidence
       artifacts produced by `m15-gameplay-visual-compare.py`. The
       `<subdir>` is typically `evidence` (the canonical path used in the
       handoff recipe) but can be `evidence-*`, `diagnostic-*`, etc.

    The game id is extracted from `data["game"]` if present, otherwise
    parsed from the parent directory name. The two patterns share the
    `verdict` + `frames[].changed_pct` shape consumed by the M15 paired
    gameplay diff check, and the gameplay artifacts add the explicit
    `evidence_class=gameplay` / `gameplay_evidence=true` markers that
    `is_gameplay_visual_evidence()` keys on.
    """
    glob_pattern_dir_re = {
        "*metal-gl-compare-*/summary.json": r"metal-gl-compare-([A-Za-z0-9_-]+)",
        "m15-gameplay-*/*/summary.json": r"m15-gameplay-([A-Za-z0-9]+)",
    }
    for pattern, dir_re in glob_pattern_dir_re.items():
        for path in root.glob(pattern):
            data = load_json(path)
            if not data:
                continue
            game = str(data.get("game") or "").strip()
            if not game:
                # The m15-gameplay-* pattern has the title as part of the
                # grandparent directory name (`m15-gameplay-<title>-<ts>`).
                # Walk parents until the regex bites — the immediate
                # parent is the `evidence` subdir which carries no title.
                for parent in (path.parent, path.parent.parent):
                    m = re.search(dir_re, parent.name)
                    if m:
                        game = m.group(1)
                        break
            if not game:
                continue
            yield path, data, game


def paired_summaries(root: Path) -> dict[str, tuple[Path, dict[str, Any]]]:
    """Return the best paired-evidence summary per game.

    Selection preference:
      1. Gameplay-evidence summaries (evidence_class=gameplay /
         gameplay_evidence=true) win over non-gameplay summaries
         regardless of mtime — this is the M15 production gate.
      2. Within each preference tier, latest by mtime wins.

    Without (1), a newer static-canary metal-gl-compare PASS would
    eclipse an earlier real gameplay PASS just because it ran more
    recently; the M15 gate would then report MISSING evidence the
    project actually has.
    """
    out: dict[str, tuple[Path, dict[str, Any], int]] = {}  # game -> (path, data, tier)
    for path, data, game in _iter_paired_summary_paths(root):
        tier = 1 if is_gameplay_visual_evidence(data) else 0
        prev = out.get(game)
        if prev is None:
            out[game] = (path, data, tier)
            continue
        prev_path, _prev_data, prev_tier = prev
        if tier > prev_tier:
            out[game] = (path, data, tier)
        elif tier == prev_tier and path.stat().st_mtime > prev_path.stat().st_mtime:
            out[game] = (path, data, tier)
    return {game: (path, data) for game, (path, data, _tier) in out.items()}


def paired_summary_history(root: Path) -> dict[str, list[tuple[Path, dict[str, Any]]]]:
    """Return all paired-evidence summaries per game, newest first.

    Used by the p99 jitter check which walks history looking for a
    summary that exposes parseable `gl_run_dir`/`metal_run_dir` fields.
    Gameplay-evidence summaries don't carry those (visual-only artifact)
    and are skipped naturally by `latest_parseable_jitter`; including
    them in history is harmless and keeps both discovery patterns
    coherent.
    """
    out: dict[str, list[tuple[Path, dict[str, Any]]]] = {}
    for path, data, game in _iter_paired_summary_paths(root):
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


def parse_perf_diff_p99(perf_diff: Path) -> tuple[float, float] | None:
    """Parse `post_load_mspf_max_p99` from a metal-gl-compare perf-diff.txt.

    The compare harness writes `perf-diff.txt` next to its `summary.json`.
    Format is a pipe-separated table; the relevant row is:

        post_load_mspf_max_p99   |    40.87 |   300.87 | +636.16 | regression

    Returns (gl_p99, metal_p99) on success. None if the file is absent,
    unreadable, or doesn't contain a parseable row.

    Why this fallback exists: `latest_parseable_jitter` previously
    depended on running `extract-perf-summary.sh` to derive p99 fields
    when `perf-summary.txt` was missing from the per-run dir. In a
    read-only execution environment (or when the launcher didn't write
    a perf-summary.txt), that subprocess call can fail silently and
    the gate reports p99 as MISSING even when the same data is sitting
    in `perf-diff.txt`. Reading the diff directly removes that
    dependency.
    """
    try:
        text = perf_diff.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    for line in text.splitlines():
        if not line.strip().startswith("post_load_mspf_max_p99"):
            continue
        parts = [part.strip() for part in line.split("|")]
        if len(parts) < 3:
            continue
        try:
            gl_p99 = float(parts[1])
            metal_p99 = float(parts[2])
        except ValueError:
            continue
        return gl_p99, metal_p99
    return None


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
        # Fallback: the compare run wrote `perf-diff.txt` next to its
        # summary.json with the same p99 metric already computed for
        # the regression report. Use that when re-extracting from the
        # per-run dir is not possible (subprocess unavailable,
        # `perf-summary.txt` missing on disk, etc.).
        perf_diff = path.parent / "perf-diff.txt"
        diff_pair = parse_perf_diff_p99(perf_diff)
        if diff_pair is not None and diff_pair[0] != 0:
            return path, data, diff_pair[0], diff_pair[1]
    return None



def read_shader_compile_counters(root: Path) -> tuple[str, str, str | None]:
    """Read the latest benchmark run's xemu.log for shader compile counters.

    Returns (status, detail, artifact_path).
    Status is "ok" if counters are present, "missing" if no run found.
    """
    # Find the latest benchmark run directory
    run_dirs = sorted(root.iterdir(), key=lambda p: p.stat().st_mtime, reverse=True)
    for run_dir in run_dirs:
        if not run_dir.is_dir():
            continue
        log_path = run_dir / "xemu.log"
        if not log_path.exists():
            continue
        # Read the first interval line (cold start)
        try:
            text = log_path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for line in text.splitlines():
            if not line.startswith("xemu-perf:"):
                continue
            # Parse key=value pairs
            counters = {}
            for token in line.split():
                if "=" in token:
                    k, v = token.split("=", 1)
                    counters[k] = v
            queued = counters.get("METAL_SHADER_COMPILE_QUEUED_TOTAL", "0")
            completed = counters.get("METAL_SHADER_COMPILE_COMPLETED_TOTAL", "0")
            failed = counters.get("METAL_SHADER_COMPILE_FAILED_TOTAL", "0")
            cache_loads = counters.get("METAL_SHADER_CACHE_LOADS", "0")
            cache_hits = counters.get("METAL_SHADER_CACHE_HITS", "0")
            cache_misses = counters.get("METAL_SHADER_CACHE_MISSES", "0")
            ubershader = counters.get("METAL_DRAWS_USING_UBERSHADER_TOTAL", "0")
            skipped = counters.get("METAL_DRAWS_SKIPPED_PENDING_TOTAL", "0")
            # If we have shader compile counters, this is a valid proof
            if int(queued) > 0:
                detail = (
                    "queued=" + queued + " completed=" + completed + " failed=" + failed + " "
                    "cache_loads=" + cache_loads + " hits=" + cache_hits + " misses=" + cache_misses + " "
                    "ubershader=" + ubershader + " skipped=" + skipped
                )
                return "ok", detail, str(log_path)
            # If we found an interval but no shader counters, keep looking
    return "missing", "no Metal shader compile counters found in any benchmark run", None

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

    shader_status, shader_detail, shader_artifact = read_shader_compile_counters(root)
    checks.append(Check(
        "cold shader compile proof",
        shader_status,
        shader_detail,
        shader_artifact,
    ))

    # Front-fb fallback policy: resolved when a decision-log entry uses
    # one of the policy-decision markers below. The markers are
    # phrased so any policy outcome (default-on, opt-in stays, faithful
    # CRTC publish replaces the fallback) registers — the gate is
    # "policy is decided", not "policy is a specific value".
    decision_log = repo / "docs/apple-silicon/decision-log.md"
    fallback_markers = (
        "Front-fb fallback policy stays opt-in",
        "Front-fb fallback policy: default-on",
        "Front-fb fallback replaced by faithful CRTC publish",
    )
    fallback_resolved_line: str | None = None
    try:
        text = decision_log.read_text(encoding="utf-8", errors="replace")
    except OSError:
        text = ""
    for marker in fallback_markers:
        if marker.lower() in text.lower():
            fallback_resolved_line = marker
            break
    if fallback_resolved_line:
        checks.append(Check(
            "front-fb fallback policy",
            "ok",
            f"decision-log records: \"{fallback_resolved_line}\"",
            str(decision_log),
        ))
    else:
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
