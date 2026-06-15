# Cycle 45F successor packet — witness-only post-JSON helper packaging

Status: READY
Cycle: 45F
Type: bounded tooling follow-up
Default worker lane: Qwen 35B in a disposable worktree
Independent review: Codex required if the helper changes non-trivial script logic

## Objective

Package the now-proven cycle 45E real-Xbox witness-only post-JSON validation flow into one reusable bounded path so future hardware slices do not have to reconstruct the same supervisor-owned command sequence by hand.

Primary deliverable:
- a small helper or runbook that performs the exact 45E-proven sequence with one invocation

Recommended implementation target:
- `scripts/apple-silicon/oracle-witness-postjson.sh`

Recommended paired doc update:
- `docs/apple-silicon/oracle-workflow.md`
  or
- `docs/apple-silicon/automation.md`

Keep the slice bounded. Prefer one helper script plus one doc touch over a broader refactor.

## Why this slice exists

Cycle 45E already proved the repaired path works on real hardware after Hermes refreshed the deployed Xbox-side oracle-agent:
- live `help --json` succeeded
- `eeprom.scratch.reset` baseline discipline mattered
- `run-diag --post-json-command ...` successfully collected machine-readable artifacts
- the successful rerun preserved the prior hardware conclusion:
  - EEPROM byte `0xBC`
  - `witness.scan-self count = 0`
  - `witness.scan count = 1`

The remaining gap is operational: the recipe is still spread across docs, logs, and supervisor memory. 45F turns it into a reusable launch path.

## Ground truth to preserve

Use the repaired 45E rerun as the canonical source recipe:
- output directory:
  - `benchmark-runs/cycle45e-realxbox-postjson-rerun-20260526T205253Z/`
- required post-JSON artifacts:
  - `post-json/eeprom.scratch.read.json`
  - `post-json/witness.scan-self.json`
  - `post-json/witness.scan.json`
- verdict artifact:
  - `verdict.json`

The successful 45E rerun used:
- XBE path:
  - `E:\Apps\witness-only\default.xbe`
- FTP collect path:
  - `/E/Apps/witness-only`
- post-JSON commands:
  - `eeprom.scratch.read`
  - `witness.scan-self`
  - `witness.scan`

## Exact recipe to encode

The helper or runbook should encode this supervisor-owned sequence in order:

1. Optional reachability / smoke preflight
   - `python3 scripts/apple-silicon/oracle-orchestrator.py status`
   - or `scripts/apple-silicon/oracle-smoke.sh` when a broader pipeline check is desired

2. Verify live JSON support before trusting post-JSON readbacks
   - `python3 scripts/apple-silicon/oracle-client.py raw 'help --json'`
   - require the response to advertise at least:
     - `eeprom.scratch.read`
     - `eeprom.scratch.reset`
     - `witness.scan`
     - `witness.scan-self`

3. Arm unsafe writes and clear the EEPROM scratch baseline
   - `python3 scripts/apple-silicon/oracle-client.py raw 'unsafe.enable'`
   - `python3 scripts/apple-silicon/oracle-client.py raw 'eeprom.scratch.reset --json'`

4. Run the bounded witness-only validation with machine-readable post-readbacks
   - `python3 scripts/apple-silicon/oracle-orchestrator.py run-diag        --xbe 'E:\Apps\witness-only\default.xbe'        --ftp-collect /E/Apps/witness-only        --out <benchmark-runs target>        --post-json-command eeprom.scratch.read        --post-json-command witness.scan-self        --post-json-command witness.scan`

5. Validate the resulting structured artifacts
   - `verdict.json` must end with `"status": "ok"`
   - `post-json/eeprom.scratch.read.json` should show byte `188` / `0xBC` for the proven 45E witness-only recipe
   - `post-json/witness.scan-self.json` should show `count = 0`
   - `post-json/witness.scan.json` should show `count = 1`

## Failure handling that must be encoded

If `help --json` does not advertise the required commands:
- treat it as deployed oracle-agent drift, not hardware truth
- refresh / redeploy the Xbox-side oracle-agent before trusting any `run-diag --post-json-command ...` result

If `run-diag` returns `post-json-readback-failed`:
- treat it as transport/protocol or deployment drift first
- do not reclassify the hardware conclusion until the JSON-capable path is repaired and rerun

If the helper performs the baseline reset itself, make that behavior explicit in the usage text so future operators understand why the pre-run EEPROM value may differ from the post-run value.

## Bounded file set

Preferred file set for 45F:
- `scripts/apple-silicon/oracle-witness-postjson.sh`  (new helper, preferred)
- one canonical doc:
  - `docs/apple-silicon/oracle-workflow.md`
  - or `docs/apple-silicon/automation.md`
- optional state touch only if the helper path materially changes the control-plane truth

Avoid widening into unrelated oracle-agent, witness-only XBE, or nxdk changes during 45F.

## Completion criteria

45F is complete when all of the following are true:
- a single documented invocation exists for the repaired witness-only post-JSON recipe
- the helper or runbook includes the JSON-support preflight and EEPROM reset step
- the expected output directory and post-JSON artifact set are documented exactly
- a bounded dry-run or static review confirms the command shape matches the successful 45E rerun
- if the helper script changes non-trivial shell logic, Codex review is completed before closure

## Non-goals

45F is not a new hardware conclusion slice.
45F does not reopen the witness-only hypothesis.
45F does not widen into broader oracle-agent feature work unless the packaging step immediately proves the recipe still cannot be expressed with existing commands.
