# Real Xbox Oracle — Feasibility Verdict

Last updated: 2026-05-06 (evening).
Status: **Phase 1 SHIPPED.** Hardware retrieved and online. XBDM
architecture below superseded by a custom oracle agent — see
"Update 2026-05-06 evening" immediately below. The rest of this
document remains valid as the feasibility analysis that motivated
the work, and as the roadmap for PrometheOS / OpenXenium bank
control which is still queued (post-Phase-2 of the agent).

## Update 2026-05-06 evening — XBDM architecture superseded

Hardware retrieved and connected to LAN at `192.168.0.200`. The
XBDM-on-debug-kernel architecture proposed below did not work on
this Xbox's iND-BiOS revision: edits to `ind-bios.cfg DISABLEDM=0`
+ `x2config.ini startDebug=1` plus uploaded `xbdm.dll` (Latest /
4242 / 4039 from SDK 4361) all failed to open port 731. Likely
cause: this iND-BiOS revision predates BFM 5004.67 which is what
added the documented `DISABLEDM`-driven debug-monitor loading.
Without TV access we cannot read the boot banner to confirm the
exact iND-BiOS version.

We pivoted to a custom nxdk-built oracle agent. It listens on TCP
port 9001 with our own text-line protocol (XBDM-inspired status
codes — `200- single-line`, `201- OK\\n...lines...\\n.\\n` multi,
`500- error`). Phase 1 commands are `info`, `eeprom`, `reboot`,
`bye`, validated end-to-end on 2026-05-06 (port 9001 listens within
~5 s of `SITE RunXBE`; agent EEPROM hex matches the file-based dump
byte-for-byte; `reboot` returns to XBMC4Gamers in ~30 s).

Source lives in-tree at
`scripts/apple-silicon/xbe-tests/oracle-agent/` with full
deploy/usage docs in that directory's `README.md`. Phase 2+ commands
(`mem.read`, `mem.write`, `nv2a.read`, `nv2a.write`, `screenshot`,
`vram.read`, `runxbe`) are queued for the next session per
`handoff.md`.

**Why the pivot is acceptable.** This project has no Visual
Studio Xbox debugger, no Xbox Neighborhood, no other tool that
needs XBDM-protocol compatibility. We need the *capability surface*
(memory R/W, register access, framebuffer capture, XBE launch).
Custom agent provides the same surface, no Microsoft IP, no
dependency on a debug-build BIOS, full source in our control.

**Reference materials retained, not deployed.** SDK 4361 was
extracted to `/Users/jbbrack03/XEMU_MacOS/xbox-oracle-backup/2026-05-06/reference-sdk/`
(outside this repo, gitignored). It contains `XbDm.h` (authoritative
protocol header), `xbdm.dll`, `xbdm.pdb` (debug symbols), and the
Windows-side `xboxdbg.dll` client lib. Used for designing Phase 2+
commands — none of these binaries are committed or deployed on the
Xbox.

**Sections of this doc that remain authoritative.** Hardware
inventory; the Mac-feasibility matrix; the network-only workflow
analysis; the diagnostic-XBE-on-retail-kernel discussion; the
PrometheOS / OpenXenium bank-switching plan (still useful future
work for unattended power-on, just not on the Phase 2 critical
path). Discard the XBDM-specific assumptions in §TL;DR and any
"port 731" references — they are now historical context for why
we built our own.

---

## Purpose

This document captures the feasibility research for using a real
OpenXenium-modded Original Xbox as a hardware oracle for validating
xemu's Metal renderer output. It supersedes the all-emulator-side
validation strategy that was Codex-flagged BLOCKING in
`diagnostic-xbe-plan.md` (commit d57742ef47, see decision-log
2026-05-06 entries).

The feasibility research was driven by user input on 2026-05-06: the
user has an OpenXenium-modded retail Xbox in storage with XBMC4Xbox +
2 TB HDD, but only Apple Silicon Macs as workstations and no PC. The
question driving the research: can the entire Mac-and-network-only
workflow actually do what we need, or are there hard blockers
requiring PC tooling, capture cards, or weeks of original kernel
research.

## TL;DR

**Feasible. ~1-2 weeks of focused engineering. Mac-and-network-only
with two physical caveats** (one-time Xbox retrieval; optional
hardware mod for unattended power-on). Major architectural pieces
already exist; we are integrating, not inventing.

The architecture leverages **Microsoft's own XBDM (Xbox Debug
Monitor) service** running on a debug kernel flashed to one
OpenXenium bank, plus **PrometheOS** as the OpenXenium chip OS for
clean REST-API control of bank switching. Diagnostic XBEs we build
ourselves (running on a retail/Cerbios kernel in another bank) cover
the NV2A-feature-surface oracle case; XBDM's `screenshot` command
covers the retail-game oracle case.

## Hardware and software inventory

User has:

- Original Xbox modded with OpenXenium chip.
- XBMC4Xbox dashboard installed on the internal HDD.
- 2 TB internal HDD with retail games installed (PGR2, Halo CE, SC2,
  Crimson Skies, Rainbow Six 3 confirmed).
- Composite / component video output only (no HDMI mod).
- No HDMI capture card available.
- Apple Silicon Macs only — no PC, no x86 Linux machine.
- Xbox is currently in storage; retrieval pending decision.

## Architecture

```
┌─ Apple Silicon Mac ─────────────────────┐
│  Python orchestrator                    │
│   ├─ FTP client (lftp / ftplib)         │
│   ├─ HTTP → PrometheOS REST API         │
│   ├─ HTTP → XBMC4Xbox API               │
│   └─ TCP → XBDM port 731                │
└─────────────┬───────────────────────────┘
              │ LAN (100 Mbit)
┌─────────────┴───────────────────────────┐
│  Original Xbox + OpenXenium             │
│   ├─ OpenXenium chip OS: PrometheOS     │
│   │   (REST: bank-switch, flash-write,  │
│   │    screenshot, reboot, EEPROM)      │
│   ├─ BIOS bank 1: XeniumOS (recovery)   │
│   ├─ BIOS bank 2: Cerbios (retail use)  │
│   ├─ BIOS bank 3: Debug (XBDM service)  │
│   ├─ Dashboard (HDD): XBMC4Xbox         │
│   │   (HTTP API: RunXBE, autoexec.py,   │
│   │    TakeScreenshot, SendKey)         │
│   └─ HDD: 2 TB retail games + diagnostic
│           XBE library                    │
└─────────────────────────────────────────┘
```

**Two oracle paths covered by the same hardware:**

1. **NV2A-feature-surface oracle** (diagnostic XBEs we build).
   Boot Cerbios bank → XBMC4Xbox launches our XBE → XBE renders
   test pattern → XBE itself sends frames to the Mac via
   `libnxdk_net` (TCP). Self-instrumented because the XBEs are our
   code. ~95 % of the diagnostic-XBE library uses this path.

2. **Retail-game visual oracle** (closed retail-game binaries).
   Boot debug bank → debug kernel includes XBDM service → launch
   retail game via `RetailGameLoader` → Mac talks to XBDM on TCP/731,
   issues `screenshot` command, receives framebuffer regardless of
   which XBE is running. The retail game is unmodified.

Cross-cutting: the Mac-side Python orchestrator drives both paths.
Bank switching is automated via PrometheOS REST API.

## Verified Mac-feasibility per workflow step

| Step | Tool | Apple Silicon arm64 status | Source |
|---|---|---|---|
| Build diagnostic XBEs | nxdk | **Confirmed native** | [PR #667 (May 2024)](https://github.com/XboxDev/nxdk/pull/667); active development, recent `nvnetdrv` work April 2026 |
| Edit Cerbios BIOS configs | CerbiosTool | osx-x64 binary, runs via Rosetta 2 | [Team-Resurgent/CerbiosTool](https://github.com/Team-Resurgent/CerbiosTool) |
| Validate XBE headers | pyxbe | **Native arm64** (`pip install pyxbe`) | [pyxbe on PyPI](https://pypi.org/project/pyxbe/) |
| FTP file transfers | lftp / curl / Python `ftplib` / Cyberduck / Transmit | **Native arm64** | macOS standard |
| HTTP automation | Python `requests` | **Native arm64** | macOS standard |
| XBDM client | xboxpy / ViridiX / xbdm_gdb_bridge / Experiment5X | Cross-platform | [XboxDev/xboxpy](https://github.com/XboxDev/xboxpy), [ViridiX](https://github.com/XboxDev/ViridiX), [abaire/xbdm_gdb_bridge](https://github.com/abaire/xbdm_gdb_bridge), [Experiment5X/XBDM](https://github.com/Experiment5X/XBDM) |
| In-system BIOS flash | Xenium-Tools XBE | Pre-built XBE, no host build needed | [Ryzee119/Xenium-Tools v2.0](https://github.com/Ryzee119/Xenium-Tools/releases) |
| OpenXenium chip-OS automation | PrometheOS | Pure HTTP/FTP from any host | [Team-Resurgent/PrometheOS-Firmware](https://github.com/Team-Resurgent/PrometheOS-Firmware) |
| XBMC4Xbox HTTP automation | curl / Python `requests` | Pure HTTP | [HTTP API doc (Web Archive)](https://web.archive.org/web/20240525074335/https://www.xbmc4xbox.org.uk/wiki/Web_Server_HTTP_API) |
| Screenshot (XBMC dashboard) | XBMC4Xbox `TakeScreenshot()` HTTP | Pure HTTP | XBMC4Xbox API |
| Screenshot (PrometheOS) | `GET /api/screenshot` | Pure HTTP, returns PNG | PrometheOS source |
| Screenshot (debug-kernel mid-game) | XBDM `screenshot` command on TCP/731 | Cross-platform clients above | [xboxdevwiki XBDM commands by version](https://xboxdevwiki.net/XBDM_commands_by_version) — `screenshot` confirmed in OG Xbox XDK build 3521+ |
| Controller input injection | XBDM `autoinput` / `queuepackets` | Cross-platform clients | **UNCERTAIN on OG Xbox** — see §"Unknowns to verify" |
| Extend XBDM with custom commands | nxdk_dyndxt | nxdk-built code, Mac-native build | [abaire/nxdk_dyndxt](https://github.com/abaire/nxdk_dyndxt) |
| EEPROM editing | xemu's built-in dialog | **Native arm64** (already shipping) | This fork's `dist/xemu.app` |
| ISO packaging | xdvdfs-cli (already in stack) | **Native arm64** | Already integrated via `package-game.sh` |

The "Windows PC required" framing is obsolete in 2024-2026 for
OpenXenium workflows. The only legacy Windows-only tool is
**FATXplorer** for offline HDD-image surgery, which is not needed
when the HDD is in the Xbox and reachable via FTP.

## Mac-feasibility ceiling — what can be done remotely

**Fully Mac+network (no physical interaction):**

- Build / edit / validate BIOS images (CerbiosTool under Rosetta;
  pyxbe natively).
- FTP arbitrary files (XBE, BIOS bin, ISO content, configs) to/from
  the Xbox.
- Trigger any installed XBE via XBMC4Xbox `ExecBuiltIn(XBMC.RunXBE(F:\...))`.
- Reboot / shutdown / eject via either dashboard's HTTP API.
- Take screenshots of the dashboard or running XBE (XBMC `TakeScreenshot`,
  PrometheOS `/api/screenshot`).
- Take screenshots **of running retail games** via XBDM `screenshot`
  (debug-kernel boot path).
- Switch active BIOS bank via PrometheOS `/api/launchbank?<id>`.
- Dump existing BIOS flash to HDD then FTP to Mac (Xenium-Tools).
- Write a new BIOS to a flash bank via PrometheOS `/api/upload`.
- Send simulated controller input to navigate dashboards (XBMC
  `SendKey`).
- Edit Cerbios .ini on a Cerbios-flashed bank (PrometheOS
  `POST /api/cerbiosini`).
- Pre-stage "next-boot launches XBE X" via FTP to `Q:\Scripts\autoexec.py`
  + `Restart()`.
- Build and run XBE diagnostic library locally (nxdk).

**Requires physical interaction with the Xbox:**

- **One-time setup**: Xbox retrieval from storage; ethernet + power
  cable connection; one initial XBMC4Xbox boot to confirm IP and
  enable HTTP API.
- **Powering on the Xbox.** Original Xbox has **no Wake-on-LAN**.
  Smart-plug solutions don't work either — restoring power doesn't
  trigger the SMC start sequence
  ([AVForums thread](https://www.avforums.com/threads/remote-control-of-xbox-power-button.75489/)).
  Without a hardware mod, the user must physically press the power
  button each session. Mitigations:
  - Commercial **XERC 2 XE** IR-receiver mod (~$25-50,
    [eBay listing](https://www.ebay.com/itm/254024289332)) lets a
    Mac-driven IR LED fire 56 kHz pulses to power-on / eject.
  - DIY **ESP32 + 56 kHz IR LED** (~$10 parts; one-time soldering)
    driven over Wi-Fi from the Mac via MQTT or HTTP.
  - DIY **relay across the front-panel power button** (similar
    parts cost; soldering).
- **Recovery-pin bridge** if all OpenXenium banks ever get bricked.
  Requires opening the Xbox case, jumpering two pads on the modchip
  PCB at power-on. Mitigated by mapping the **eject** button to a
  known-good XeniumOS bank as a default fallback — eject takes you
  to recovery without case-opening if at least one bank is intact.
  Worst-case (rare) requires JTAG re-flash of the CPLD via a
  ~$20 FT2232H clone connected to the Mac (achievable on Apple
  Silicon via OpenOCD or `flashrom`).
- **128 MB RAM upgrade** if 64 MB Xbox + debug kernel + heavy retail
  game (PGR2 streams a lot of assets) hits OOM. ~$30 mod
  ([ModzvilleUSA kit](https://modzvilleusa.com/products/xbox-128-ram-upgrade-kit-for-1-0-1-5-ogxbox-4x-sticks-of-ram),
  [quade.co procedure](https://quade.co/2018/xbox-128mb-ram-upgrade/)).
  Soldering required. **May not be needed** depending on which
  retail-game frames we actually want to capture; test per title.

## OpenXenium specifics

- **Modchip total flash:** 2 MB (1 MB user-area + reserved sectors).
  Configurable as 4×256 KB, 1×512 + 2×256, 2×512, or 1×1 MB user
  banks. Up to **8 virtual banks** mappable inside XeniumOS.
- **Bank-switching:** software-driven via CPLD register I/O (no
  physical switch). XeniumOS lets you map "Power button → Bank N"
  and "Eject button → XeniumOS" — the eject path is the reliable
  software fallback into recovery.
- **Recovery sector:** fixed flash region (bank 10 = 0b1010).
  Activated by **bridging two recovery pins on power-up** —
  physical jumper, requires opening the case. Recovery sector is
  only present if you flashed from a real Xenium dump (`flash.bin`
  from Xenium-Tools' START+X menu); flashing from XeniumOS update
  files alone does NOT populate the recovery sector. **Action:
  before any experimental flashing, dump current flash via
  Xenium-Tools to preserve the recovery sector image.**
- **Flash-cycle endurance:** Macronix MX29LV160-class flash, rated
  ~100,000 P/E cycles per sector
  ([datasheet](https://www.macronix.com/Lists/Datasheet/Attachments/8520/MX29LV160D%20T-B,%203V,%2016Mb,%20v1.2.pdf)).
  Not a concern at any reasonable test cadence.
- **Failure mode for a bricked active bank:** black screen on
  power-on; press eject → boots XeniumOS from a different bank;
  recovery is one button press if XeniumOS is intact in another
  bank.

## Why XBDM is the right oracle

The breakthrough finding (research stream 3, agent
`ac2b46e61736d3fbf` 2026-05-06) is that **Microsoft's own XBDM
(Xbox Debug Monitor) service** ships with debug kernels and exposes:

- `screenshot` command — TCP/731, dumps the live framebuffer over
  the wire as raw pixel bytes. Confirmed available in OG Xbox XDK
  build 3521 and later.
  ([Experiment5X/XBDM Xbdm.cpp](https://github.com/Experiment5X/XBDM/blob/master/Xbdm.cpp)
  shows a working client implementation.)
- `autoinput user=N bind|unbind` and `autoinput user=N queuepackets count=K
  timearray countarray` — queue gamepad state into the kernel HID layer,
  replayed as if the controller had sent it. Confirmed on Xbox 360 dev
  kits; **UNCERTAIN on OG Xbox** — the
  [version table](https://xboxdevwiki.net/XBDM_commands_by_version) for
  OG XDK 3146-5933 does not list these specific commands.
- Memory read/write/exec/reboot/sysinfo — full debug surface.
- File I/O on Xbox HDD — alternate to FTP.

**Debug kernels can run retail games** via `RetailGameLoader`
([Digiex thread](https://digiex.net/threads/retail-game-loader-v1-20-load-games-on-debug-developer-xbox.13818/)).
The screenshot command does not care whether the running XBE is
retail or homebrew — it reads the GPU framebuffer regardless.

This dissolves the "weeks of original kernel research" framing from
the kernel-landscape research (stream 1, agent
`adcf5a8b0b093e2d7`). Stream 1 was looking at *community custom
kernels* (Cromwell, Cerbios, M8+/Titan) and correctly found none
have kernel-resident TCP. Stream 1's blind spot was treating
"Microsoft kernel" as "the retail kernel only." Microsoft's debug
kernel does exactly what we need, and the OpenXenium chip can flash
a debug BIOS to one bank just as easily as any other BIOS.

The `nxdk_dyndxt` project ([abaire/nxdk_dyndxt](https://github.com/abaire/nxdk_dyndxt))
provides a runtime mechanism to extend XBDM with custom commands
loaded as dynamically-built nxdk libraries. So even if a specific
command is missing on OG Xbox (e.g., `autoinput`), we can add it.

## Unknowns to verify by experiment (one afternoon once Xbox is set up)

These are testable in 30 minutes each, total ~half-day:

1. **Does XBDM `screenshot` work mid-retail-game on OG Xbox?**
   Test: launch PGR2 via RetailGameLoader on the debug-kernel bank;
   issue `screenshot\r\n` over TCP/731; parse response. Expected:
   YES. Risk if NO: write a custom DXT to hook the video-flip path
   and DMA the framebuffer to a TCP socket (~2-3 weeks).
2. **Does XBDM `autoinput` exist on OG Xbox XDK?** Test: connect
   to debug kernel; issue `autoinput user=0 bind`; observe response
   code (200 success vs 405 not implemented). Expected: NO. Fix if
   missing: implement `autoinput` as a custom XBDM command via
   nxdk_dyndxt (~1-2 weeks).
3. **What is the exact pixel format of the `screenshot` response?**
   Probably 32-bit ARGB matching the framebuffer surface format,
   possibly swizzled. Test: capture a known-content frame, decode
   bytes, verify against XBE-side known content.
4. **Memory pressure on 64 MB Xbox.** Test: run XBDM resident +
   each retail game in turn; measure available memory; observe any
   crashes / asset loading degradation. If OOM on PGR2 or another
   memory-heavy title, plan a 128 MB RAM upgrade ($30 mod, soldering).
5. **Does the debug kernel's XBDM tolerate Insignia DNS redirect /
   network configuration?** Probably yes (XBDM is on port 731 vs
   Insignia's port 3074); verify no port conflict in our specific
   setup.
6. **PrometheOS install procedure on OpenXenium V1.4 firmware.**
   Verify the Mac-driven flow (XeniumOS → FTP PrometheOS XBE → run
   → flashes itself → reboot) works as documented.

## Effort estimate

| Scenario | Effort | Confidence |
|---|---|---|
| Best case — both `screenshot` and `autoinput` work on OG Xbox | ~1 person-week (Mac orchestrator + debug-kernel install + diagnostic-XBE library v1) | HIGH for `screenshot`, MEDIUM for `autoinput` |
| `screenshot` works, `autoinput` doesn't | + 1-2 weeks (add `autoinput` via nxdk_dyndxt) | MEDIUM |
| 128 MB RAM upgrade required | + 1 day (soldering + revalidation) | hardware-dependent |
| Worst case — `screenshot` doesn't work for retail games | + 2-3 weeks (custom DXT for video-flip framebuffer DMA) | LOW (no evidence; just unverified) |

Realistic total: **1-3 weeks for the full pipeline,** with most
weight on the 1-week best case.

## Sequencing recommendation

**Phase 0 — Mac-side prep (no Xbox required, can run in parallel
with retrieval decision):**

- Write the Python orchestrator skeleton (FTP / HTTP / XBDM TCP
  layer; ~150 lines).
- Write the nxdk diagnostic-XBE template with `libnxdk_net`
  framebuffer-to-TCP capture path (~100 lines + lib).
- Test the orchestrator against the existing `flat-tri-depth` XBE
  running under xemu (xemu doesn't implement XBDM, but the
  HTTP/FTP layer is exercisable).

**Phase 1 — Xbox bring-up (Day 1 with hardware):**

- FTP test to the existing XBMC4Xbox setup; confirm dashboard HTTP
  API responds.
- Dump current OpenXenium flash via Xenium-Tools; archive the dump
  on the Mac.
- Install PrometheOS to the OpenXenium chip (preserves XeniumOS in
  one bank as recovery).
- Flash a debug BIOS to bank 3.
- Verify XBDM is reachable on TCP/731 from the Mac via `xboxpy` or
  `ViridiX`.

**Phase 2 — Resolve unknowns (Day 2):**

- Boot debug bank.
- Launch PGR2 via RetailGameLoader.
- Test `screenshot` mid-game; resolve unknowns 1, 3, 4.
- Test `autoinput`; resolve unknown 2.
- Document findings in this doc + decision-log.

**Phase 3 — Production orchestrator (Days 3-7):**

- Per-game canonical-state reference captures (10-20 frames each).
- Diagnostic XBE library v1 — first wave of XBEs from
  `diagnostic-xbe-plan.md` (revised version) running with real-Xbox
  reference comparison.
- Integrate into `metal-canary-regress.sh` rotation.

**Phase 4 — Iterate (Week 2-3):**

- Expand diagnostic XBE library to second-wave coverage.
- If `autoinput` was missing on OG Xbox, implement it via
  nxdk_dyndxt.
- Build out remote game-launch automation.
- (Optional) Install IR / relay power-on mod for fully unattended
  testing.

## What this changes about existing project plans

The diagnostic-XBE plan (`diagnostic-xbe-plan.md`, originally
committed d57742ef47, Codex-flagged BLOCKING 2026-05-06) is being
revised to incorporate this oracle path. Specifically:

- **Self-validation tier ordering reverses.** Old Tier 1 (CPU-side
  VRAM readback) is renamed Tier 3 (escape hatch for guest-state
  probes only). New Tier 1 is host-side capture: on real Xbox,
  XBDM `screenshot` provides the canonical reference frame; on
  xemu, the existing `XEMU_METAL_SCREENSHOT_PATH` + F1 flip-stall
  trigger captures the equivalent. Comparison is per-pixel against
  the real-Xbox gold.
- **Math-derivation as oracle becomes audit material.** The XBE
  source-file header still derives expected output from first
  principles (for reviewer audit), but the empirical reference is
  the real-Xbox capture. Math and real-Xbox must agree; if they
  disagree, either the math is wrong (we update) or NV2A has
  undocumented behavior (we update the catalog).
- **CRTC-publish XBE re-architected.** Old design self-classified
  fallback behavior from guest memory — Codex correctly flagged
  this as a category error (host-side behavior not visible to
  guest). New design: XBE renders distinctive content into surface
  A vs surface B; host capture sees what's published; orchestrator
  per-flag-setting expected-result lookup picks the correct
  reference.
- **§3a unresolved items resolve by experiment.** ARL bias rounding,
  edge flags, line stipple, point-sprite enable source — all
  testable directly on real Xbox. Run the probe XBE on real
  hardware, observe output, document the empirical answer.

The full plan revision is in `diagnostic-xbe-plan.md` (revised
2026-05-06).

## Cost summary

| Item | Cost | Required when |
|---|---|---|
| Xbox retrieval from storage | $0 | One-time |
| Ethernet cable | $0 | One-time |
| 128 MB RAM upgrade kit | $30 | Only if PGR2 / heavy titles OOM |
| ESP32 + 56 kHz IR LED for remote power-on | $10 | Only if unattended testing required |
| OR Commercial XERC 2 XE IR mod | $25-50 | Same as above |
| FT2232H USB JTAG (worst-case CPLD recovery) | $20 | Only if OpenXenium chip-OS catastrophic failure |
| All other tooling | $0 | Already on Mac (nxdk, lftp, Python, xemu) |

Best-case zero-dollar path: Xbox + ethernet, manual power-on each
session, no RAM upgrade. Most-realistic ~$30-40 path: + IR power-on
mod for unattended overnight runs.

## Decision pending

User decision required:

1. **Retrieve Xbox from storage and proceed with the real-Xbox
   oracle path.** Phase 0 (Mac-side prep) can run while the user
   schedules retrieval. After retrieval: Phases 1-4 over 1-3 weeks.
2. **Defer Xbox retrieval; build the diagnostic-XBE library against
   xemu-only first** (host-side capture per Option C). Real-Xbox
   becomes a future second pass when the user is ready.
3. **Skip real Xbox entirely; accept math-derivation as oracle for
   diagnostic XBEs** and add a USB capture card later if retail-
   game oracle becomes critical.

Project memory entry (cross-session prior):
`memory/project_real_xbox_oracle.md`.

## Sources

### Apple Silicon Mac compatibility

- [XboxDev/nxdk PR #667 — Apple Silicon arm64 LLVM fix](https://github.com/XboxDev/nxdk/pull/667)
- [nxdk wiki — Install the Prerequisites](https://github.com/XboxDev/nxdk/wiki/Install-the-Prerequisites)
- [pyxbe (PyPI)](https://pypi.org/project/pyxbe/)
- [XboxDev/xboxpy](https://github.com/XboxDev/xboxpy)
- [XboxDev/extract-xiso](https://github.com/XboxDev/extract-xiso)

### XBDM (Microsoft debug kernel service)

- [Xbox Debug Monitor (xboxdevwiki)](https://xboxdevwiki.net/Xbox_Debug_Monitor)
- [XBDM commands by version (xboxdevwiki)](https://xboxdevwiki.net/XBDM_commands_by_version)
- [Experiment5X/XBDM source — screenshot + autoinput implementation](https://github.com/Experiment5X/XBDM/blob/master/Xbdm.cpp)
- [Experiment5X/XBDM header — gamepad APIs](https://github.com/Experiment5X/XBDM/blob/master/Xbdm.h)
- [XboxDev/ViridiX — Original Xbox debug suite (C# .NET, MIT)](https://github.com/XboxDev/ViridiX)
- [abaire/xbdm_gdb_bridge — OG Xbox XBDM ↔ GDB bridge with screenshot](https://github.com/abaire/xbdm_gdb_bridge)
- [abaire/nxdk_dyndxt — runtime XBDM extension (Unlicense)](https://github.com/abaire/nxdk_dyndxt)
- [docbrown/xbdm-rs — Rust XBDM client](https://github.com/docbrown/xbdm-rs)
- [XboxDev/nxdk-rdt — protocol scaffolding (XBE-side daemon)](https://github.com/XboxDev/nxdk-rdt)
- [Retail Game Loader v1.20 (Digiex)](https://digiex.net/threads/retail-game-loader-v1-20-load-games-on-debug-developer-xbox.13818/)
- [Xbox:Debug — ConsoleMods Wiki](https://consolemods.org/wiki/Xbox:Debug)

### OpenXenium and dashboards

- [Ryzee119/OpenXenium README](https://github.com/Ryzee119/OpenXenium/blob/master/README.md)
- [OpenXenium INSTALLATION.md](https://github.com/Ryzee119/OpenXenium/blob/master/INSTALLATION.md)
- [Ryzee119/Xenium-Tools (in-system flasher XBE)](https://github.com/Ryzee119/Xenium-Tools)
- [Xenium-Tools releases](https://github.com/Ryzee119/Xenium-Tools/releases)
- [MakeMHz/xenium-fw-update](https://github.com/MakeMHz/xenium-fw-update)
- [XeniumMods/XeniumUpdate](https://github.com/XeniumMods/XeniumUpdate)
- [Team-Resurgent/PrometheOS-Firmware](https://github.com/Team-Resurgent/PrometheOS-Firmware)
- [PrometheOS V1.5.0 release notes](https://www.xbox-scene.info/articles/prometheos-v150-released-huge-additions-and-updates-to-prom-tools-r56/)
- [Cerbios/Cerbios-Xbox BIOS](https://github.com/Cerbios/Cerbios-Xbox)
- [Team-Resurgent/CerbiosTool](https://github.com/Team-Resurgent/CerbiosTool)
- [XBMC4Xbox HTTP API (Web Archive)](https://web.archive.org/web/20240525074335/https://www.xbmc4xbox.org.uk/wiki/Web_Server_HTTP_API)
- [XBMC4Xbox List_of_Built_In_Functions (Web Archive)](https://web.archive.org/web/2023/https://www.xbmc4xbox.org.uk/wiki/List_of_Built_In_Functions)
- [XBMC4Xbox Scripts/Autoexec.py (Web Archive)](https://web.archive.org/web/20210708040939/https://www.xbmc4xbox.org.uk/wiki/Scripts/Autoexec.py)
- [Rocky5/XBMC4Xbox fork](https://github.com/Rocky5/XBMC4Xbox)
- [Rocky5/XBMC4Gamers](https://github.com/Rocky5/XBMC4Gamers)
- [ConsoleMods OpenXenium wiki](https://consolemods.org/wiki/Xbox:OpenXenium)
- [ConsoleMods PrometheOS wiki](https://consolemods.org/wiki/Xbox:PrometheOS)
- [ConsoleMods XeniumOS wiki](https://consolemods.org/wiki/Xbox:XeniumOS)

### Hardware

- [Macronix MX29LV160 datasheet (flash endurance)](https://www.macronix.com/Lists/Datasheet/Attachments/8520/MX29LV160D%20T-B,%203V,%2016Mb,%20v1.2.pdf)
- [128 MB RAM upgrade kit — ModzvilleUSA](https://modzvilleusa.com/products/xbox-128-ram-upgrade-kit-for-1-0-1-5-ogxbox-4x-sticks-of-ram)
- [128 MB RAM upgrade procedure (quade.co)](https://quade.co/2018/xbox-128mb-ram-upgrade/)
- [XERC 2 XE IR power mod](https://www.ebay.com/itm/254024289332)
- [Original Xbox power-button discussion (AVForums)](https://www.avforums.com/threads/remote-control-of-xbox-power-button.75489/)
- [Xbox 56 kHz IR carrier (Righto blog)](https://www.righto.com/2010/12/64-bit-rc6-codes-arduino-and-xbox.html)

### Xbox kernel landscape (negative findings)

- [insignia-live/xblunblock — RAM patcher, not parallel kernel networking](https://github.com/insignia-live/xblunblock)
- [Wikipedia — Insignia (Xbox)](https://en.wikipedia.org/wiki/Insignia_(Xbox))
- [XboxDev/cromwell (Linux loader BIOS, not retail-game host)](https://github.com/XboxDev/cromwell)
- [openxdk single-process model](https://openxdk.sourceforge.net/openxdk/xbox.html)
- [xboxdevwiki Boot_Process](https://xboxdevwiki.net/Boot_Process)
- [xboxdevwiki Kernel](https://xboxdevwiki.net/Kernel)

### Comparable consoles' homebrew (architecture references)

- [emukidid/swiss-gc (GameCube)](https://github.com/emukidid/swiss-gc)
- [FIX94/Nintendont (Wii/GC HID-layer interception)](https://github.com/FIX94/Nintendont)
- [SamsidParty/OberonRemote (Xbox One/Series; not OG Xbox)](https://github.com/SamsidParty/OberonRemote)
- [XeniumMods/OGX360 (USB-HID forgery hardware)](https://github.com/XeniumMods/OGX360)
