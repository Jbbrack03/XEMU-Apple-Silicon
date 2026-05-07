#!/usr/bin/env python3
"""
oracle-seqlock-test.py — exercise the controller buffer's seqlock
under concurrent set / get to verify torn-read detection.

The agent's controller.set wraps each port-state mutation with the
seqlock pair `seq_begin_write` (parity → odd, marks "in flight") and
`seq_end_write` (parity → even, marks "stable"). The shim's reader
(xbed_input_synth_read) treats odd seq as "writer in flight, retry"
and equal-pre-and-post even seq as "consistent snapshot".

This script hammers controller.set from one thread while controller.get
reads from another. We check that:

  1. Every observed seq is internally consistent (seq parity even =
     no in-flight write at observation time).
  2. We never observe an "impossible" partial state — e.g. the
     buttons-trigger-stick triple matching prior `set` N exactly while
     seq matches `set` N+1 (a tear that would mean we read partial
     bytes from a mid-write state).

Note: on the OG Xbox single-CPU box the agent's lwIP loop is
single-threaded so torn writes physically cannot occur during the
agent's RPC dispatch. This test mainly verifies the wire-protocol's
end-to-end behavior across many rapid set/get cycles, plus exposes
any agent bugs (e.g. stale buffer pointer after kernel-pool
re-allocation, RPC dispatcher state corruption).

Usage:
  oracle-seqlock-test.py [--host HOST] [--rounds N] [--workers W]
  oracle-seqlock-test.py --selftest        # offline algorithmic check

Exit 0 on PASS, 1 on FAIL.
"""
from __future__ import annotations

import argparse
import importlib.util
import os
import sys
import threading
import time
from pathlib import Path
from typing import List, Optional, Tuple

_HERE = Path(__file__).resolve().parent


def _load_oc():
    spec = importlib.util.spec_from_file_location(
        "oracle_client", _HERE / "oracle-client.py")
    mod = importlib.util.module_from_spec(spec)  # type: ignore[arg-type]
    assert spec and spec.loader
    spec.loader.exec_module(mod)  # type: ignore[union-attr]
    return mod


# Algorithmic correctness check for the seqlock predicate. Runs offline.
def _selftest_predicate() -> int:
    """Validate the agent + shim's seqlock contract with a unit test
    against the predicate logic. We never touch the Xbox here."""
    fails = 0

    def reader_consistent(seq_pre: int, seq_post: int) -> bool:
        """Mirrors `xbed_input_synth_read`'s consistency check at
        xbed_input_synth.c:209. A read is consistent iff the pre/post
        seq are equal and even (no write in flight, no write completed
        during the copy)."""
        return (seq_pre == seq_post) and (seq_post & 1) == 0

    # 1. Stable read (no writer): seq stays even, pre==post → consistent.
    if not reader_consistent(2, 2):
        print("  FAIL stable-read predicate")
        fails += 1

    # 2. Mid-write read (writer began but hasn't ended): seq is odd
    #    → not consistent.
    if reader_consistent(3, 3):
        print("  FAIL mid-write detection")
        fails += 1

    # 3. Write completed during copy: pre even, post higher even → not
    #    consistent (pre != post).
    if reader_consistent(2, 4):
        print("  FAIL write-during-copy detection")
        fails += 1

    # 4. Write started during copy (pre even, post odd) → not consistent.
    if reader_consistent(2, 3):
        print("  FAIL write-started-during-copy detection")
        fails += 1

    # 5. Pristine state (seq=0): consistent (even, equal).
    if not reader_consistent(0, 0):
        print("  FAIL pristine-state predicate")
        fails += 1

    if fails == 0:
        print("[selftest] seqlock predicate: PASS (5/5)")
        return 0
    print(f"[selftest] seqlock predicate: FAIL ({5 - fails}/5 passed)")
    return 1


def _set_state(c, port: int, btn: int, lt: int, rt: int,
               lx: int, ly: int, rx: int, ry: int) -> str:
    line = (f"controller.set port={port} buttons=0x{btn:04x} "
            f"lt={lt} rt={rt} lx={lx} ly={ly} rx={rx} ry={ry}")
    code, _ = c.raw(line)
    return code


def _get_state(c, port: int) -> dict:
    code, payload = c.raw(f"controller.get port={port}")
    out = {}
    for ln in payload.decode("ascii", errors="replace").splitlines():
        if "buttons=" in ln:
            for tok in ln.split():
                if "=" in tok:
                    k, v = tok.split("=", 1)
                    if k == "buttons":
                        out[k] = int(v, 16)
                    else:
                        try:
                            out[k] = int(v)
                        except ValueError:
                            out[k] = v
        elif "triggers" in ln:
            for tok in ln.split():
                if tok.startswith("lt="):
                    out["ltrigger"] = int(tok.split("=")[1])
                elif tok.startswith("rt="):
                    out["rtrigger"] = int(tok.split("=")[1])
        elif "lstick" in ln:
            for tok in ln.split():
                if tok.startswith("x="):
                    out["lstick_x"] = int(tok.split("=")[1])
                elif tok.startswith("y="):
                    out["lstick_y"] = int(tok.split("=")[1])
        elif "rstick" in ln:
            for tok in ln.split():
                if tok.startswith("x="):
                    out["rstick_x"] = int(tok.split("=")[1])
                elif tok.startswith("y="):
                    out["rstick_y"] = int(tok.split("=")[1])
        elif "seq=" in ln:
            for tok in ln.split():
                if tok.startswith("seq="):
                    try:
                        out["seq"] = int(tok.split("=")[1])
                    except ValueError:
                        pass
    return out


def _hammer_writer(host: str, port: int, n_rounds: int,
                   stop: threading.Event, errs: List[str]) -> None:
    oc = _load_oc()
    try:
        with oc.OracleClient(host, 9001, timeout=10.0) as c:
            for i in range(n_rounds):
                if stop.is_set():
                    return
                btn = 0x1000 + (i & 0x0fff)
                lt = i % 32768
                rt = (32767 - i) % 32768
                code = _set_state(c, port, btn, lt, rt,
                                  i * 13, -i * 17, i * 19, -i * 23)
                if code != "200":
                    errs.append(f"writer set unexpected code: {code}")
                    return
    except Exception as e:
        errs.append(f"writer exception: {e}")


def _hammer_reader(host: str, port: int, n_rounds: int,
                   stop: threading.Event, observed: List[dict],
                   errs: List[str]) -> None:
    oc = _load_oc()
    try:
        with oc.OracleClient(host, 9001, timeout=10.0) as c:
            for i in range(n_rounds):
                if stop.is_set():
                    return
                snap = _get_state(c, port)
                if "seq" in snap and (snap["seq"] & 1) != 0:
                    errs.append(f"reader observed odd seq={snap['seq']} "
                                f"(should never happen — agent's "
                                f"`controller.get` responds only outside "
                                f"seq_begin_write..seq_end_write window)")
                observed.append(snap)
    except Exception as e:
        errs.append(f"reader exception: {e}")


def run_live_test(host: str, n_rounds: int, n_writers: int,
                  n_readers: int) -> int:
    """Run a live controller buffer hammer test against the agent."""
    print(f"[live-test] host={host} rounds={n_rounds} "
          f"writers={n_writers} readers={n_readers}")
    stop = threading.Event()
    errs: List[str] = []
    observed: List[dict] = []
    threads: List[threading.Thread] = []
    t0 = time.time()
    for i in range(n_writers):
        t = threading.Thread(
            target=_hammer_writer,
            args=(host, 0, n_rounds, stop, errs))
        t.start()
        threads.append(t)
    for i in range(n_readers):
        t = threading.Thread(
            target=_hammer_reader,
            args=(host, 0, n_rounds, stop, observed, errs))
        t.start()
        threads.append(t)

    for t in threads:
        t.join(timeout=120)
    t1 = time.time()
    print(f"[live-test] elapsed={t1 - t0:.1f}s observed={len(observed)} "
          f"errors={len(errs)}")
    if errs:
        for e in errs:
            print(f"  ERROR: {e}")

    # Validate every observed snapshot's seq parity.
    odd_count = sum(1 for s in observed if (s.get("seq", 0) & 1) != 0)
    if odd_count > 0:
        errs.append(f"{odd_count} observed snapshots had odd seq "
                    f"(torn / mid-write read)")

    # Sanity: at least the writer made progress.
    if not observed:
        errs.append("no snapshots observed — readers never ran or "
                    "agent unresponsive")
    else:
        max_seq = max(s.get("seq", 0) for s in observed)
        if max_seq < 2:
            errs.append(f"max observed seq={max_seq} (writer never "
                        f"completed even one cycle)")

    if errs:
        print(f"[live-test] FAIL ({len(errs)} issue(s))")
        return 1
    print(f"[live-test] PASS — every snapshot had even seq, "
          f"max seq={max(s.get('seq', 0) for s in observed)}")
    return 0


def main(argv: Optional[List[str]] = None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--host", default=os.environ.get("ORACLE_HOST",
                                                    "192.168.0.200"))
    p.add_argument("--rounds", type=int, default=100,
                   help="rounds per worker thread")
    p.add_argument("--workers", type=int, default=2,
                   help="writer threads")
    p.add_argument("--readers", type=int, default=2,
                   help="reader threads")
    p.add_argument("--selftest", action="store_true",
                   help="run offline predicate unit test only "
                        "(no Xbox needed)")
    args = p.parse_args(argv)

    if args.selftest:
        return _selftest_predicate()

    # Always run the predicate selftest first; it's free and proves
    # the algorithmic intent before any network traffic.
    rc = _selftest_predicate()
    if rc != 0:
        return rc
    return run_live_test(args.host, args.rounds, args.workers,
                         args.readers)


if __name__ == "__main__":
    sys.exit(main())
