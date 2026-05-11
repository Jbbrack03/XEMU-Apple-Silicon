# Retail Oracle Workflow Validation - 2026-05-10 (Rainbow Six 3)

## Verdict

PASS. The real-Xbox retail oracle workflow completed end to end on
Rainbow Six 3:

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
benchmark-runs/retail-oracle-workflow-rainbow-20260510T214732Z
```

Key artifacts:

- `workflow.json`: `status=ok`
- `gameplay/verdict.json`: `verdict=ok`
- `gameplay/composite/capture-meta.json`: `frames_ok=102`
- `gameplay/composite/`: reference-frame sequence covering title launch,
  route replay, and dashboard return

Important fields from `gameplay/verdict.json`:

```json
{
  "capture_backend": "xemu-capture",
  "capture_rc": 0,
  "capture_timed_out": false,
  "dashboard_returned": true,
  "input_driver_rc": 0,
  "reference_frame_count": 102,
  "runxbe_ack": "dashboard FTP launch issued for F:\\Games\\Rainbow Six 3\\default.xbe",
  "verdict": "ok"
}
```

## Notes

- `route_offset_ms=0` was sufficient on the current retail Xbox image.
- The known-good Crimson bridge proof and IGR proof were reusable for this
  run; Rainbow did not require a title-specific input/backend workaround.
- This is the second retail title live-proven on the current hardware-backed
  workflow after Crimson Skies.
