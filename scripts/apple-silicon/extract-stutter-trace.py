#!/usr/bin/env python3
"""Summarize XEMU_STUTTER_TRACE intervals from an xemu benchmark run.

Input can be a run directory or an xemu.log path. The script prints the
slowest xemu-perf intervals with their TCG_CHAIN_TOP and TCG_TB_TOP fields.
Those fields are emitted as:

  TCG_CHAIN_TOP=pc:chains:total_us:max_us:tb_count;...
  TCG_TB_TOP=pc:execs:guest_insns:guest_bytes;...
"""

from __future__ import annotations

import argparse
from pathlib import Path


def parse_kv(line: str) -> dict[str, str]:
    out: dict[str, str] = {}
    for part in line.strip().split():
        if "=" not in part:
            continue
        key, value = part.split("=", 1)
        if key.endswith(":"):
            key = key[:-1]
        out[key] = value
    return out


def parse_chain_top(value: str) -> list[tuple[str, int, int, int, int]]:
    rows = []
    for entry in value.split(";"):
        if not entry:
            continue
        parts = entry.split(":")
        if len(parts) != 5:
            continue
        pc, chains, total_us, max_us, tb_count = parts
        rows.append((pc, int(chains), int(total_us), int(max_us), int(tb_count)))
    return rows


def parse_tb_top(value: str) -> list[tuple[str, int, int, int]]:
    rows = []
    for entry in value.split(";"):
        if not entry:
            continue
        parts = entry.split(":")
        if len(parts) != 4:
            continue
        pc, execs, guest_insns, guest_bytes = parts
        rows.append((pc, int(execs), int(guest_insns), int(guest_bytes)))
    return rows


def resolve_log(path: Path) -> Path:
    if path.is_dir():
        return path / "xemu.log"
    return path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("path", type=Path, help="benchmark run dir or xemu.log")
    parser.add_argument("--threshold-us", type=int, default=50000)
    parser.add_argument("--top", type=int, default=12)
    args = parser.parse_args()

    log = resolve_log(args.path)
    if not log.exists():
        parser.error(f"log not found: {log}")

    intervals = []
    stutters_by_interval: dict[str, list[dict[str, str]]] = {}

    with log.open("r", errors="replace") as fh:
        for line in fh:
            if line.startswith("xemu-stutter:"):
                kv = parse_kv(line)
                stutters_by_interval.setdefault(kv.get("interval_id", "?"), []).append(kv)
                continue
            if not line.startswith("xemu-perf:") or "interval_ms=" not in line:
                continue
            kv = parse_kv(line)
            if kv.get("final") == "1":
                continue
            try:
                mspf_max_us = int(round(float(kv.get("mspf_max", "0")) * 1000))
            except ValueError:
                continue
            if mspf_max_us >= args.threshold_us or "TCG_CHAIN_TOP" in kv:
                intervals.append((mspf_max_us, kv))

    intervals.sort(key=lambda item: item[0], reverse=True)
    intervals = intervals[: args.top]

    print(f"log={log}")
    print(f"threshold_us={args.threshold_us}")
    print(f"reported_intervals={len(intervals)}")

    for rank, (mspf_max_us, kv) in enumerate(intervals, start=1):
        interval_id = kv.get("interval_id", "?")
        print()
        print(
            f"#{rank} interval_id={interval_id} "
            f"mspf_max_us={mspf_max_us} interval_ms={kv.get('interval_ms', '?')} "
            f"frames={kv.get('frames', '?')} fps={kv.get('fps', '?')} "
            f"start_us={kv.get('interval_start_us', '?')} "
            f"end_us={kv.get('interval_end_us', '?')}"
        )
        if interval_id in stutters_by_interval:
            for st in stutters_by_interval[interval_id]:
                print(
                    "  stutter "
                    f"frame_index={st.get('frame_index', '?')} "
                    f"duration_us={st.get('duration_us', '?')} "
                    f"now_us={st.get('now_us', '?')}"
                )

        chain_top = parse_chain_top(kv.get("TCG_CHAIN_TOP", ""))
        if chain_top:
            print("  TCG_CHAIN_TOP pc chains total_us max_us tb_count")
            for pc, chains, total_us, max_us, tb_count in chain_top:
                print(f"    {pc} {chains} {total_us} {max_us} {tb_count}")

        tb_top = parse_tb_top(kv.get("TCG_TB_TOP", ""))
        if tb_top:
            print("  TCG_TB_TOP pc execs guest_insns guest_bytes")
            for pc, execs, guest_insns, guest_bytes in tb_top:
                print(f"    {pc} {execs} {guest_insns} {guest_bytes}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
