#!/usr/bin/env python3
"""Generate the NV2A feature-surface coverage matrix (the Workstream-A burndown).

Pure offline aggregation — does NOT launch xemu. It joins three data sources:
  1. XBE manifests   (scripts/apple-silicon/xbe-tests/*/manifest.json) — the
     `catalog_ref` field maps each XBE to NV2A feature surfaces (e.g. "§A.6, §C.8",
     ranges like "§D.1-D.7"). NOTE: catalog_ref is NOT parsed by xbe_discover into
     the XbeManifest dataclass, so we read the raw JSON here.
  2. Harness results (benchmark-runs/*/summary.json) — per-cell PASS/FAIL/xfail
     verdicts written by xbe_orchestrator.py run. We take the LATEST status per
     (xbe, renderer) across all runs supplied.
  3. Feature catalog (docs/apple-silicon/nv2a-feature-surface-research.md) — the
     canonical list of every NV2A surface, parsed from `### X.N Title` headers.
     This is the denominator: it lets us show UNCOVERED surfaces, not just
     XBE-centric status.

Outputs an XBE matrix + a per-surface coverage view + a tally, as markdown
and/or JSON. See metal-parity-roadmap.md (M0) and xbe-coverage-matrix.md.

Usage:
  xbe-coverage-matrix.py                      # scan ./benchmark-runs, print md to stdout
  xbe-coverage-matrix.py --runs A/summary.json B/  --out-md docs/apple-silicon/xbe-coverage-matrix.md
  xbe-coverage-matrix.py --out-json /tmp/coverage.json
"""
import argparse
import glob
import json
import os
import re
import sys
from collections import defaultdict
from datetime import datetime, timezone

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DEF_XBE_DIR = os.path.join(REPO, "scripts", "apple-silicon", "xbe-tests")
DEF_CATALOG = os.path.join(REPO, "docs", "apple-silicon", "nv2a-feature-surface-research.md")
DEF_RUNS_DIR = os.path.join(REPO, "benchmark-runs")

# Renderers we report columns for, in display order.
RENDERERS = ["metal", "gl", "real-xbox"]

STATUS_ICON = {
    "pass": "PASS",
    "fail": "FAIL",
    "expected_fail": "xfail",
    "skip": "skip",
    "not-built": "not-built",
    "infra-error": "ERR",
    "ok": "ok?",
    "": "-",
    None: "-",
}


def expand_catalog_ref(ref):
    """'§A.6, §C.8' -> ['A.6','C.8'];  '§D.1-D.7' -> ['D.1'..'D.7'].

    Returns a sorted unique list of surface ids (no § prefix)."""
    if not ref:
        return []
    out = set()
    # strip § and split on commas / 'and'
    tokens = re.split(r"[,;]| and ", ref.replace("§", ""))
    for tok in tokens:
        tok = tok.strip()
        if not tok:
            continue
        # range like D.1-D.7  or  D.1-7
        m = re.match(r"^([A-Z])\.(\d+)\s*-\s*(?:([A-Z])\.)?(\d+)$", tok)
        if m:
            sec, lo, sec2, hi = m.group(1), int(m.group(2)), m.group(3), int(m.group(4))
            sec2 = sec2 or sec
            if sec2 == sec:
                for n in range(lo, hi + 1):
                    out.add(f"{sec}.{n}")
                continue
        m = re.match(r"^([A-Z])\.(\d+)$", tok)
        if m:
            out.add(tok)
            continue
        # anything else (e.g. 3b.5, J.1 free-form) keep verbatim
        out.add(tok)
    return sorted(out, key=_surface_sort_key)


def _surface_sort_key(s):
    m = re.match(r"^([A-Za-z]+)\.(\d+)$", s)
    if m:
        return (0, m.group(1), int(m.group(2)))
    return (1, s, 0)


def parse_catalog(path):
    """Parse canonical surface list from '### X.N Title' headers.

    Returns OrderedDict-ish list of (surface_id, title, group_letter, group_title)."""
    surfaces = []
    groups = {}
    if not os.path.exists(path):
        return surfaces, groups
    cur_group = None
    with open(path, encoding="utf-8") as f:
        for line in f:
            mg = re.match(r"^##\s+([A-Z])\.\s+(.*)$", line)
            if mg:
                cur_group = mg.group(1)
                groups[cur_group] = mg.group(2).strip()
                continue
            ms = re.match(r"^###\s+([A-Z])\.(\d+)\s+(.*)$", line)
            if ms:
                sid = f"{ms.group(1)}.{ms.group(2)}"
                surfaces.append((sid, ms.group(3).strip(), ms.group(1)))
    return surfaces, groups


def load_manifests(xbe_dir):
    """Return list of dicts with id, title, tier, real_xbox_only, expected_fail_renderers,
    catalog_ref, surfaces, purpose, built."""
    out = []
    for mpath in sorted(glob.glob(os.path.join(xbe_dir, "*", "manifest.json"))):
        d = os.path.dirname(mpath)
        try:
            man = json.load(open(mpath, encoding="utf-8"))
        except Exception as e:
            print(f"WARN: bad manifest {mpath}: {e}", file=sys.stderr)
            continue
        xid = man.get("id") or os.path.basename(d)
        iso = os.path.join(d, f"{xid}.iso")
        xbe = os.path.join(d, "bin", "default.xbe")
        out.append({
            "id": xid,
            "title": man.get("title", ""),
            "tier": man.get("self_validation_tier", 1),
            "real_xbox_only": bool(man.get("real_xbox_only", False)),
            "expected_fail_renderers": man.get("expected_fail_renderers", []),
            "catalog_ref": man.get("catalog_ref", ""),
            "surfaces": expand_catalog_ref(man.get("catalog_ref", "")),
            "purpose": (man.get("purpose", "") or "")[:160],
            "built": os.path.exists(iso) and os.path.exists(xbe),
        })
    return out


def iter_summary_paths(runs):
    """runs: list of summary.json files or dirs. Yield summary.json paths."""
    for r in runs:
        if os.path.isdir(r):
            yield from sorted(glob.glob(os.path.join(r, "**", "summary.json"), recursive=True))
        elif os.path.basename(r) == "summary.json" and os.path.exists(r):
            yield r
        elif os.path.exists(r):
            yield r


def load_results(runs):
    """Return {(xbe, renderer): {status, run, ts, variant, notes}} taking latest by ts.

    Only the canonical recipe_variant is used for the headline status (additional
    recipes are reported separately if needed)."""
    latest = {}
    run_index = []
    for sp in iter_summary_paths(runs):
        try:
            data = json.load(open(sp, encoding="utf-8"))
        except Exception as e:
            print(f"WARN: bad summary {sp}: {e}", file=sys.stderr)
            continue
        ts = data.get("finished_at") or data.get("started_at") or 0
        run_index.append({"path": sp, "ts": ts, "renderers": data.get("renderers", []),
                          "n_results": len(data.get("results", []))})
        for cell in data.get("results", []):
            variant = cell.get("recipe_variant", "canonical")
            if variant not in ("canonical", "", None):
                continue  # headline = canonical recipe only
            key = (cell.get("xbe"), cell.get("renderer"))
            prev = latest.get(key)
            if prev is None or ts >= prev["ts"]:
                latest[key] = {
                    "status": cell.get("status"),
                    "run": sp,
                    "ts": ts,
                    "notes": cell.get("notes", ""),
                    "signal_pct": cell.get("best_frame_signal_match_pct"),
                    "counter": (cell.get("counter_assertion") or {}).get("status"),
                }
    return latest, run_index


def ts_str(ts):
    if not ts:
        return "n/a"
    try:
        return datetime.fromtimestamp(ts, timezone.utc).strftime("%Y-%m-%d %H:%MZ")
    except Exception:
        return str(ts)


def build_report(manifests, results, surfaces, groups, run_index):
    # XBE-centric rows
    xbe_rows = []
    for m in manifests:
        row = {"id": m["id"], "tier": m["tier"], "catalog_ref": m["catalog_ref"],
               "built": m["built"], "real_xbox_only": m["real_xbox_only"],
               "expected_fail_renderers": m["expected_fail_renderers"],
               "statuses": {}}
        for rnd in RENDERERS:
            res = results.get((m["id"], rnd))
            row["statuses"][rnd] = res["status"] if res else None
        xbe_rows.append(row)

    # Surface-centric coverage
    surface_to_xbes = defaultdict(list)
    for m in manifests:
        for s in m["surfaces"]:
            surface_to_xbes[s].append(m["id"])

    surface_rows = []
    catalog_ids = [s[0] for s in surfaces]
    for sid, title, grp in surfaces:
        covering = surface_to_xbes.get(sid, [])
        # best metal status among covering XBEs
        best = None
        order = ["pass", "expected_fail", "fail", "infra-error", "not-built", "skip", None]
        for x in covering:
            st = (results.get((x, "metal")) or {}).get("status")
            if best is None or order.index(st if st in order else None) < order.index(best if best in order else None):
                best = st
        surface_rows.append({"surface": sid, "title": title, "group": grp,
                             "xbes": covering, "metal_status": best,
                             "covered": bool(covering)})

    # surfaces referenced by manifests but NOT in the catalog (e.g. J.*, K.*, 3b.*)
    referenced = set()
    for m in manifests:
        referenced.update(m["surfaces"])
    extra_refs = sorted(referenced - set(catalog_ids), key=_surface_sort_key)

    # Tally
    tally = defaultdict(int)
    for (xid, rnd), res in results.items():
        if rnd == "metal":
            tally[res["status"] or "none"] += 1
    n_surfaces = len(surfaces)
    n_covered = sum(1 for r in surface_rows if r["covered"])
    n_green = sum(1 for r in surface_rows if r["metal_status"] in ("pass", "expected_fail"))

    return {
        "xbe_rows": xbe_rows, "surface_rows": surface_rows, "extra_refs": extra_refs,
        "tally": dict(tally), "run_index": run_index,
        "n_surfaces": n_surfaces, "n_covered": n_covered, "n_green": n_green,
        "groups": groups,
    }


def render_md(rep, runs_desc):
    L = []
    now = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%MZ")
    L.append("# XBE Coverage Matrix (NV2A feature-surface burndown)")
    L.append("")
    L.append(f"> **GENERATED by `scripts/apple-silicon/xbe-coverage-matrix.py` at {now}.** "
             "Do not hand-edit — re-run the generator. The single source of truth for "
             "Workstream-A progress (see `metal-parity-roadmap.md`).")
    L.append(">")
    L.append(f"> Result runs ingested: {runs_desc or 'none (inventory-only)'}")
    L.append("")
    # Run provenance
    if rep["run_index"]:
        L.append("## Result provenance")
        L.append("")
        L.append("| run summary.json | finished | renderers | cells |")
        L.append("|---|---|---|---|")
        for r in sorted(rep["run_index"], key=lambda x: x["ts"], reverse=True):
            rel = os.path.relpath(r["path"], REPO)
            L.append(f"| `{rel}` | {ts_str(r['ts'])} | {','.join(r['renderers'])} | {r['n_results']} |")
        L.append("")
    else:
        L.append("## Result provenance")
        L.append("")
        L.append("**No harness result runs ingested** — statuses below are blank "
                 "(inventory-only). Run the harness and re-generate to populate.")
        L.append("")

    # Tally
    L.append("## Tally (Metal, canonical recipe)")
    L.append("")
    t = rep["tally"]
    L.append(f"- XBE cells with a Metal result: PASS {t.get('pass',0)}, "
             f"xfail {t.get('expected_fail',0)}, FAIL {t.get('fail',0)}, "
             f"ERR {t.get('infra-error',0)}, not-built {t.get('not-built',0)}, skip {t.get('skip',0)}")
    L.append(f"- Feature surfaces (catalog): {rep['n_surfaces']} total, "
             f"{rep['n_covered']} have ≥1 XBE, {rep['n_green']} green on Metal "
             f"(PASS or xfail). **Uncovered: {rep['n_surfaces']-rep['n_covered']}.**")
    L.append("")

    # XBE matrix
    L.append("## XBE matrix (status per renderer, canonical recipe, latest run)")
    L.append("")
    L.append("| XBE | tier | surfaces (catalog_ref) | Metal | GL | real-Xbox | built | xfail-on |")
    L.append("|---|---|---|---|---|---|---|---|")
    for row in sorted(rep["xbe_rows"], key=lambda r: r["id"]):
        st = row["statuses"]
        L.append("| `{id}` | {tier} | {ref} | {m} | {g} | {rx} | {b} | {ef} |".format(
            id=row["id"], tier=row["tier"], ref=row["catalog_ref"] or "—",
            m=STATUS_ICON.get(st["metal"], st["metal"] or "-"),
            g=STATUS_ICON.get(st["gl"], st["gl"] or "-"),
            rx=STATUS_ICON.get(st["real-xbox"], st["real-xbox"] or "-"),
            b="yes" if row["built"] else "**NO**",
            ef=",".join(row["expected_fail_renderers"]) or "—"))
    L.append("")

    # Surface coverage
    L.append("## Feature-surface coverage (canonical catalog × XBE)")
    L.append("")
    L.append("Denominator parsed from `nv2a-feature-surface-research.md` `### X.N` headers. "
             "`metal` column = best status among covering XBEs.")
    L.append("")
    cur_grp = None
    for r in rep["surface_rows"]:
        if r["group"] != cur_grp:
            cur_grp = r["group"]
            gtitle = rep["groups"].get(cur_grp, "")
            L.append("")
            L.append(f"### {cur_grp}. {gtitle}")
            L.append("")
            L.append("| surface | title | XBE(s) | Metal |")
            L.append("|---|---|---|---|")
        cov = ", ".join(f"`{x}`" for x in r["xbes"]) if r["xbes"] else "**none**"
        L.append(f"| {r['surface']} | {r['title']} | {cov} | "
                 f"{STATUS_ICON.get(r['metal_status'], r['metal_status'] or '-')} |")
    L.append("")

    if rep["extra_refs"]:
        L.append("## catalog_refs referenced by XBEs but outside the numbered catalog")
        L.append("")
        L.append(", ".join(rep["extra_refs"]))
        L.append("")
    return "\n".join(L) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--runs", nargs="*", default=None,
                    help="summary.json files or dirs to scan (default: ./benchmark-runs)")
    ap.add_argument("--xbe-dir", default=DEF_XBE_DIR)
    ap.add_argument("--catalog", default=DEF_CATALOG)
    ap.add_argument("--out-md", default=None, help="write markdown here (default stdout)")
    ap.add_argument("--out-json", default=None, help="write json artifact here")
    args = ap.parse_args()

    runs = args.runs if args.runs is not None else [DEF_RUNS_DIR]
    manifests = load_manifests(args.xbe_dir)
    results, run_index = load_results(runs)
    surfaces, groups = parse_catalog(args.catalog)
    rep = build_report(manifests, results, surfaces, groups, run_index)

    runs_desc = ", ".join(os.path.relpath(r["path"], REPO) for r in run_index) if run_index else ""
    md = render_md(rep, runs_desc)

    if args.out_md:
        open(args.out_md, "w", encoding="utf-8").write(md)
        print(f"wrote {args.out_md} ({len(md)} bytes)", file=sys.stderr)
    else:
        sys.stdout.write(md)

    if args.out_json:
        json.dump(rep, open(args.out_json, "w", encoding="utf-8"), indent=2, default=str)
        print(f"wrote {args.out_json}", file=sys.stderr)


if __name__ == "__main__":
    main()
