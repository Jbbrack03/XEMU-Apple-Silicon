#!/usr/bin/env python3
"""
xbe_orchestrator — top-level driver for the diagnostic-XBE pipeline.

The production correctness gate for the Metal-renderer port: given a
set of Tier-1 diag XBEs (`mirror`, `color-channel`, `depth-floor`, ...),
run each one against every available renderer (xemu-GL, xemu-Metal,
real-Xbox), compare each capture against the per-XBE math-derived or
real-Xbox-canonical reference, and emit a per-cell PASS/FAIL matrix.

When the matrix is all-PASS for the active manifest, we have empirical
evidence the Metal renderer is correct on the feature surface those
XBEs cover. When a real game then misrenders, the bug is *outside*
that feature surface — narrowing the search.

CLI
---

  list                              Discover XBEs from xbe-tests/.
  probe                             Probe renderer availability.
  expected --xbe ID --out PATH      Generate math-derived expected PNG.
  capture-reference --xbe ID        Pull canonical real-Xbox PNG to
                                    docs/apple-silicon/xbox-real-references/<id>/.
  run [--xbe ID...] [--renderer R...] [--out DIR] [--surface-scale N]
                                    Run the matrix; default = all
                                    Tier-1 XBEs × all available
                                    renderers. Writes report.md +
                                    summary.json under --out.

Default surface scale is 1 to match diag-XBE-plan §2.6 (forces guest
640×480 = host 640×480; resolves coordinate confusion).
"""
from __future__ import annotations

import argparse
import json
import os
import socket
import subprocess
import sys
import time
from dataclasses import asdict
from pathlib import Path
from typing import Dict, List, Optional

import xbe_compare
import xbe_discover
import xbe_renderers


_HERE = Path(__file__).resolve().parent
DEFAULT_OUT_ROOT = _HERE.parent.parent.parent / "benchmark-runs"


# ---------- per-renderer flag recipes ----------

# The "canonical" Apple Silicon Metal recipe (per project rule #11 +
# the 2026-05-04/05 PGR2 / Crimson canary work). Always-on flags are
# not duplicated here — xbe_renderers.run_xemu sets them via env
# defaults already.
METAL_CANONICAL_RECIPE: Dict[str, str] = {
    "XEMU_METAL_TRANSLATED_PIPELINE": "1",
    "XEMU_METAL_FRONT_FB_FALLBACK": "1",
    "XEMU_METAL_MSAA": "0",  # diag XBEs render flat patterns; no AA needed
    "XEMU_METAL_HUD": "0",
    "XEMU_METAL_VALIDATION": "1",
}

GL_CANONICAL_RECIPE: Dict[str, str] = {
    # GL canary recipe lives mostly in env defaults. No MSAA for
    # diag XBEs (forces byte-exact comparison).
    "XEMU_GL_MSAA": "0",
}


# ---------- helpers ----------

def _renderer_available(name: str) -> bool:
    name = name.lower()
    if name in ("gl", "metal"):
        return xbe_renderers.XEMU_BIN.exists()
    if name in ("real-xbox", "real_xbox", "xbox"):
        return _real_xbox_alive()
    return False


def _real_xbox_alive(host: Optional[str] = None) -> bool:
    host = host or os.environ.get("ORACLE_HOST", "192.168.0.200")
    return _ping(host) and (xbe_renderers._agent_alive(host) or _ftp_alive(host))


def _ping(host: str) -> bool:
    rc = subprocess.call(["ping", "-c", "1", "-W", "2000", host],
                         stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL)
    return rc == 0


def _ftp_alive(host: str, port: int = 21, timeout: float = 2.0) -> bool:
    try:
        s = socket.create_connection((host, port), timeout=timeout)
        s.close()
        return True
    except OSError:
        return False


# ---------- matrix run ----------

def run_matrix(xbe_ids: List[str], renderers: List[str], out_root: Path,
               surface_scale: int = 1, threshold: int = 0,
               max_changed_pct: float = 0.0,
               crop: str = "0,0,640,480",
               real_xbox_host: str = "192.168.0.200",
               upload_xbe: bool = True) -> dict:
    """For each (XBE × renderer), run + capture + compare. Return a
    summary dict suitable for JSON serialization."""
    out_root.mkdir(parents=True, exist_ok=True)
    report: dict = {
        "started_at": time.time(),
        "xbe_ids": xbe_ids,
        "renderers": renderers,
        "surface_scale": surface_scale,
        "threshold": threshold,
        "crop": crop,
        "real_xbox_host": real_xbox_host,
        "results": [],
    }

    manifests = xbe_discover.discover_all(only=xbe_ids)
    found_ids = {m.id for m in manifests}
    missing = [x for x in xbe_ids if x not in found_ids]
    if missing:
        report["missing_xbes"] = missing

    for m in manifests:
        if not m.xbe_path.exists():
            report["results"].append({
                "xbe": m.id, "status": "not-built",
                "note": f"run `make` in {m.dir}",
            })
            continue
        for renderer in renderers:
            cell_dir = out_root / m.id / renderer
            cell_dir.mkdir(parents=True, exist_ok=True)
            # Honor `real_xbox_only`: a diag XBE that depends on the
            # real-Xbox kernel-pool controller buffer (or any other
            # real-only resource) cannot run on xemu-GL/Metal. Skip
            # those cells with an explicit "skip" status so the
            # gate runner counts them as not-applicable rather than
            # spurious failures.
            if (m.real_xbox_only and
                    renderer.lower() not in ("real-xbox", "real_xbox", "xbox")):
                print(f"[xbe-harness] skipping {m.id} on {renderer} "
                      f"(real_xbox_only=true)", flush=True)
                report["results"].append({
                    "xbe": m.id, "renderer": renderer,
                    "status": "skip",
                    "notes": "real_xbox_only",
                })
                continue
            print(f"[xbe-harness] running {m.id} on {renderer} ...",
                  flush=True)
            run_t0 = time.time()
            run = _drive(m, renderer, cell_dir, surface_scale,
                         real_xbox_host=real_xbox_host,
                         upload_xbe=upload_xbe)
            run_t1 = time.time()
            cell: dict = {
                "xbe": m.id,
                "renderer": renderer,
                "run_seconds": round(run_t1 - run_t0, 2),
                "captured_png": str(run.captured_png),
                "status": run.status,
                "notes": run.notes,
            }
            if run.status == "ok":
                # Pick reference & compare. For xemu runs the diag XBE
                # renders a brief window then reboots, so only a subset
                # of the captured PNGs show our pattern. Iterate over
                # all captured PNGs and pick the one with the lowest
                # changed_pixels_pct against the reference; that's the
                # frame that landed during the diag render.
                try:
                    # Build the flag-recipe dict from the canonical
                    # recipe + per-cell overrides so the reference
                    # key actually reflects what we ran (e.g.
                    # FRONT_FB_FALLBACK affects expected output for
                    # crtc-publish-style XBEs).
                    recipe_flags = _flag_recipe_dict(renderer)
                    cell_recipe = {"scale": surface_scale, "msaa": 0,
                                   **recipe_flags}
                    key = xbe_compare.select_reference_key(
                        m, renderer, cell_recipe)
                    cell["reference_key"] = key
                    ref_png, ref_kind = xbe_compare.resolve_reference_png(
                        m, key, cell_dir)
                    cell["reference_kind"] = ref_kind
                    candidates = (run.extra or {}).get(
                        "screenshots", [str(run.captured_png)])
                    # Frame selection: pick the candidate with the
                    # HIGHEST signal_match_pct (i.e. the frame where
                    # the expected non-black pixels are actually
                    # rendered). Tiebreak by lowest changed_pixels_pct.
                    # Using changed_pixels_pct alone is biased AGAINST
                    # diag-render frames for sparse oracles like
                    # mirror: a fully-black BIOS frame has
                    # changed_pixels_pct=0.005% (the missing white
                    # block), beating the correctly-rendered frame
                    # which adds sub-pixel anti-aliasing drift to the
                    # signal pixels.
                    best_signal = -1.0
                    best_pct: Optional[float] = None
                    best_path: Optional[str] = None
                    for cand in candidates:
                        sig_pass, sig_notes = xbe_compare.signal_match_check(
                            Path(cand), ref_png,
                            threshold=max(threshold, 16),
                            min_signal_match_pct=0.0)
                        sig_pct = 0.0
                        for tok in sig_notes.split():
                            if tok.startswith("signal_match_pct="):
                                try:
                                    sig_pct = float(tok.split("=", 1)[1])
                                except ValueError:
                                    pass
                        # Cheap percent-changed scan only for tiebreak
                        # (skip running compare-screenshots for every
                        # candidate; do it once on the chosen frame).
                        if (sig_pct > best_signal or
                                (abs(sig_pct - best_signal) < 1e-6 and
                                 best_path is None)):
                            best_signal = sig_pct
                            best_path = cand
                    if best_path is None:
                        cell["status"] = "infra-error"
                        cell["notes"] = "no comparable screenshots"
                    else:
                        # Re-run the compare with the chosen frame for
                        # the canonical artifact dir + final gate.
                        cell["captured_png"] = best_path
                        verdict = xbe_compare.compare(
                            Path(best_path), ref_png,
                            out_dir=cell_dir / "compare",
                            crop=crop, threshold=threshold,
                            max_changed_pct=max_changed_pct,
                        )
                        cell["verdict"] = verdict.as_dict()
                        cell["best_frame_count"] = len(candidates)
                        cell["best_frame_signal_match_pct"] = best_signal
                        cell["status"] = verdict.status
                except (KeyError, FileNotFoundError, ValueError) as e:
                    cell["status"] = "infra-error"
                    cell["notes"] = (cell["notes"] + " | " if cell["notes"]
                                     else "") + f"reference: {e}"
            report["results"].append(cell)
            print(f"[xbe-harness]   → {cell['status']} "
                  f"({cell.get('notes', '')})",
                  flush=True)

    report["finished_at"] = time.time()
    summary_path = out_root / "summary.json"
    summary_path.write_text(json.dumps(report, indent=2, default=str))

    report_md = _format_report_md(report)
    (out_root / "report.md").write_text(report_md)

    return report


def _flag_recipe_dict(renderer: str) -> Dict[str, str]:
    """Translate the renderer's canonical recipe (METAL_/GL_)
    into the recipe-key dict format `select_reference_key` expects."""
    r = renderer.lower()
    if r == "metal":
        return {
            "fallback": "1" if METAL_CANONICAL_RECIPE.get(
                "XEMU_METAL_FRONT_FB_FALLBACK") == "1" else "0",
            "translated": "1" if METAL_CANONICAL_RECIPE.get(
                "XEMU_METAL_TRANSLATED_PIPELINE") == "1" else "0",
            "msaa": METAL_CANONICAL_RECIPE.get("XEMU_METAL_MSAA", "0"),
        }
    if r == "gl":
        return {
            "msaa": GL_CANONICAL_RECIPE.get("XEMU_GL_MSAA", "0"),
        }
    if r in ("real-xbox", "real_xbox", "xbox"):
        return {}
    return {}


def _verdict_pct(v) -> Optional[float]:
    """Extract changed_pixels_pct from a CompareVerdict's stdout."""
    for line in (v.stdout or "").splitlines():
        if line.startswith("changed_pixels_pct="):
            try:
                return float(line.split("=", 1)[1].strip())
            except ValueError:
                return None
    return None


def _drive(manifest: xbe_discover.XbeManifest, renderer: str,
           cell_dir: Path, surface_scale: int,
           real_xbox_host: str, upload_xbe: bool) -> xbe_renderers.RunResult:
    r = renderer.lower()
    if r == "gl":
        return xbe_renderers.run_xemu(manifest, "GL", cell_dir,
                                      flag_recipe=GL_CANONICAL_RECIPE,
                                      surface_scale=surface_scale)
    if r == "metal":
        return xbe_renderers.run_xemu(manifest, "METAL", cell_dir,
                                      flag_recipe=METAL_CANONICAL_RECIPE,
                                      surface_scale=surface_scale)
    if r in ("real-xbox", "real_xbox", "xbox"):
        return xbe_renderers.run_real_xbox(manifest, cell_dir,
                                           host=real_xbox_host,
                                           upload_xbe=upload_xbe)
    return xbe_renderers.RunResult("infra-error",
                                   cell_dir / "no-capture.png",
                                   "", notes=f"unknown renderer: {renderer}")


def _format_report_md(report: dict) -> str:
    lines = [
        "# xbe-harness report",
        "",
        f"- Started: {time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(report['started_at']))}",
        f"- XBEs: {', '.join(report['xbe_ids']) or '(all)'}",
        f"- Renderers: {', '.join(report['renderers'])}",
        f"- Surface scale: {report['surface_scale']}",
        f"- Compare threshold (per-channel): {report['threshold']}",
        f"- Real-Xbox host: {report['real_xbox_host']}",
        "",
        "## Results matrix",
        "",
        "| XBE | Renderer | Status | Notes |",
        "|---|---|---|---|",
    ]
    for c in report["results"]:
        lines.append(
            f"| {c['xbe']} | {c.get('renderer', '-')} | "
            f"{c['status']} | {c.get('notes', '').replace('|', '\\|')} |"
        )
    lines.append("")
    lines.append("## Per-cell artifacts")
    lines.append("")
    for c in report["results"]:
        if "captured_png" in c:
            lines.append(f"- {c['xbe']} / {c.get('renderer', '-')}: "
                         f"`{c['captured_png']}`")
            if "verdict" in c:
                lines.append(f"  - reference: `{c['verdict']['reference']}`")
                lines.append(f"  - compare-out: `{c['verdict']['out_dir']}`")
    return "\n".join(lines) + "\n"


# ---------- CLI ----------

def cmd_list(args) -> int:
    items = xbe_discover.discover_all()
    if not items:
        print("no XBEs discovered", file=sys.stderr)
        return 1
    for m in items:
        built = "BUILT" if m.xbe_path.exists() else "not-built"
        print(f"{m.id:18}  tier={m.self_validation_tier}  {built}  "
              f"{m.title}")
    return 0


def cmd_probe(args) -> int:
    out = {}
    out["xemu_binary_present"] = xbe_renderers.XEMU_BIN.exists()
    out["xemu_binary_path"] = str(xbe_renderers.XEMU_BIN)
    out["real_xbox_alive"] = _real_xbox_alive(args.host)
    out["real_xbox_host"] = args.host
    print(json.dumps(out, indent=2))
    return 0 if (out["xemu_binary_present"] or out["real_xbox_alive"]) else 1


def cmd_expected(args) -> int:
    m = xbe_discover.get(args.xbe)
    spec = m.expected_results.get("any/any/any")
    if not spec or spec.get("kind") != "math-derived":
        print(f"xbe {args.xbe} has no math-derived 'any/any/any' "
              f"reference", file=sys.stderr)
        return 1
    w, h = xbe_compare.synthesize_expected_png(m, spec["generator"],
                                                Path(args.out))
    print(f"wrote {w}x{h} expected PNG to {args.out}")
    return 0


def cmd_capture_reference(args) -> int:
    """Capture a canonical real-Xbox reference frame for an XBE.
    Equivalent to run_real_xbox + copy-into-tracked-references-dir.
    Run this once per XBE per console; the captured PNG becomes the
    canonical oracle for that XBE going forward."""
    if not _real_xbox_alive(args.host):
        print(f"real Xbox not reachable at {args.host}", file=sys.stderr)
        return 1
    m = xbe_discover.get(args.xbe)
    work = Path(args.work_dir or f"/tmp/xbe-capture-ref-{m.id}")
    work.mkdir(parents=True, exist_ok=True)
    res = xbe_renderers.run_real_xbox(m, work, host=args.host,
                                      upload_xbe=not args.no_upload)
    if res.status != "ok":
        print(f"capture failed: {res.notes}\n{res.log}", file=sys.stderr)
        return 1
    dest_dir = m.real_xbox_reference_dir
    dest_dir.mkdir(parents=True, exist_ok=True)
    label = args.label or "real-xbox"
    dest_png = dest_dir / f"{label}.png"
    dest_png.write_bytes(res.captured_png.read_bytes())
    print(f"wrote canonical reference: {dest_png}")
    return 0


def cmd_run(args) -> int:
    out_root = Path(args.out) if args.out else (
        DEFAULT_OUT_ROOT / time.strftime("xbe-harness-%Y%m%d-%H%M%S"))
    xbe_ids = args.xbe or [
        m.id for m in xbe_discover.discover_all()
        if m.self_validation_tier == 1 and not m.real_xbox_only
    ]
    renderers = args.renderer or _autodetect_renderers(args.host)
    print(f"[xbe-harness] xbe_ids: {xbe_ids}", flush=True)
    print(f"[xbe-harness] renderers: {renderers}", flush=True)
    print(f"[xbe-harness] out: {out_root}", flush=True)
    rep = run_matrix(xbe_ids, renderers, out_root,
                     surface_scale=args.surface_scale,
                     threshold=args.threshold,
                     max_changed_pct=args.max_changed_pct,
                     crop=args.crop,
                     real_xbox_host=args.host,
                     upload_xbe=not args.no_upload)
    pass_count = sum(1 for c in rep["results"] if c["status"] == "pass")
    fail_count = sum(1 for c in rep["results"] if c["status"] == "fail")
    skip_count = sum(1 for c in rep["results"] if c["status"] == "skip")
    err_count = sum(1 for c in rep["results"]
                    if c["status"] not in ("pass", "fail", "skip"))
    print(f"[xbe-harness] {pass_count} pass, {fail_count} fail, "
          f"{skip_count} skip, {err_count} infra-error", flush=True)
    print(f"[xbe-harness] report: {out_root}/report.md", flush=True)
    # Skips are not-applicable, not failures.
    return 0 if fail_count == 0 and err_count == 0 else 1


def _autodetect_renderers(host: str) -> List[str]:
    """Autodetect renderers for the matrix run. GL is intentionally
    excluded by default because window-targeted screencapture is
    fragile for diag XBEs (window aspect ratio differs from native;
    the boot animation tends to dominate the captured frames; a
    Quartz install is required). Pass --renderer gl explicitly to
    include it. Metal + real-Xbox cover the production gate."""
    out = []
    if xbe_renderers.XEMU_BIN.exists():
        out.append("metal")
    if _real_xbox_alive(host):
        out.append("real-xbox")
    return out


def main(argv: Optional[List[str]] = None) -> int:
    p = argparse.ArgumentParser(description="diag-XBE harness")
    p.add_argument("--host", default=os.environ.get("ORACLE_HOST",
                                                    "192.168.0.200"))
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("list", help="discover XBEs").set_defaults(fn=cmd_list)
    sub.add_parser("probe", help="probe renderer availability").set_defaults(
        fn=cmd_probe)

    pe = sub.add_parser("expected",
                        help="generate math-derived expected PNG")
    pe.add_argument("--xbe", required=True)
    pe.add_argument("--out", required=True)
    pe.set_defaults(fn=cmd_expected)

    pcr = sub.add_parser("capture-reference",
                         help="capture canonical real-Xbox reference frame")
    pcr.add_argument("--xbe", required=True)
    pcr.add_argument("--label", default="real-xbox",
                     help="reference filename (default: real-xbox)")
    pcr.add_argument("--work-dir",
                     help="scratch dir for the capture run "
                          "(default: /tmp/xbe-capture-ref-<id>)")
    pcr.add_argument("--no-upload", action="store_true",
                     help="skip XBE upload; assume already deployed")
    pcr.set_defaults(fn=cmd_capture_reference)

    pr = sub.add_parser("run", help="run the matrix")
    pr.add_argument("--xbe", action="append",
                    help="XBE id (repeat for many; default: all Tier-1)")
    pr.add_argument("--renderer", action="append",
                    help="renderer (gl|metal|real-xbox; default: autodetect)")
    pr.add_argument("--out", help="output dir (default: "
                                  "benchmark-runs/xbe-harness-<ts>)")
    pr.add_argument("--surface-scale", type=int, default=1,
                    help="XEMU_DISPLAY_SCALE (default: 1)")
    pr.add_argument("--threshold", type=int, default=8,
                    help="per-channel byte tolerance (default: 8 = "
                         "matches metal-gl-compare; 0 = exact)")
    pr.add_argument("--max-changed-pct", type=float, default=0.5,
                    help="max %% of pixels allowed to exceed --threshold "
                         "before failing (default: 0.5%%)")
    pr.add_argument("--crop", default="0,0,640,480",
                    help="x,y,w,h crop rect (default: full 640x480)")
    pr.add_argument("--no-upload", action="store_true",
                    help="skip real-Xbox XBE FTP upload step")
    pr.set_defaults(fn=cmd_run)

    args = p.parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
