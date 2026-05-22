# Validation Status

- Active slice: cycle 21 Path A.2 — re-route image-blit progress markers from `D:\` (cycle-19/20 proved blocked under `runxbe` chainload) to `E:\Apps\image-blit\…` (harness's existing FTP-collect target); re-run real-Xbox witness.
- Validation state: **CLOSED — marker re-route shipped, local validation green, real-Xbox witness returned a clear NEGATIVE answer, Codex validation passed (MINOR ISSUES adopted in full).**

## Gate status (cycle 21)

- [x] Fresh worker receipt posted before deeper work (18:11 CDT).
- [x] Bounded reviewable diff landed (~120 net lines, main.c only).
- [x] Rebuild successful (default.xbe + image-blit.iso; rebuilt via `eval "$(nxdk/bin/activate -s)" && make`).
- [x] Local xemu validation: 4 boots, 52 markers fire via `xemu-guest-log:` channel, all `e_mount=1`, **0 `fopen-failed` lines** (vs cycle-20's 52/52 fopen-failed for D:\ baseline); v0.4 tally `3/8, 3/8, 2/8, 2/8` byte-identical to cycle 20.
- [x] Real-Xbox run: 1 attempt; chainload→FTP-back 22.40 s (4th independent reproduction); FTP-collect returned 1 file (`default.xbe` upload echo), **0 `image-blit-marker-*` files**.
- [x] Conservative interpretation recorded in `handoff.md` + `decision-log.md` cycle-21 entries — cycle-19 hypothesis #1 demoted from "leading" to "insufficient as sole explanation" WITHOUT claiming any specific alternative is now leading.
- [x] Codex validation: mode `changes`, MINOR ISSUES, both findings (about doc-sync completeness at Codex-run-time) adopted in full in the same closure pass.
- [x] Validation marker written at `.claude/state/codex-validate-last-run` per rule #15.

## Witness outcome summary (cycle 21 Path A.2)

| Run | Run dir | Chainload→FTP-back | Files retrieved from `/E/Apps/image-blit/` |
|---|---|---:|---|
| local xemu Metal (E:\ path, NO dir-create) | `cycle21-image-blit-markers-local-metal-guestlog-20260522T231623Z/` | n/a | all 13 markers in host log; every fopen returned NULL (target dir does not exist on scratch HDD); fix landed before real-Xbox run |
| local xemu Metal (E:\ path, WITH `CreateDirectoryA` chain) | `cycle21-image-blit-markers-local-metal-guestlog-20260522T231823Z/` | n/a | 52/52 marker host-log lines, `e_mount=1` everywhere, **0 fopen-failed**; v0.4 tally drift byte-identical to cycle 20 |
| real Xbox (1 attempt per assignment) | `cycle21-real-xbox-image-blit-markers-20260522T232031Z/` | 22.40 s | `default.xbe` (upload echo, 159 744 B) only — **0 marker files** |

Real-Xbox attempt: `verdict.json status: ok` for the chainload-and-collect cycle, no `image-blit-marker-NN-*.txt` files present. Pixel oracle correctly classifies as `fail: no-xoss-blob-pulled` (capture/done writes still target `D:\`, unchanged in cycle 21).

## Yes/no answer to the cycle-21 question

**NO.** Re-routing markers from `D:\image-blit-marker-NN-STAGE.txt` to `E:\Apps\image-blit\image-blit-marker-NN-STAGE.txt` — a path the local xemu validation provably writes successfully (52→0 fopen-failed flip) and the harness provably FTP-retrieves any file from — does NOT make the markers observable on real Xbox under the current `runxbe`/oracle workflow. The failure mode is upstream of any in-XBE `fopen` call.

## What stands from cycle 17 + 19 + 20

- xemu `pass=8/8 mask=0xff` on Metal (4 boots) and GL (15 boots) under `XEMU_DIAG_PGRAPH_STATUS_DRAIN=1` — unchanged.
- §H.6 IMAGE_BLIT MET under the flag locally — unchanged.
- Flag ships opt-in, default OFF — unchanged.
- Cycle-20's observation that "D:\ write-back is blocked under `runxbe` chainload" is unchanged. Cycle 21 adds the further fact that **E:\ write-back is also blocked under the same chainload**.

## What changed (cycle 21)

- Marker write path: `D:\image-blit-marker-NN-STAGE.txt` → `E:\Apps\image-blit\image-blit-marker-NN-STAGE.txt`.
- Marker helper: gained an idempotent cached `image_blit_ensure_e_mount` shim (`nxIsDriveMounted('E')` → fallback `nxMountDrive('E', …)` → `CreateDirectoryA("E:\\Apps", NULL)` + `CreateDirectoryA("E:\\Apps\\image-blit", NULL)`); same pattern as four other XBEs in this fork.
- Buffer for the path string bumped 64 → 96 bytes.
- Host-log mirror format extended with `e_mount=N` field so xemu logs decompose mount success vs fopen success.
- Path-overflow safety preserved; host-log mirror preserved; failed-fopen surfacing preserved; FATX 42-char basename invariant preserved (only path prefix grew, basenames still ≤39 chars).
- Cycle-19 hypothesis #1 (D:\ remap mismatch under `runxbe`) — **DEMOTED from leading to insufficient as sole explanation**. The blocker is upstream of partition choice for this XBE on this chainload path.
- New cycle-21 hypothesis recorded: the FTP UPLOAD step makes `E:\Apps\image-blit\` writeable from FTP-server-time perspective; the chainloaded XBE may run in an environment where FATX-driver / NT-mount state differs from FTP-server-time state — `nxMountDrive` may report success but writes silently fail. Speculative.
