# Handoff Summary

- Cycle: 2026-05-21 evening, Hermes-supervised diagnosis slice (cycle 2).
- Owner: Claude Code.
- Outcome: Exit option B — durable diagnosis, no bounded fix landed. Tree state: 2 source files modified with env-gated diagnostics (zero impact when env unset), 4 doc files updated, new durable artifact directory under `docs/apple-silicon/task-16-evidence-2026-05-21/`. Codex validation completed with MAJOR ISSUES (3 findings) all adopted in-slice. Ready to commit.
- Key findings (durable, see `handoff.md` task #16 evening banner + `decision-log.md` entry):
  1. CPU-side per-vertex slot-9 stream is correct at collect time (`metal_attrib_stream slot=9 count=4 stride=44 src=0`).
  2. `pgraph_mtl_set_attr_masks` computes the correct `uniform_attrs=0xFDF6` for stride==44 draws (`metal_set_attr_masks` log line).
  3. Pipelines passing the `stride44` heuristic dump filter (attrs[3]+attrs[9] both populated) for back-buffer-class targets emit v0+v3+v9 all streaming. The filter is necessary but NOT sufficient to identify XBE-bind-state pipelines (per adopted Codex finding).
  4. A pipeline for the front buffer (`0x032a4000`) with v3 uniform / v9 streaming was observed during this slice but the dump was overwritten before staging; a replay capture is the first concrete step when work resumes.
  5. Visual symptom is a corner-tinted gradient over the full surface, NOT the prior-handoff "Q0 collapse"; bug mechanism is different than previously stated.
- Prior narrative superseded: the handoff's earlier "v9 = inlineValue[8], uniform_attrs=0xFFE0" description does NOT match the current source tree.
- New tooling shipped (env-gated): `XEMU_METAL_DIAG_ATTRIB_DUMP` extended with a third `metal_set_attr_masks` log stream; `XEMU_METAL_DUMP_TARGET_SHADER=stride44` heuristic noise-filter mode.
- Next slice's highest-value experiment: instrument `mtl_dispatch_decoded_draw` to log `draw_target_vram_addr` alongside the `metal_set_attr_masks` log for stride==44 draws, then correlate against the pipeline-target dumps to decide whether the XBE renders to back buffer (and front-buffer composition uses a stale pipeline) or front buffer directly. Also replay the front-buffer `0x032a4000` dump.
- Codex review state: PASS (all MAJOR findings adopted). Safe to commit.
