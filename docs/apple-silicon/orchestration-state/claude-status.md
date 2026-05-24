# Claude Status

- Objective: cycle 42G real-Xbox deployment of the cycle-42F R5-GREEN witness-only XBE.
- Status: **PAUSED BEFORE EXECUTION.** Prior cycle 42F implementation/build/Codex slice is commit-verified CLOSED at 5fa39ae0aa. Cycle 42G posted its fresh-session receipt, then Josh intentionally stopped the worker before the physical-hardware deployment began so orchestration cleanup could happen first.
- Current hypothesis: the highest-value next evidence is the real-Xbox classification of the cycle-42F post-stamp readback discriminator. Cleanest expected success shape is (eeprom=0xBC, witness.scan-self count=0), which would rule out cycle-42E beta on the bypass-body + stamp-observability axis while leaving the residual discoverability question for the next slice.
- Required evidence to collect: pre/post witness.scan, pre/post witness.scan-self, eeprom.scratch.reset, final eeprom.scratch.read, full EEPROM dump cross-check, recovery timing notes, artifact directory summary.
- Constraints: no host-xemu source changes; no oracle-agent rebuild unless a genuinely new blocker forces scope change; preserve pre-existing tracked drift in the four apple-silicon scripts + two .inl files and preserve repo-root .hermes_* sprawl unstaged; prefer agent-side evidence over transcript tails.
- Next action: after orchestration cleanup is complete, relaunch a fresh bounded cycle-42G worker and execute the documented 18-step cycle-42E-derived deployment runbook with the cycle-42F interpretation delta.
