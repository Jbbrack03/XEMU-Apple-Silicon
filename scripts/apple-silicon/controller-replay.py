#!/usr/bin/env python3
"""
controller-replay.py — replay a XEMU_RECORD_INPUT CSV through the
real-Xbox oracle agent's controller.* RPCs.

The xemu input recorder (`XEMU_RECORD_INPUT=path.csv`) writes
`time_ms,control,value` rows; the project already maintains a library
of these CSVs under
`scripts/apple-silicon/input-scripts/` (pgr2-gameplay.csv,
sc2-gameplay.csv, ...). They drive xemu to gameplay states for
benchmarks. This tool feeds the SAME files to the real Xbox via the
oracle agent so the third oracle leg can validate both engines on
identical input.

Phase 1 (this version): the agent owns a synthetic-input STATE BUFFER
but does NOT yet inject that state into a running game's input read
path. The diag-XBE shim and the retail-game kernel hook (see
docs/apple-silicon/controller-injection-research.md) are the two
follow-on consumers that will close the loop.

Until those land, controller-replay.py still validates end-to-end
that:
  - Every CSV row parses cleanly.
  - Every event reaches the agent on time (jitter is logged).
  - The agent's buffer reflects the latest event.

USAGE
    controller-replay.py CSV [--host H] [--port P]
                             [--port-index N]   default 0
                             [--rate-multiplier M] default 1.0
                             [--start-at-ms N]  default 0
                             [--stop-at-ms N]   default end-of-file
                             [--dry-run]        parse + simulate without RPC
                             [--clear-on-start] zero state before replay

OUTPUT
    Per-event status line on stderr.
    JSON summary on stdout (event count, jitter histogram, RPC errors).

EXIT
    0 on completion (even with non-fatal RPC retries)
    1 on transport failure / unreachable agent
    2 on bad CSV
"""
from __future__ import annotations

import argparse
import csv
import json
import os
import statistics
import sys
import time
from pathlib import Path
from typing import Dict, List, Optional, Tuple

# Local import of oracle-client.py
_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE))
import importlib.util as _imp
_spec = _imp.spec_from_file_location("oracle_client", _HERE / "oracle-client.py")
oc = _imp.module_from_spec(_spec)  # type: ignore[arg-type]
assert _spec and _spec.loader
_spec.loader.exec_module(oc)  # type: ignore[union-attr]


# Same vocabulary as ui/xemu-input.c:101-127 + our agent's controller.h.
BUTTON_NAMES = {
    "a", "b", "x", "y",
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
    "back", "start", "white", "black",
    "lstick_btn", "rstick_btn", "guide",
}
AXIS_NAMES = {
    "ltrigger", "rtrigger",
    "lstick_x", "lstick_y", "rstick_x", "rstick_y",
}


def _parse_csv(path: Path) -> List[Tuple[int, str, int]]:
    """Read xemu's CSV format (`time_ms,control,value`). Returns a
    chronologically-sorted list of (time_ms, control, value) tuples.
    Comments (#) and blank lines are skipped, as are tab-separated
    rows that the recorder sometimes emits."""
    out: List[Tuple[int, str, int]] = []
    with path.open() as f:
        for line_no, raw in enumerate(f, start=1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            for sep in (",", "\t"):
                line = line.replace(sep, " ")
            parts = line.split()
            if len(parts) != 3:
                raise ValueError(
                    f"{path}:{line_no}: expected `time_ms,control,value`, "
                    f"got {raw!r}"
                )
            try:
                t = int(parts[0])
                v = int(parts[2])
            except ValueError as e:
                raise ValueError(
                    f"{path}:{line_no}: integer parse: {raw!r}"
                ) from e
            ctrl = parts[1].lower()
            if ctrl not in BUTTON_NAMES and ctrl not in AXIS_NAMES:
                raise ValueError(
                    f"{path}:{line_no}: unknown control '{ctrl}' "
                    f"(buttons={sorted(BUTTON_NAMES)} axes={sorted(AXIS_NAMES)})"
                )
            out.append((t, ctrl, v))
    out.sort(key=lambda r: r[0])
    return out


def _emit_event(client: oc.OracleClient, port: int, control: str,
                value: int) -> Tuple[bool, str]:
    """Translate one CSV row into a controller.button or controller.axis
    RPC. Returns (ok, message)."""
    try:
        if control in BUTTON_NAMES:
            v = 1 if value else 0
            code, body = client.raw(
                f"controller.button port={port} name={control} value={v}")
        else:
            code, body = client.raw(
                f"controller.axis port={port} name={control} value={value}")
        return (code == "200", body.decode("utf-8", "replace"))
    except oc.OracleRemoteError as e:
        return (False, f"500: {e}")
    except oc.OracleError as e:
        return (False, f"{type(e).__name__}: {e}")


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv")
    ap.add_argument("--host", default=os.environ.get("ORACLE_HOST", "192.168.0.200"))
    ap.add_argument("--port", type=int, default=int(os.environ.get("ORACLE_PORT", 9001)))
    ap.add_argument("--port-index", type=int, default=0,
                    help="Xbox controller port (0..3); default 0")
    ap.add_argument("--rate-multiplier", type=float, default=1.0,
                    help="Time-warp the replay (e.g. 2.0 = 2x speed); "
                         "default 1.0 = wall-clock-faithful")
    ap.add_argument("--start-at-ms", type=int, default=0)
    ap.add_argument("--stop-at-ms", type=int, default=None)
    ap.add_argument("--dry-run", action="store_true",
                    help="Parse + simulate; do not contact agent")
    ap.add_argument("--clear-on-start", action="store_true",
                    help="Send controller.clear before replay")
    args = ap.parse_args(argv)

    csv_path = Path(args.csv).resolve()
    if not csv_path.exists():
        print(f"csv not found: {csv_path}", file=sys.stderr)
        return 2
    try:
        events = _parse_csv(csv_path)
    except ValueError as e:
        print(f"parse error: {e}", file=sys.stderr)
        return 2

    if not events:
        print("[replay] no events to replay", file=sys.stderr)
        return 0

    if args.start_at_ms > 0:
        events = [e for e in events if e[0] >= args.start_at_ms]
    if args.stop_at_ms is not None:
        events = [e for e in events if e[0] <= args.stop_at_ms]
    if not events:
        print("[replay] all events filtered out by --start/--stop", file=sys.stderr)
        return 0

    base_t = events[0][0]
    rate = max(args.rate_multiplier, 0.001)

    print(f"[replay] csv      : {csv_path}", file=sys.stderr)
    print(f"[replay] events   : {len(events)} "
          f"(t={base_t}..{events[-1][0]} ms; rate={rate}x)", file=sys.stderr)
    print(f"[replay] target   : {args.host}:{args.port} port_index={args.port_index}",
          file=sys.stderr)
    if args.dry_run:
        print("[replay] DRY RUN — not contacting agent", file=sys.stderr)

    client: Optional[oc.OracleClient] = None
    if not args.dry_run:
        client = oc.OracleClient(args.host, args.port, timeout=10.0)
        try:
            client.connect()
        except oc.OracleError as e:
            print(f"[replay] cannot reach agent: {e}", file=sys.stderr)
            return 1
        if args.clear_on_start:
            try:
                client.raw(f"controller.clear port={args.port_index}")
            except oc.OracleError as e:
                print(f"[replay] clear failed (non-fatal): {e}", file=sys.stderr)

    started_wall = time.monotonic()
    sent = 0
    failed = 0
    # Two separate jitter measurements, both useful but for different
    # purposes (Codex 2026-05-07: don't conflate them):
    #   schedule_jitter_ms — wall-clock between target_wall and the
    #     moment we finished sleeping. Reflects Python+OS scheduling
    #     drift only; does NOT include RPC latency.
    #   delivery_jitter_ms — wall-clock between target_wall and the
    #     moment we got the agent's `200-` ack back. Includes RPC
    #     latency end-to-end. This is the metric an "is the buffer
    #     fresh by the time it gets read" downstream consumer cares
    #     about.
    schedule_jitters_ms: List[float] = []
    delivery_jitters_ms: List[float] = []
    errors: List[str] = []
    last_progress_log = 0
    # CSV trigger axis values are xemu int16 (0..32767, see
    # `ui/xemu-input.c:1117-1118` and CSV examples in
    # `scripts/apple-silicon/input-scripts/*.csv`). The agent's buffer
    # also stores triggers as int16 in the same range (post-2026-05-07
    # ABI fix), so the value passes through unchanged. Document the
    # invariant here so a future refactor doesn't accidentally
    # reintroduce a u8 representation only on the agent side.
    try:
        for idx, (t_ms, ctrl, val) in enumerate(events):
            target_wall = started_wall + (t_ms - base_t) / 1000.0 / rate
            now = time.monotonic()
            if target_wall > now:
                time.sleep(target_wall - now)
            schedule_actual = time.monotonic()
            schedule_jitters_ms.append((schedule_actual - target_wall) * 1000.0)
            if not args.dry_run and client is not None:
                ok, msg = _emit_event(client, args.port_index, ctrl, val)
                delivery_actual = time.monotonic()
                delivery_jitters_ms.append((delivery_actual - target_wall) * 1000.0)
                if ok:
                    sent += 1
                else:
                    failed += 1
                    errors.append(f"{t_ms}ms {ctrl}={val}: {msg}")
                    print(f"[replay] {t_ms}ms FAIL {ctrl}={val}: {msg}",
                          file=sys.stderr)
            else:
                sent += 1
            if idx - last_progress_log >= 500:
                last_progress_log = idx
                elapsed = time.monotonic() - started_wall
                avg_sched = statistics.mean(schedule_jitters_ms[-500:])
                avg_deliver = (statistics.mean(delivery_jitters_ms[-500:])
                               if delivery_jitters_ms else float("nan"))
                print(f"[replay] {idx + 1}/{len(events)} events "
                      f"({elapsed:.1f}s wall; "
                      f"schedule {avg_sched:.2f} ms / "
                      f"delivery {avg_deliver:.2f} ms)",
                      file=sys.stderr)
    finally:
        if client is not None:
            client.close()

    elapsed_total = time.monotonic() - started_wall

    def _jitter_summary(samples: List[float]) -> dict:
        if not samples:
            return {"count": 0, "mean": None, "median": None,
                    "max": None, "p95": None}
        return {
            "count": len(samples),
            "mean": round(statistics.mean(samples), 3),
            "median": round(statistics.median(samples), 3),
            "max": round(max(samples), 3),
            "p95": (round(sorted(samples)[int(len(samples) * 0.95)], 3)
                    if len(samples) >= 20 else None),
        }

    summary = {
        "csv": str(csv_path),
        "host": args.host,
        "port_index": args.port_index,
        "events_total": len(events),
        "events_sent": sent,
        "events_failed": failed,
        "wall_elapsed_s": round(elapsed_total, 3),
        "rate_multiplier": rate,
        # `schedule_jitter_ms` reflects Python+OS scheduling drift only
        # (reasonable < 5 ms on an idle Mac). `delivery_jitter_ms` adds
        # the agent RPC round-trip on top — that's the metric a reader
        # of the buffer downstream of the agent actually cares about.
        "schedule_jitter_ms": _jitter_summary(schedule_jitters_ms),
        "delivery_jitter_ms": _jitter_summary(delivery_jitters_ms),
        "errors_first_5": errors[:5],
        "dry_run": args.dry_run,
    }
    print(json.dumps(summary, indent=2))
    return 0 if failed == 0 else 0  # non-fatal RPC errors don't fail the run


if __name__ == "__main__":
    sys.exit(main())
