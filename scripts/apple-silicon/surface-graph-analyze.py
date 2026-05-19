#!/usr/bin/env python3
"""Per-flip NV2A surface-graph analyzer (Tool 1, 2026-05-19).

Consumes the JSONL stream written by
`XEMU_METAL_SURFACE_GRAPH_DUMP=<path>` (with optional
`_AT_FLIP_STALL=N` / `_INTERVAL=N` gating) and emits a structured
report.

Background: the PGR2 multi-RT compositing investigation
(M5.12/M17 per `benchmarks/2026-05-11-pgr2-metal-render-path-
diagnostic.md`) needs to identify which cached `MtlSurfaceBinding`
holds the final composited cityscape — the title abandons the
CRTC-pointed surface after boot and uses a multi-RT pipeline
whose final stage is not yet known. Today this requires three
separate diagnostic xemu runs with
`XEMU_METAL_SCREENSHOT_SOURCE={drawable,vram:0x32a4000,vram:0x3628000}`
and manual cross-referencing. With this analyzer one xemu run
produces the entire surface graph per flip and the heuristic does
the elimination automatically.

Output:
  summary.json          per-flip + per-vram_addr aggregate metrics
  report.md             human-readable narrative with candidates
                        section and per-flip tables (first/last/mid)
  per-flip.csv          spreadsheet-friendly long-form rows
  candidates.md         standalone candidates writeup for fast eyes

Heuristics for "final-composite candidates" (per-flip):
  1) Color RT (is_color=true).
  2) `last_color_draw_seq > 0` — the binding has been color-drawn at
     SOME point in this run. Per Codex review 2026-05-19 finding #2,
     this avoids the cumulative-frame_draw_count trap (frame_draw_count
     only resets on fallback publish, not per flip). The filter is
     "drawn this run, ranked by recency" — implementing a true
     per-flip freshness gate would require comparing seq deltas across
     consecutive flip headers (not implemented; ranking by recency
     surfaces the same candidates in practice).
  3) Was NOT the publish source (`was_publish_source=false`) — the
     publish source is already what we suspect of being WRONG for
     PGR2 (it's the boot-residual or HUD surface).
  4) Has guest_width × guest_height that looks like a final-composite
     resolution (640x480 nominal, with a 2x scale tolerance).
  5) Ranked by `last_color_draw_seq` (recency) then by `frame_draw_count`
     (cumulative activity).

Exit codes:
  0  Wrote a complete report.
  2  INFRA-FAIL — no flips in input, or malformed JSONL.
"""

import argparse
import csv
import json
import sys
from collections import OrderedDict, defaultdict
from pathlib import Path


def parse_args():
    p = argparse.ArgumentParser(
        description="Analyze a surface-graph JSONL stream from "
                    "XEMU_METAL_SURFACE_GRAPH_DUMP.")
    p.add_argument("--jsonl", required=True, type=Path,
                   help="Path to the surface-graph JSONL file produced by "
                        "xemu (XEMU_METAL_SURFACE_GRAPH_DUMP=...).")
    p.add_argument("--out-dir", required=True, type=Path,
                   help="Output directory; created if missing.")
    p.add_argument("--target-width", type=int, default=640,
                   help="Guest-pixel width hint for the final-composite "
                        "candidate heuristic. Default 640 (PGR2 final-RT "
                        "hint per handoff).")
    p.add_argument("--target-height", type=int, default=480,
                   help="Guest-pixel height hint. Default 480.")
    p.add_argument("--target-format", type=int, default=None,
                   help="nv097_format hint (e.g., 4 for format-4 per the "
                        "2026-05-11 PGR2 diagnostic). If omitted, the "
                        "candidate filter ignores format.")
    p.add_argument("--scale-tolerance", type=float, default=2.5,
                   help="Allowed ratio of host_dim / guest_dim for shape "
                        "matching. Default 2.5 covers surface_scale=2 "
                        "with a small margin.")
    p.add_argument("--max-per-flip-rows", type=int, default=12,
                   help="Cap per-flip table rows in report.md.")
    return p.parse_args()


def parse_int_maybe_hex(s):
    """Parse '0x1234' or '4660' to int. Empty/None returns 0."""
    if s is None:
        return 0
    if isinstance(s, int):
        return int(s)
    s = str(s)
    if s.startswith("0x") or s.startswith("0X"):
        return int(s, 16)
    return int(s) if s else 0


def load_jsonl(path):
    """Returns (header_records_by_flip, binding_records_by_flip, ordered_flips).

    header_records_by_flip[flip_ordinal] = dict (the "flip" line).
    binding_records_by_flip[flip_ordinal] = list[dict] (the "binding" lines).
    ordered_flips = sorted list of flip_ordinals in first-seen order.
    """
    headers = OrderedDict()
    bindings = defaultdict(list)
    with path.open() as fh:
        for line_no, raw in enumerate(fh, start=1):
            raw = raw.strip()
            if not raw:
                continue
            try:
                rec = json.loads(raw)
            except json.JSONDecodeError as e:
                print(f"WARN: line {line_no} malformed JSON ({e}); "
                      "skipping", file=sys.stderr)
                continue
            t = rec.get("type")
            flip = rec.get("flip_ordinal")
            if flip is None:
                continue
            if t == "flip":
                headers[flip] = rec
            elif t == "binding":
                bindings[flip].append(rec)
    return headers, dict(bindings), list(headers.keys())


def shape_matches(b, target_w, target_h, tolerance):
    guest_w = int(b.get("guest_width", 0))
    guest_h = int(b.get("guest_height", 0))
    if guest_w == target_w and guest_h == target_h:
        return True
    host_w = int(b.get("width", 0))
    host_h = int(b.get("height", 0))
    if host_w == 0 or host_h == 0:
        return False
    # Tolerance ratio: host_dim / target_dim must be <= tolerance.
    rw = host_w / float(target_w) if target_w else 0.0
    rh = host_h / float(target_h) if target_h else 0.0
    if 0.5 <= rw <= tolerance and 0.5 <= rh <= tolerance:
        return True
    return False


def candidates_for_flip(binding_records, target_w, target_h, target_format,
                        tolerance):
    """Apply the elimination heuristic. Returns ranked list of dicts."""
    out = []
    for b in binding_records:
        if not b.get("is_color"):
            continue
        if b.get("was_publish_source"):
            continue
        last_seq = int(b.get("last_color_draw_seq", 0))
        if last_seq == 0:
            continue
        if not shape_matches(b, target_w, target_h, tolerance):
            continue
        if target_format is not None and \
                int(b.get("nv097_format", -1)) != target_format:
            continue
        out.append(b)

    # Rank by (last_color_draw_seq desc, frame_draw_count desc).
    out.sort(key=lambda b: (-int(b.get("last_color_draw_seq", 0)),
                            -int(b.get("frame_draw_count", 0))))
    return out


def write_per_flip_csv(out_path, headers, bindings, flips):
    fields = [
        "flip_ordinal", "seq", "ts_us", "cache_size",
        "vram_addr", "is_color",
        "width", "height", "guest_width", "guest_height",
        "nv097_format", "mtl_pixel_format",
        "frame_draw_count", "last_color_draw_seq", "last_use_seq",
        "draw_dirty", "dirty_vram",
        "is_current_color", "is_current_depth", "was_publish_source",
    ]
    with out_path.open("w", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        for flip in flips:
            head = headers.get(flip, {})
            for b in bindings.get(flip, []):
                row = dict(b)
                row["ts_us"] = head.get("ts_us")
                row["seq"] = head.get("seq")
                row["cache_size"] = head.get("cache_size")
                writer.writerow(row)


def build_summary(headers, bindings, flips, args):
    """Per-vram_addr aggregate + per-flip candidate counts."""
    per_addr = defaultdict(lambda: {
        "first_flip": None, "last_flip": None,
        "max_frame_draw_count": 0, "max_last_color_draw_seq": 0,
        "is_color_ever": False, "is_depth_ever": False,
        "ever_was_publish_source": False,
        "host_dims_seen": set(), "guest_dims_seen": set(),
        "nv097_formats_seen": set(),
    })
    per_flip_candidate_count = {}
    candidate_addrs_overall = set()

    for flip in flips:
        c = candidates_for_flip(bindings.get(flip, []),
                                args.target_width, args.target_height,
                                args.target_format, args.scale_tolerance)
        per_flip_candidate_count[flip] = len(c)
        for b in c:
            candidate_addrs_overall.add(b.get("vram_addr"))

        for b in bindings.get(flip, []):
            addr = b.get("vram_addr")
            a = per_addr[addr]
            if a["first_flip"] is None:
                a["first_flip"] = flip
            a["last_flip"] = flip
            a["max_frame_draw_count"] = max(
                a["max_frame_draw_count"], int(b.get("frame_draw_count", 0)))
            a["max_last_color_draw_seq"] = max(
                a["max_last_color_draw_seq"],
                int(b.get("last_color_draw_seq", 0)))
            if b.get("is_color"):
                a["is_color_ever"] = True
            else:
                a["is_depth_ever"] = True
            if b.get("was_publish_source"):
                a["ever_was_publish_source"] = True
            a["host_dims_seen"].add(f"{b.get('width')}x{b.get('height')}")
            a["guest_dims_seen"].add(
                f"{b.get('guest_width')}x{b.get('guest_height')}")
            a["nv097_formats_seen"].add(int(b.get("nv097_format", -1)))

    # Set -> sorted list for JSON-serializability.
    for a in per_addr.values():
        a["host_dims_seen"] = sorted(a["host_dims_seen"])
        a["guest_dims_seen"] = sorted(a["guest_dims_seen"])
        a["nv097_formats_seen"] = sorted(a["nv097_formats_seen"])

    return {
        "flip_count": len(flips),
        "first_flip": flips[0] if flips else None,
        "last_flip": flips[-1] if flips else None,
        "target_width": args.target_width,
        "target_height": args.target_height,
        "target_format": args.target_format,
        "scale_tolerance": args.scale_tolerance,
        "candidate_addrs_overall": sorted(candidate_addrs_overall),
        "per_flip_candidate_count": per_flip_candidate_count,
        "per_addr": per_addr,
    }


def fmt_row_md(b):
    return "| {addr} | {col} | {host} | {guest} | {fmt} | {fdc} | {lcds} | {publish} |".format(
        addr=b.get("vram_addr"),
        col="C" if b.get("is_color") else "D",
        host=f"{b.get('width')}×{b.get('height')}",
        guest=f"{b.get('guest_width')}×{b.get('guest_height')}",
        fmt=b.get("nv097_format"),
        fdc=b.get("frame_draw_count"),
        lcds=b.get("last_color_draw_seq"),
        publish="src" if b.get("was_publish_source") else "",
    )


def write_report_md(out_path, headers, bindings, flips, summary, args):
    lines = []
    lines.append("# Surface-graph analysis\n")
    lines.append(f"- jsonl: `{args.jsonl}`")
    lines.append(f"- flips_seen: {summary['flip_count']} "
                 f"({summary['first_flip']}..{summary['last_flip']})")
    lines.append(f"- candidate heuristic: is_color & last_color_draw_seq>0 & "
                 f"NOT publish-source & guest dims within "
                 f"±{args.scale_tolerance}× of "
                 f"{args.target_width}×{args.target_height}"
                 + (f" & nv097_format=={args.target_format}"
                    if args.target_format is not None else ""))
    lines.append("")

    # Overall candidates section.
    lines.append("## Final-composite candidates (across all flips)\n")
    if summary["candidate_addrs_overall"]:
        lines.append("| vram_addr | max frame_draw_count | "
                     "max last_color_draw_seq | guest dims | "
                     "nv097_formats | first..last flip |")
        lines.append("|---|---:|---:|---|---|---|")
        for addr in summary["candidate_addrs_overall"]:
            a = summary["per_addr"].get(addr, {})
            lines.append(
                f"| {addr} | {a.get('max_frame_draw_count', 0)} | "
                f"{a.get('max_last_color_draw_seq', 0)} | "
                f"{','.join(a.get('guest_dims_seen', []))} | "
                f"{','.join(str(f) for f in a.get('nv097_formats_seen', []))} | "
                f"{a.get('first_flip')}..{a.get('last_flip')} |")
    else:
        lines.append("_No candidates matched the heuristic. Re-run with "
                     "different --target-width/-height/-format._")
    lines.append("")

    # Per-flip walks: first, middle, last by default.
    sample_flips = []
    if flips:
        sample_flips.append(flips[0])
        if len(flips) > 2:
            sample_flips.append(flips[len(flips) // 2])
        if len(flips) > 1:
            sample_flips.append(flips[-1])
    sample_flips = list(dict.fromkeys(sample_flips))

    for flip in sample_flips:
        head = headers.get(flip, {})
        rows = bindings.get(flip, [])
        lines.append(f"## Flip {flip} (cache_size={head.get('cache_size')})\n")
        lp = head.get("last_publish", {})
        lines.append(
            f"- last_publish: kind=`{lp.get('kind')}` "
            f"reason=`{lp.get('reason')}` "
            f"source_vram_addr=`{lp.get('source_vram_addr')}` "
            f"source_texture=`{lp.get('source_texture')}` "
            f"published_texture=`{lp.get('published_texture')}`")
        lines.append(
            f"- current bindings: color_vram_addr=`"
            f"{head.get('current_color_binding_vram_addr')}` "
            f"depth_vram_addr=`"
            f"{head.get('current_depth_binding_vram_addr')}`")
        lines.append("")
        lines.append("| vram_addr | C/D | host | guest | nv097 | "
                     "frame_draw_count | last_color_seq | publish |")
        lines.append("|---|---|---|---|---:|---:|---:|---|")
        rows = sorted(rows, key=lambda b: (
            -int(b.get("last_color_draw_seq", 0)),
            -int(b.get("frame_draw_count", 0))))
        for b in rows[:args.max_per_flip_rows]:
            lines.append(fmt_row_md(b))
        lines.append("")

        cands = candidates_for_flip(rows, args.target_width,
                                    args.target_height,
                                    args.target_format,
                                    args.scale_tolerance)
        if cands:
            lines.append("### Candidates for this flip\n")
            for b in cands:
                lines.append(f"- `{b.get('vram_addr')}` "
                             f"guest={b.get('guest_width')}×{b.get('guest_height')} "
                             f"nv097={b.get('nv097_format')} "
                             f"frame_draw_count={b.get('frame_draw_count')} "
                             f"last_color_seq={b.get('last_color_draw_seq')}")
            lines.append("")

    lines.append("---")
    lines.append("_Generated by "
                 "`scripts/apple-silicon/surface-graph-analyze.py`._")
    out_path.write_text("\n".join(lines) + "\n")


def write_candidates_md(out_path, summary, args):
    lines = []
    lines.append("# Final-composite candidates (standalone)\n")
    lines.append(f"- target: {args.target_width}×{args.target_height} guest "
                 f"(±{args.scale_tolerance}× tolerance)"
                 + (f", nv097_format=={args.target_format}"
                    if args.target_format is not None else ""))
    lines.append(f"- flips analyzed: {summary['flip_count']}")
    lines.append("")
    if not summary["candidate_addrs_overall"]:
        lines.append("_No candidates._")
        out_path.write_text("\n".join(lines) + "\n")
        return
    for addr in summary["candidate_addrs_overall"]:
        a = summary["per_addr"].get(addr, {})
        lines.append(f"## {addr}")
        lines.append(f"- first/last flip: {a.get('first_flip')}..{a.get('last_flip')}")
        lines.append(f"- max frame_draw_count: {a.get('max_frame_draw_count', 0)}")
        lines.append(f"- max last_color_draw_seq: "
                     f"{a.get('max_last_color_draw_seq', 0)}")
        lines.append(f"- host dims seen: {a.get('host_dims_seen')}")
        lines.append(f"- guest dims seen: {a.get('guest_dims_seen')}")
        lines.append(f"- nv097_formats seen: {a.get('nv097_formats_seen')}")
        lines.append("")
    out_path.write_text("\n".join(lines) + "\n")


def main():
    args = parse_args()

    if not args.jsonl.exists():
        print(f"INFRA-FAIL: jsonl input not found: {args.jsonl}",
              file=sys.stderr)
        return 2

    args.out_dir.mkdir(parents=True, exist_ok=True)

    headers, bindings, flips = load_jsonl(args.jsonl)
    if not flips:
        print(f"INFRA-FAIL: no flip records in {args.jsonl}", file=sys.stderr)
        # Still emit empty artifacts so downstream M15-bundle-style gates
        # can distinguish "tool ran" from "tool not run".
        empty = {"flip_count": 0, "per_flip_candidate_count": {},
                 "per_addr": {}, "candidate_addrs_overall": []}
        (args.out_dir / "summary.json").write_text(
            json.dumps(empty, indent=2) + "\n")
        return 2

    summary = build_summary(headers, bindings, flips, args)

    summary_path = args.out_dir / "summary.json"
    summary_path.write_text(json.dumps(summary, indent=2,
                                       default=str) + "\n")

    csv_path = args.out_dir / "per-flip.csv"
    write_per_flip_csv(csv_path, headers, bindings, flips)

    report_path = args.out_dir / "report.md"
    write_report_md(report_path, headers, bindings, flips, summary, args)

    candidates_path = args.out_dir / "candidates.md"
    write_candidates_md(candidates_path, summary, args)

    print(f"surface-graph-analyze: flips={len(flips)} "
          f"candidate_addrs={len(summary['candidate_addrs_overall'])}")
    print(f"  summary.json:    {summary_path}")
    print(f"  report.md:       {report_path}")
    print(f"  candidates.md:   {candidates_path}")
    print(f"  per-flip.csv:    {csv_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
