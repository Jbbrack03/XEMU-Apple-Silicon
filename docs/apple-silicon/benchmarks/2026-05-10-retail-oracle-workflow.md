# Retail Oracle Workflow Validation - 2026-05-10

## Verdict

PASS. The real-Xbox retail oracle workflow completed end to end on
Crimson Skies:

1. dashboard FTP launched the retail XBE;
2. OGX360 hardware replay drove the recorded route;
3. approved macOS `xemu-capture.app` captured composite reference frames;
4. controller IGR returned to UnleashX;
5. dashboard FTP was reachable after the run.

Final console state after validation:

```json
{"ping": true, "ftp": true, "agent": false}
```

## Decisive Run

Run directory:

```text
benchmark-runs/retail-oracle-workflow-crimson-routeoffset-20260510T183546Z
```

Key artifacts:

- `workflow.json`: `status=ok`
- `gameplay/verdict.json`: `verdict=ok`
- `gameplay/composite/capture-meta.json`: `frames_ok=91`
- `gameplay/composite/contact-sheet-all-frames.png`: visual proof of
  dashboard -> Crimson boot/loading -> title/menu -> cutscene/game
  scene/plane frames -> UnleashX/dashboard return

Important fields from `gameplay/verdict.json`:

```json
{
  "capture_backend": "xemu-capture",
  "capture_rc": 0,
  "capture_timed_out": false,
  "dashboard_returned": true,
  "input_driver_rc": 0,
  "reference_frame_count": 91,
  "runxbe_ack": "dashboard FTP launch issued for F:\\Games\\Crimson Skies\\default.xbe",
  "verdict": "ok"
}
```

## Fixes Required During Validation

- Raw `.build/release/xemu-capture` and direct
  `Contents/MacOS/xemu-capture` execution were removed from the
  workflow path. Camera operations now go through
  `scripts/apple-silicon/xemu-capture-app.py`, so macOS TCC sees the
  approved LaunchServices app identity.
- Long reference capture now uses
  `scripts/apple-silicon/capture-frame-sequence.py` by default, writing
  PNG frames and `capture-meta.json`.
- Retail games launch from dashboard FTP `SITE EXEC`, not
  `oracle-agent runxbe`. The agent path acknowledged the command but
  did not reliably transition into the retail title.
- Post-dashboard screenshots are opt-in only. Capturing one relaunches
  the oracle agent and suspends dashboard FTP, which breaks the handoff
  to a subsequent dashboard-launched retail game.
- Crimson Skies needs a real-hardware route timing offset:
  `route_offset_ms=28000`. Without it, the route presses `Start` before
  the retail console reaches the title screen, producing a mechanically
  green but title-screen-only run.

## Current Command

```sh
python3 scripts/apple-silicon/retail-oracle-workflow.py --title crimson
```

For title retuning:

```sh
python3 scripts/apple-silicon/retail-oracle-workflow.py \
  --title crimson \
  --route-offset-ms 28000
```

The workflow can reuse known-good bridge/exit evidence with
`--bridge-evidence` and `--exit-evidence`, but normal production runs
should let it validate those gates itself.
