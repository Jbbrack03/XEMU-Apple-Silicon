# 2026-06-15 — Real-Xbox oracle access restored + diagnostic-XBE hardware-crash finding

## Access (resolved)

The real Xbox (192.168.0.200) was reachable all along — ping + FTP/21 up — but
the oracle agent on TCP 9001 does NOT auto-launch, so prior sessions concluded
the oracle was unavailable. Brought it up with
`python3 scripts/apple-silicon/oracle-orchestrator.py ensure-agent` (FTP
`SITE EXEC E:\Apps\oracle-agent\default.xbe` under UnleashX). Verified: `info`
→ `xbox-oracle-agent v0.4 (Phase 2 + controller.* + smc.*)`, live `screenshot`
captured. Captured durably in memory + `.claude/rules`/handoff updates so this
isn't lost again. The agent ↔ FTP/21 are mutually exclusive (agent up suspends
FTP); reboot to dashboard to FTP-inspect.

## Capture-path fix (D:\ → E:\Apps\<id>\) — shipped

`scripts/apple-silicon/xbe-tests/lib/xbed_capture.c`: the XBEs wrote their XOSS
capture + done-marker to `D:\<id>-capture.bin`, but under the UnleashX `SITE EXEC`
chainload `D:\` is the read-only launch mount, so `fopen(...,"wb")` failed and no
blob landed for the orchestrator (which collects from `/E/Apps/<id>/`). Fix:
`xbed_capture_and_reboot` now derives `E:\Apps\<id>\<id>-capture.bin` + `-done.txt`
from `xbe_id`, mounting E: (`nxMountDrive('E',"\\Device\\Harddisk0\\Partition1")`)
+ `CreateDirectoryA`, mirroring the proven idiom in `image-blit/main.c` and
`oracle-agent/controller.c`. Safe fallback to the passed D:\ paths if xbe_id is
absent / E: unavailable. Compiles clean (nxdk; stencil-ops rebuilt). Manifest
`artifacts.capture_blob.xbox_path` (D:\…) is now cosmetically stale (harness
ignores it; uses `ftp_path` = E:\…). Rebuild-all-XBEs deferred (see below).

## KEY FINDING: diagnostic XBEs crash on real hardware before capture

After the fix, `capture-reference --xbe stencil-ops` still returned
`no-xoss-blob-pulled`. FTP inspection of `/E/Apps/stencil-ops/` showed only the
deployed `default.xbe` — zero files written by the XBE. Crash-bisection markers
(temporary, since reverted) confirmed: **stencil-ops wrote no marker at all → it
crashes during its 300-frame stencil render, before reaching capture.**
`/E/Apps/image-blit/` corroborates: image-blit writes early markers
`00-program_entered` … `06-before_blits` then stops — **it dies at its
NV097_IMAGE_BLIT.** E:\ writes provably work (image-blit's markers landed), so
the blocker is not the path — **the homebrew diagnostic XBEs fault on real NV2A
hardware during render.** They pass on xemu and crash on silicon: direct evidence
of xemu↔hardware divergence (xemu tolerates states hardware rejects).

## Decision (autonomous, best-practice)

- Real-Xbox diagnostic-XBE goldens are BLOCKED by per-XBE hardware render crashes
  → tracked as task #12 (future xemu-divergence-mapping via gated staged markers;
  not critical path).
- The real-Xbox oracle's high-value WORKING use is **retail-title** validation
  (commercial games DO run on hardware; the 2026-06-02 Crimson run scored 82/91
  frames via the correctness oracle). That is M-IV, and it's where the
  deterministic oracle-vs-xemu comparison matters most (complex scenes a human/
  agent can't reliably judge). See `feedback_deterministic_visual_comparison`.
- For M-I geometry correctness, the math `expected.py` oracle is spec-authoritative
  for well-specified features (stencil ops → exact per-op colors from documented
  NV2A semantics; GL agrees). Proceeding with the `stencil-ops` Metal fix (task
  #10) against that spec oracle is legitimate — it is NOT scene-eyeballing.

## Next

Resume task #10: fix the Metal clear-rect defect (Metal ignores `SET_CLEAR_RECT`,
always clears the full surface; see root-cause analysis) that makes stencil-ops
FAIL, validate via the deterministic XBE harness against the spec oracle + full
board regression. Real-Xbox retail validation (M-IV) and the diag-XBE
hardware-crash investigation (#12) proceed on their own tracks.
