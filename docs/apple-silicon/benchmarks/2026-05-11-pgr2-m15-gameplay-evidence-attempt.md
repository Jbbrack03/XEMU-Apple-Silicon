# 2026-05-11 PGR2 M15 Gameplay Evidence Attempt

> Superseded as the current next-step diagnosis by
> `2026-05-11-pgr2-metal-render-path-diagnostic.md` and then by
> `2026-05-19-pgr2-snapshot-publish-and-rtt-followup.md`. This note still
> matters as the first strict gameplay-evidence failure, but its
> "capture-source divergence" framing is historical, not the current
> project conclusion.

Goal: produce the first strict M15 PGR2 gameplay visual artifact from fresh GL
and Metal sequences plus the existing real-Xbox oracle composite sequence.

## Runs

- Initial GL run:
  `benchmark-runs/20260511-181650-pgr2`
- Initial Metal NV2A run:
  `benchmark-runs/20260511-181909-pgr2`
- Shared attempt directory:
  `benchmark-runs/m15-gameplay-pgr2-20260511-181650`
- Strict xemu-window GL rerun:
  `benchmark-runs/20260511-182317-pgr2`
- Window-GL attempt directory:
  `benchmark-runs/m15-gameplay-pgr2-windowgl-20260511-182317`

## Result

No M15 gameplay evidence was produced.

The initial strict compare:

```text
benchmark-runs/m15-gameplay-pgr2-20260511-181650/evidence/summary.json
verdict=INFRA-FAIL
```

Root cause for that artifact: the GL leg used full-desktop macOS screenshots
because the launcher command omitted `XEMU_CAPTURE_WINDOW_PATTERN=xemu` and
`XEMU_CAPTURE_WINDOW_REQUIRED=1`.

The strict xemu-window rerun fixed the capture targeting:

```text
benchmark-runs/20260511-182317-pgr2/capture.log
source=window:662
```

`m15-gameplay-visual-compare.py` was updated to accept source-specific crops:
`--gl-crop`, `--metal-crop`, and `--oracle-crop`. This lets the GL xemu-window
capture be cropped to the viewport while Metal remains raw NV2A output.

The strict cropped compare still rejected alignment:

```text
benchmark-runs/m15-gameplay-pgr2-windowgl-20260511-182317/evidence-gl-crop/summary.json
verdict=INFRA-FAIL
```

A relaxed diagnostic compare was then generated only to inspect triptychs:

```text
benchmark-runs/m15-gameplay-pgr2-windowgl-20260511-182317/diagnostic-relaxed-align/summary.json
verdict=FAIL
changed_pct=85.4635..100.0000
contact_sheet=benchmark-runs/m15-gameplay-pgr2-windowgl-20260511-182317/diagnostic-relaxed-align/contact-sheet.jpg
```

## Interpretation

The diagnostic contact sheet shows GL and oracle frames progressing through
normal PGR2 title/profile/menu visuals with real background imagery. Metal
NV2A captures are visually divergent: the selected frames stay around earlier
title/profile states, and the profile-select background is flat gray/missing
the GL background detail.

This should be treated as an early PGR2 gameplay-evidence failure to debug, not
as gameplay parity. Do not count any of these artifacts toward M15. The later
diagnostic work ruled out capture-source error and the May 19 follow-up moved
the active blocker to late RTT/render-target-as-texture correctness.

## Follow-Up

1. Reproduce PGR2 with strict xemu-window GL capture and Metal NV2A capture.
2. Inspect whether `XEMU_METAL_SCREENSHOT_SOURCE=drawable` matches the visible
   Metal output better than `source=nv2a`, while remembering drawable captures
   can include xemu UI/HUD pixels.
3. If live drawable is correct but NV2A is wrong, debug the NV2A screenshot
   source/published texture path. If both are wrong, debug the Metal renderer
   texture/publish path for PGR2 profile/menu backgrounds.
