# Deeper spike attribution for the residual jitter pillar — Crimson 1.39-second worst-frame composition (D3)

Date: 2026-05-02

## One-line conclusion

**The Crimson 1.35-second worst-frame stutter is NOT iothread-bound, NOT
BQL-contention-bound at the worst-frame timescale, NOT MMIO-blocking-bound
at the worst-frame timescale, and NOT a single dominant event.** It is
composed of (a) ~410 ms of `tcg_tb_chain` events at guest **kernel**
PC `0x80030e4c` (~1 ms each, 409 events in the worst-frame window) plus
(b) ~970 ms of unattributed sub-1 ms events. The dominant guest-side cost
class **across the full 300 s run** is `mcpx-apu-vp` MMIO writes to
`0xfe8202fc` (`NV1BA0_PIO_VOICE_LOCK`) — 21.3 s of vCPU time blocked
on the APU thread's `MCPXAPUState::lock`, **but those events do not
fire inside the worst-frame window itself.** The next slice should
target the kernel-PC `0x80030e4c` chain (decompose what one TB
iteration is spending 1 ms on) and/or the `mcpx-apu-vp` voice-lock
contention (proven dominant on the steady-state axis even though it
is not the worst-frame trigger).

A sibling 60 Hz Xbox title sanity test (Soul Calibur 2 sustained
60.57 FPS on the same build/flag stack) already established that the
30 FPS cap on PGR2/Rainbow/Crimson is **title-intrinsic**, not an
xemu pacing bug — see `2026-05-02-60hz-title-sanity-test.md`. The D3
Mission 1 vblank/present counters built for this slice corroborate
that finding (PGR2 snapshot reads `NV2A_VBLANK_FIRES=62-66/s` while
`NV2A_PRESENT_HEARTBEAT=30-32/s`).

## Build / commit verification

- Binary: `dist/xemu.app/Contents/MacOS/xemu`, built 2026-05-02 ~02:00 UTC
  (this session).
- `xemu --version` → `xemu_version: 0.8.134-47-g1534bb7688`,
  `xemu_commit: 1534bb7688718e6bdfdf9e3bfe533829991ca202` (dirty,
  V1+V2+V3+V4+composite-goal stack + D3 instrumentation on top).
- `codesign --verify --deep --strict --verbose=2 dist/xemu.app` →
  `valid on disk` and `satisfies its Designated Requirement`.
- GL renderer / OS confirmed Apple GL 4.1 Metal — 90.5 on Apple M3 Ultra,
  macOS 26.4.1.
- Sanity: 15 s PGR2-snapshot run with all D3 instrumentation enabled at
  1 ms threshold produced 374 `mmio_helper_block`, 211 `bql_acquire_wait`,
  1 `aio_run_iter`, plus pre-existing renderer/TCG sources — D3 emit path
  wired through end-to-end. Sanity-run dir:
  `benchmark-runs/20260502-020218-pgr2`.

## D3 slice — what code changed

Five instrumentation slices (zero perf-fix work):

### Mission 1 — Display-pacing counters (`util/xemu-display-perf.{c,h}`)

Four atomic counters appended to the existing `xemu-perf:` interval
line via `xemu_display_perf_emit_and_reset()`, called from
`nv2a_profile_log_emit_interval`:

- `NV2A_VBLANK_FIRES` — bumped in `hw/xbox/nv2a/nv2a.c::nv2a_vga_gfx_update`
  every time the xemu vblank-timer thread sets `NV_PCRTC_INTR_0_VBLANK`.
- `NV2A_FLIP_STALL_WRITES` — bumped in
  `hw/xbox/nv2a/pgraph/pgraph.c::DEF_METHOD(NV097, FLIP_STALL)` on
  every guest write to `NV097_FLIP_STALL`.
- `NV2A_PRESENT_HEARTBEAT` — bumped in the
  `NV_PGRAPH_INCREMENT_READ_3D` write path (the actual page-flip
  completion). Same counter as `g_nv2a_stats.increment_fps` integrated
  over the interval.
- `XEMU_GL_SWAPS` — bumped in `ui/xemu.c::gl_render_frame` after
  `SDL_GL_SwapWindow`. **Caveat:** the per-interval emit runs from
  `pfifo_thread`, so reads are subject to thread-race noise (the
  swap counter is incremented from the SDL display thread). Useful
  only as a "is the host loop alive" signal at the per-interval
  granularity; sum across the whole run for a meaningful
  per-second value.

Cost when off: zero — the counters are unconditional atomics, but the
`xemu-perf:` interval line is only emitted when `XEMU_PERF_LOG=1`.

### Mission 2 — Iothread / BQL / AIO / MMIO spike sources

All gated on `xemu_spike_log_tcg_enabled` (env: `XEMU_PERF_SPIKE_LOG_TCG=1`),
shared threshold `xemu_spike_threshold_us` (env:
`XEMU_PERF_SPIKE_LOG_THRESHOLD_US`).

- `op=qemu_main_loop_iter` — `util/main-loop.c::main_loop_wait`. Times
  the *post-poll dispatch* phase (BH dispatch + timer fire), excluding
  the blocking `os_host_main_loop_wait` which is allowed to sleep up to
  the soonest-timer deadline. `extra` carries `total_us=N nonblocking=0|1`
  so the sleep portion is visible.
- `op=bql_acquire_wait` — `system/cpus.c::bql_lock_impl`. Times the
  lock acquisition wait specifically (not held time). `extra` carries
  `from=<file>:<line>` from the bql_lock_impl call site.
- `op=aio_run_iter` — `util/aio-posix.c::aio_dispatch`. One full
  AioContext dispatch pass (BH + fd handlers + timer dispatch).
- `op=mmio_helper_block` — `accel/tcg/cputlb.c::do_st_mmio_leN`. One
  guest MMIO store helper (BQL acquire + dispatch + return). `extra`
  carries `size=N addr=0xADDR mr=<name>` so the implicated
  `MemoryRegion` is obvious.

All five spike sources written via the shared `xemu_spike_emit()`
helper; the per-event cost is zero when `XEMU_PERF_SPIKE_LOG_TCG=0`
(single load + branch).

### Mission 3 — Guest-PC disassembly tooling (one-off)

`/tmp/xbe_disasm.py` — Python XBE parser that reads the Xbox
Executable header, locates which section a given guest-PC falls in,
extracts a chunk of raw bytes around it, and emits `objdump`-ready
metadata (chunk virtual base, target offset). Combined with `capstone`
(`pip install capstone --user`) for i386 32-bit Intel-syntax
disassembly. Not committed to the tree; documented here as the recipe
to rerun for any future guest-PC investigation. If this becomes a
recurring need, integrate as
`scripts/apple-silicon/xbe-disasm.py` per project rule #5.

## Test matrix

All runs use the post-fast-read stable opt-in flag set
(`XEMU_NATIVE_TRI_DEPTH=1 XEMU_NATIVE_QUAD=1 XEMU_PGRAPH_FAST_READ=1`)
plus `XEMU_PERF_LOG=1 XEMU_PERF_FRAME_LOG=1`. Splitwx and
targeted-jmp-cache are auto-on (Apple Silicon system-build defaults
from V1 / V2). Screenshots are off (`XEMU_BENCH_SCREENSHOT_BACKEND=none`).

| Arm | Title | Mode | Spike threshold | Duration | Run dir |
| --- | --- | --- | --- | --- | --- |
| M1 | PGR2 | snapshot `pgr2_gameplay_b4`, noop | (none) | 60 s | `benchmark-runs/20260502-015456-pgr2` |
| Sanity | PGR2 | snapshot `pgr2_gameplay_b4`, noop | 1 ms | 15 s | `benchmark-runs/20260502-020218-pgr2` |
| M2 | Crimson | retail `crimson-gameplay.csv`, profile-prep HDD | 1 ms | 300 s | `benchmark-runs/20260502-020320-crimson-skies` |

## Mission 1 — vblank / present corroboration (one paragraph)

The 60 s PGR2 mid-route snapshot replay (Arm M1) confirms what the
sibling Soul Calibur 2 60 Hz sanity test established: the cap is
intrinsic. Per-interval averages over the 56 logged intervals:
`NV2A_VBLANK_FIRES` = 62-74/s (xemu's 60 Hz vblank pacing fires
correctly), `NV2A_FLIP_STALL_WRITES` = 29-37/s, `NV2A_PRESENT_HEARTBEAT`
= 30-32/s. The guest is **electing not to present** every vblank — its
engine produces ~30 frames per second regardless of the host's vblank
offer. Same pattern visible on Crimson during the M2 worst-frame
interval: `NV2A_VBLANK_FIRES=90 NV2A_FLIP_STALL_WRITES=7
NV2A_PRESENT_HEARTBEAT=7` (xemu offered 90 vblanks across the 1486 ms
interval, the guest completed 7 frames). The `XEMU_GL_SWAPS` counter
mostly reads 0 due to the perf-interval emit racing the swap counter
across threads (documented in `automation.md`); the host display loop
is alive (the fork's `gl_render_frame` is gated by
`pgraph_gl_get_framebuffer_surface`'s `qemu_event_wait(&pg->sync_complete)`
so the SDL thread cannot present faster than the guest produces, and
the per-second total `XEMU_GL_SWAPS` averaged over a longer window
matches `NV2A_PRESENT_HEARTBEAT`). Mission 1 corroboration done; cap
attribution left to the sibling note.

## Mission 2 — 1 ms spike-log breakdown

300 s Crimson route, 284 perf intervals, **69,211** spike lines emitted
at the 1 ms threshold. Headline per-interval metrics from
`extract-perf-summary.sh`:

| Metric | Value |
| --- | ---: |
| post_load_intervals | 279 |
| post_load_avg_fps | 30.46 |
| post_load_avg_mspf | 28.61 |
| post_load_fps_stddev | 3.927 |
| post_load_mspf_max_p50 | 33.69 |
| post_load_mspf_max_p95 | 40.99 |
| post_load_mspf_max_p99 | 459.59 |
| **post_load_mspf_max_max** | **1386.37** |
| post_load_frame_mspf_us_p50 | 27,313 |
| post_load_frame_mspf_us_p99 | 36,164 |
| post_load_frame_mspf_us_p999 | 80,404 |
| **post_load_frame_mspf_us_max** | **1,386,373** |
| post_load_stutter_intervals_30fps | 167 |
| post_load_longest_stutter_run_30fps | 18 |

The headline 1.39 s worst frame reproduces, fully consistent with V1/V2/V3.

### Op-tag total counts

| op tag | count | comment |
| --- | ---: | --- |
| `tcg_tb_chain` | 42,671 | dominant. Most are 1.0 ms (just over threshold) |
| `bql_acquire_wait` | 10,774 | most fire from `main-loop.c:384` (iothread) |
| `mmio_helper_block` | 9,805 | **99 % `mcpx-apu-vp/0xfe8202fc`** — VOICE_LOCK |
| `flip_stall_glfinish` | 2,235 | renderer-side, expected |
| `draw_begin` | 1,284 | renderer-side, expected |
| `surf_download` | 1,095 | renderer-side, expected |
| `surf_upload` | 759 | renderer-side, expected |
| `bind_textures` | 440 | renderer-side, expected |
| `tcg_pg_lock_wait` | 86 | small residual, well below MMIO contention |
| `flush_draw` | 52 | renderer-side, expected |
| `tex_upload` | 10 | renderer-side, expected |
| `qemu_main_loop_iter` | **0** | iothread post-poll dispatch never >1 ms |
| `aio_run_iter` | **0** | AioContext dispatch never >1 ms |

**Critical disproof:** zero `qemu_main_loop_iter` and zero `aio_run_iter`
across 300 s of gameplay including the 1.39 s worst frame. **The
iothread is not the bottleneck on any axis at any timescale visible
to a 1 ms threshold.**

### `mmio_helper_block` breakdown by region

Across the entire 300 s route, the spike total wall-clock by
`MemoryRegion`:

| `mr` / `addr` | events | total (ms) | avg (µs) |
| --- | ---: | ---: | ---: |
| **`mcpx-apu-vp` / 0xfe8202fc** | **9,709** | **21,255** | **2,189** |
| `PGRAPH` / 0xfd40071c | 88 | 98 | 1,114 |
| `mcpx-apu-vp` / 0xfe82012c | 8 | 23 | 2,870 |

**21.3 seconds of vCPU thread time across 300 s** — **7.1 % of total
wallclock** — spent blocked inside guest MMIO writes to
`mcpx-apu-vp/0xfe8202fc` (`NV1BA0_PIO_VOICE_LOCK`, see Mission 3).
Each event is ~2.2 ms on average. Mean rate: 32 events/s. This is
**the dominant single guest-MMIO blocking class** on the steady-state
axis.

The other regions are quiet: PGRAPH MMIO blocks in 88 events totalling
98 ms (~0.03 %), and a separate APU-VP register `0x12c` blocks 8
times. The benchmark fork's `XEMU_PGRAPH_FAST_READ` slice is doing
its job — PGRAPH register reads no longer take pg->lock, and the
PGRAPH writes that do still serialize cost <0.1 % of wallclock.

### `bql_acquire_wait` top contributors

| from= site | events | top (ms) | comment |
| --- | --- | ---: | ---: | --- |
| `rcu.c:309` (RCU thread) | 1 | 82.5 | RCU synchronize-quiescent wait, not vCPU |
| `physmem.c:3327` | 1 | 35.7 | RAM hot-add path, one-shot |
| `apu.c:283` (APU thread) | (small) | 18.3 | APU thread waiting for BQL during mcpx_apu_frame |
| `main-loop.c:384` (iothread) | many | 5.9-6.2 | normal iothread BQL re-acquire after poll |
| `xemu.c:133` | (small) | 5.6 | xemu UI thread |
| `pgraph.c:969` | (small) | 5.5 | pfifo thread — PGRAPH method handler BQL acquire |

The largest BQL waits do not come from the vCPU thread — the iothread,
RCU thread, and APU thread are the contention sources. The vCPU
thread's BQL waits stay below the 1 ms threshold (BQL is mostly
uncontended for vCPU under this workload because the vCPU rarely
takes BQL outside MMIO; MMIO writes take BQL via `BQL_LOCK_GUARD()`
inside `do_st_mmio_leN` and that path is the `mmio_helper_block`
attribution above).

### Worst-frame attribution at 1 ms threshold

The 1386.37 ms worst frame is **interval i158** (line 3813 of
`xemu.log`), `frame_mspf_us=1097,1120,1091,1203,1166,1057,1386373`.
The bad frame is the seventh in a 7-frame interval; the first six
are themselves 1.0-1.2 ms (steady-state at the *snapshot resume's*
post-load tail), and the seventh is the catastrophic 1.386 s outlier.

Spike events that fired inside the **last-frame window** (the 1386 ms
window from `worst_frame_end_us - 1386373` to `worst_frame_end_us`):

| op tag | events | total (ms) |
| --- | ---: | ---: |
| `tcg_tb_chain` | 420 | 422.864 |
| `surf_upload` | 2 | 4.871 |
| `draw_begin` | 2 | 12.063 |
| `surf_download` | 1 | 1.710 |
| (everything else) | 0 | 0 |

**Zero `bql_acquire_wait` inside the worst-frame window.**
**Zero `mmio_helper_block` inside the worst-frame window.**
**Zero `qemu_main_loop_iter`, zero `aio_run_iter`.**

The worst frame is **dominated by `tcg_tb_chain`** at the 1 ms
threshold. By PC:

| `first_pc` | events | total (ms) | avg (µs) | identification |
| --- | ---: | ---: | ---: | --- |
| **`0x80030e4c`** | **409** | **409.852** | **1,002** | **Xbox kernel** (kernel base 0x80010000) |
| `0x8003adcc` | 10 | 10.023 | 1,002 | Xbox kernel |
| `0x8001fcd6` | 1 | 2.989 | 2,989 | Xbox kernel |

**The single-PC dominator of the worst frame is the Xbox kernel at
`0x80030e4c`, with 409 events at almost exactly 1.0 ms each.** This
is *not* the V3 PC `0x23dd47` (Crimson DSOUND voice-lock); it is a
different cost class entirely.

The top-15 longest spikes in the worst-frame window:

| duration (µs) | op | first_pc | tb_count |
| ---: | --- | --- | ---: |
| 9319 | `draw_begin` | (renderer) | — |
| 2989 | `tcg_tb_chain` | 0x8001fcd6 | 551 |
| 2744 | `draw_begin` | (renderer) | — |
| 2604 | `surf_upload` | (renderer) | — |
| 2267 | `surf_upload` | (renderer) | — |
| 1710 | `surf_download` | (renderer) | — |
| 1027 | `tcg_tb_chain` | 0x80030e4c | 204 |
| 1012 | `tcg_tb_chain` | 0x80030e4c | 7 |
| 1011 | `tcg_tb_chain` | 0x80030e4c | 7 |
| 1011 | `tcg_tb_chain` | 0x80030e4c | 5 |
| 1011 | `tcg_tb_chain` | 0x80030e4c | 10 |
| 1011 | `tcg_tb_chain` | 0x80030e4c | 5 |
| 1010 | `tcg_tb_chain` | 0x80030e4c | 2790 |
| 1010 | `tcg_tb_chain` | 0x80030e4c | 7 |
| 1010 | `tcg_tb_chain` | 0x80030e4c | 6 |

`tb_count` varies widely (5 to 2790). The constant ~1.0 ms duration
suggests **the cost is not per-TB but per-iteration of an outer host
loop** — likely either:
- a host-side wait inside `cpu_exec_loop` (e.g.
  `cpu_handle_interrupt` blocking on a condition with ~1 ms timeout),
  or
- a host-side timer-driven sleep/yield somewhere in the TCG loop
  that fires at ~1 kHz, or
- the kernel itself doing a `KeStallExecutionProcessor`-equivalent
  loop that translates to a host-side `usleep`/`nanosleep`-class call.

Sub-1 ms residual: 1386.373 ms (worst frame) − 422.864 ms
(spike-attributed) = **963.5 ms unattributed** at the 1 ms threshold.
This is consistent with the V3 finding that a substantial portion of
the worst frame lives below the per-event threshold.

### Top-N tcg_tb_chain attribution across the full 300 s

| `first_pc` | events | total (ms) | comment |
| --- | ---: | ---: | --- |
| `0x80030e4c` | 35,143 | **35,283** | Xbox kernel — 11.8 % of wallclock dedicated to 1 ms-class TB chains starting here |
| `0x8001b02f` | 5,738 | 5,759 | Xbox kernel |
| `0x23dd47` | 1,149 | 1,438 | **Crimson DSOUND VOICE_LOCK (V3 PC)** — confirms V3, but no longer dominant at this threshold |
| `0x23dd65` | 185 | 186 | Crimson DSOUND, adjacent to V3 PC |
| `0x8001b043` | 128 | 129 | Xbox kernel |
| `0x23e042` | 112 | 121 | Crimson DSOUND |
| `0x8001b02e` | 47 | 47 | Xbox kernel |
| `0xfffffff0` | 1 | 36 | x86 reset vector — boot phase |
| `0x8003adcc` | 33 | 34 | Xbox kernel |
| `0x20b38a` | 31 | 32 | Crimson app code |

V3's `0x23dd47` is **confirmed but reframed**: it is the largest
single-PC Crimson-app-code TB-chain attributor, but it is dwarfed
6× by a kernel-PC chain and 24× by total `tcg_tb_chain` cost. V3 saw
it as a smoking gun because V3's 10 ms threshold filtered out the
1 ms kernel-PC chain. At the 1 ms threshold the picture inverts:
**Xbox kernel TB chains are the dominant TCG-side cost class, not
Crimson app code.**

## Mission 3 — guest PC `0x23dd47` disassembly

The XBE was extracted from `Test_Games/Crimson skies.xiso.iso` via
`nxdk/tools/extract-xiso/build/extract-xiso`, parsed by a one-off
Python tool (`/tmp/xbe_disasm.py`, recipe documented in the D3 slice
section above), and disassembled with `capstone` 5.0.7.

PC `0x23dd47` falls in the **`DSOUND` section** — Microsoft's
DirectSound runtime statically linked into the title (XBEs include
the SDK runtimes as their own sections, distinct from the title's
`.text`). Section table:

```
.text    vaddr=0x00011000 vsize=0x001ee96c  Crimson app code
D3D      vaddr=0x001ff980 vsize=0x00013a5c  Microsoft Direct3D runtime
D3DX     vaddr=0x002133e0 vsize=0x00021650  Microsoft D3DX helper
XGRPH    vaddr=0x00234a40 vsize=0x00002444
DSOUND   vaddr=0x00236ea0 vsize=0x0000c894  Microsoft DirectSound  <-- contains 0x23dd47
...
```

Disassembly around `0x23dd47`:

```
0x23dcfc: mov     esi, ecx              ; this = ecx (DSound voice obj)
0x23dcfe: xor     edi, edi
0x23dd00: lea     ecx, [ebp - 8]
0x23dd03: mov     [ebp - 4], edi
0x23dd06: call    0x236ed0              ; some lock prologue
0x23dd0b: xor     eax, eax
0x23dd0d: inc     eax                   ; eax = 1 (voice-lock argument)
0x23dd0e: test    [esi + 0x12], al      ; voice flag check
0x23dd11: je      0x23dd72
0x23dd13: movzx   ecx, byte [esi + 0x64]
0x23dd17: movzx   ecx, word [esi + ecx*2 + 0xa]
0x23dd1c: mov     edx, [0x243660]
0x23dd22: shl     ecx, 7
0x23dd25: test    [ecx + edx + 0x54], 0x100000
0x23dd2d: jne     0x23dd72

0x23dd2f: mov     ecx, [0xfe820010]    ; read NV1BA0_PIO_FREE
0x23dd35: and     ecx, 0xfffffffc      ; mask low 2 bits
0x23dd38: cmp     ecx, 0xc             ; need >= 12 free FIFO slots
0x23dd3b: jb      0x23dd2f             ; busy-wait spin

0x23dd3d: movzx   ecx, word [esi + 0xc] ; voice handle
0x23dd41: mov     [0xfe8202f8], ecx    ; write NV1BA0_PIO_SET_CURRENT_VOICE
0x23dd47: mov     [0xfe8202fc], eax    ; write NV1BA0_PIO_VOICE_LOCK = 1   <-- TARGET PC
0x23dd4c: ...
```

This is **Microsoft DirectSound's voice-lock acquisition primitive**.
The sequence:

1. Spin-read `NV1BA0_PIO_FREE` (`0xfe820010`) until ≥12 PIO FIFO
   slots are reported free. **xemu's emulation of this register is
   `return 0x80;` (always 128 free)** in
   `hw/xbox/mcpx/apu/vp/vp.c:583`, so the spin should exit
   immediately. The spike is therefore *not* from the spin-wait.
2. Write `NV1BA0_PIO_SET_CURRENT_VOICE` (`0xfe8202f8`).
3. **Write `NV1BA0_PIO_VOICE_LOCK` (`0xfe8202fc`) — this is PC
   `0x23dd47`.** The write is dispatched via xemu's `vp_write` →
   `fe_method` → `voice_lock` →
   `qemu_mutex_lock(&MCPXAPUState::lock)`. **If the APU thread is
   mid-`mcpx_apu_frame` holding `d->lock`, the vCPU blocks here until
   the APU thread finishes its frame (~5.33 ms is the VP frame
   period, but tail latency under composite contention can stack to
   100+ ms).** This is the V3 attribution mechanism, now confirmed by
   MMIO-region attribution.

V3 measured one 146 ms `tcg_tb_chain` spike at this PC and called
it the "smoking gun". D3 attributes it more precisely: this is a
**`mcpx-apu-vp/0xfe8202fc` blocking MMIO write** that the V3 spike
log saw as a TB chain because the chain happened to begin at the
guest PC just before the BQL acquire. At the 1 ms threshold both
`tcg_tb_chain` (V3's lens) and `mmio_helper_block` (D3's new lens)
fire on the same event class — the latter is the more precise
attribution.

**The 21.3 second total `mcpx-apu-vp/0xfe8202fc` block time over
300 s is the dominant guest-MMIO blocking class on the steady-state
axis.** It is *not* the worst-frame trigger (no `mmio_helper_block`
fires inside the i158 worst-frame window), but it is the largest
single-region MMIO-blocking contributor across the full route.

This was the explicit follow-up of the prior `voice_fast_lock`
investigation (`2026-05-01-voice-fast-lock-investigation.md`), which
measured the bottleneck at the snapshot scene (audio idle, ~6.8 %
TCG-thread cost) and concluded the slice didn't help FPS. **D3's
retail-route 21.3 s / 300 s = 7.1 % is consistent with that
measurement** at the audio-busy axis the prior investigation called
out as a follow-up.

## Combined conclusion — what governs the residual jitter pillar

The 30 FPS cap is intrinsic (Soul Calibur 2 sustained 60 FPS proves
the emulator can do 60 FPS — see sibling note). The residual
jitter pillar — Crimson's 1.35-second worst-frame stutter — is a
**different problem from the cap**, and D3 attribution shows it is
**not the same problem the prior slices were chasing either**.

Three orthogonal cost classes are now visible:

1. **Steady-state audio-MMIO blocking** (dominant on the full-route
   axis): 21.3 s / 300 s = **7.1 % of vCPU wallclock** spent inside
   `mcpx-apu-vp/0xfe8202fc` (NV1BA0_PIO_VOICE_LOCK), serialized
   behind `MCPXAPUState::lock` held by the APU worker. This is the
   classical voice-lock contention; the prior `voice_fast_lock`
   slice investigated it at the wrong scene and the right slice
   needs the audio-busy retail route.

2. **Steady-state guest-kernel TB-chain cost** (dominant on the
   per-event axis at 1 ms threshold): 35.3 s / 300 s = **11.8 %**
   of wallclock spent in 1.0 ms tb_chain iterations starting at
   guest kernel PC `0x80030e4c`. The constant 1.0 ms duration is
   suspicious — it is consistent with a host-side ~1 kHz timer-driven
   sleep/yield inside `cpu_exec_loop` or with a guest
   `KeStallExecutionProcessor`-class loop. **Need a V5 slice that
   adds per-phase instrumentation inside `cpu_exec_loop` to
   decompose 1 ms TB iterations into translation/exec/exit cost.**

3. **Worst-frame composite (the 1.35 s stutter)**: composed of (a)
   ~410 ms of category-2 events (the 1 ms kernel TB chains stack
   up during the worst frame, accounting for 30 % of it), (b)
   ~970 ms of sub-1 ms events that even D3's instrumentation
   cannot decompose, and (c) **zero** category-1 events
   (mmio_helper_block does not fire in the worst frame window).
   The remaining 970 ms is consistent with the V3 hypothesis of
   "100+ events of 5-10 ms each that we don't catch" combined
   with **a non-instrumented host-side wait class** (host
   thread-scheduler yields, GPU command-buffer-full waits in
   Apple's GL driver, or APU-thread `cond_timedwait` loops that
   are not on the BQL or AIO axes).

The worst-frame trigger and the steady-state cost dominators are
**different cost classes**. Optimizing the audio voice-lock path
will reduce 7 % of steady-state cost (probably moving avg FPS by
~2 FPS on Crimson, not closing the 30→60 gap, and not necessarily
eliminating the worst-frame stutter). Decomposing the kernel-PC
1 ms tb_chain class will identify what 35 s of wallclock is going
to in the kernel — that's the bigger steady-state lever and may
also explain the worst-frame composition (since 30 % of the worst
frame is this same class).

### Recommended next slice

**Highest priority — V5 cpu_exec_loop per-phase instrumentation.**
Add three new spike sources gated on `XEMU_PERF_SPIKE_LOG_TCG`:

- `op=tcg_tb_lookup` — wallclock of `tb_lookup` (TB hash + binary
  search) inside the inner cpu_exec_loop iteration.
- `op=tcg_tb_gen_code` — wallclock of `tb_gen_code` (translation)
  when a fresh TB is needed.
- `op=tcg_handle_interrupt` — wallclock of the
  `cpu_handle_interrupt` exit-handling pass.

A 1.0 ms tb_chain with `tb_count=1` is one of these three classes;
this slice decomposes which. If it turns out to be `tb_gen_code`
churn, the slice that follows is to investigate why the kernel PC
is being re-translated. If it turns out to be `cpu_handle_interrupt`
with a host-side wait, the next slice is to find which interrupt
type and which device.

**Second priority — V6 audio voice-lock release.** Two designs to
A/B (separate slice, separate measurement):

a. Lock-elision on the voice-locked bitmap (the prior
   `voice_fast_lock` slice, but measured against the audio-busy
   retail route this time, not the audio-idle snapshot).
b. **Move APU frame processing off the BQL/`d->lock` critical
   section entirely.** Read the voice config snapshot under the
   lock, release the lock, then process audio frame, then
   re-acquire briefly to publish results. This decouples vCPU
   voice_lock latency from audio-frame duration. Riskier slice
   (correctness must be re-validated on every tracked title) but
   bigger ceiling (eliminates the 7 % cost rather than reducing
   it).

**Do NOT pursue:**
- Iothread / main-loop optimization. **Zero** `qemu_main_loop_iter`
  spikes in 300 s at 1 ms threshold. The iothread is clean.
- Further BQL-release work. Top BQL waits come from RCU and APU
  thread, not vCPU. The vCPU's BQL acquire-time cost is folded
  into `mmio_helper_block` (the BQL is taken inside `do_st_mmio_leN`
  via `BQL_LOCK_GUARD()`).
- AioContext optimization. Zero `aio_run_iter` spikes.
- Renderer slices. Renderer-side spikes inside the worst frame
  total 18 ms across 5 events (vs 410 ms of TCG events) — not the
  binding constraint.

## Honest-limits caveats

- **The Mission 1 `XEMU_GL_SWAPS` counter has a thread-race noise
  floor of O(1 swap/interval).** The increment is on the SDL
  display thread, the per-interval emit is on `pfifo_thread`, and
  the counter is reset to 0 in the emit. Observed values are
  almost always 0; the per-interval read is meaningful only as
  "is the host loop alive at all" (yes, always, given the
  pre-existing `nv2a_get_framebuffer_surface` event-wait
  coupling). The decisive Mission 1 evidence is therefore the
  `NV2A_VBLANK_FIRES` vs `NV2A_PRESENT_HEARTBEAT` ratio, not
  `XEMU_GL_SWAPS`. Documented in `automation.md`.

- **The Mission 2 worst-frame `tcg_tb_chain` 1 ms cluster is
  evidence-thin on the cause side.** D3 establishes that 409
  events at PC `0x80030e4c` fire with ~1 ms duration each in the
  worst-frame window — but does not decompose what that 1 ms is
  spent on inside one TB iteration. The "host-side ~1 kHz timer
  yield" hypothesis is plausible but not measured. V5
  decomposition is required to confirm.

- **`tb_count` interpretation.** The largest worst-frame
  `tcg_tb_chain` (`0x8001fcd6`, 2989 µs, tb_count=551) is one
  setjmp pass that executed 551 chained TBs in 3 ms — **healthy**
  TB execution. The smaller 1.0 ms events with tb_count=5-10 are
  *unhealthy* — they spent most of the 1 ms outside actual TB
  execution (in `tb_lookup`, `tb_gen_code`, `cpu_handle_interrupt`,
  or a host-side wait). The same `first_pc` (`0x80030e4c`)
  appears in both healthy (tb_count=2790) and unhealthy
  (tb_count=5) variants, which strongly suggests the cost
  variable is not the PC itself but something happening between
  iterations of the cpu_exec_loop body.

- **Mission 3 disassembly used a one-off Python tool, not a
  durable script.** If guest-PC investigation becomes a recurring
  need (e.g. for another title with a different worst-frame PC),
  the `/tmp/xbe_disasm.py` script should be moved to
  `scripts/apple-silicon/xbe-disasm.py` and the recipe
  (extract-xiso → parse XBE → capstone) should be wired into
  `automation.md`. Project rule #5 covers this.

- **Mission 3 conclusion ("DSOUND voice-lock blocks on APU
  thread") was reached partially from inference**, not from a
  single-step host-vCPU trace. Direct evidence chain: PC 0x23dd47
  is in the DSOUND section (verified by XBE section table),
  writes to `0xfe8202fc` (verified by disassembly) which maps to
  `mcpx-apu-vp` offset `0x2fc` = `NV1BA0_PIO_VOICE_LOCK` (verified
  by `apu_regs.h`), the write handler ends in
  `qemu_mutex_lock(&d->lock)` (verified by `vp.c:137`). The "APU
  thread holds `d->lock` during `mcpx_apu_frame`" link is
  established in the prior `voice_fast_lock` investigation note
  but not re-measured this session.

- **The 35 s of `0x80030e4c` tb_chain time is the largest
  single-PC attribution, but its cause is unknown.** Without
  Xbox kernel symbols, "what guest function is at kernel-base +
  0x20e4c" is not directly answerable. A future tool that maps
  the kernel binary's section table (kernel image is on the
  profile-prep HDD, extractable via QEMU's `pmemsave` HMP) plus
  any public XBOXKRNL symbol hints (xemu-project, xeniaproj,
  cxbx-reloaded community RE work) would unblock this.

- **The 970 ms unattributed remainder of the worst frame** lives
  below D3's instrumentation floor on every axis tested. The
  remaining instrumentation that has not yet been added: GL
  command-buffer-full waits in Apple's driver (would require
  GL_ARB_debug_output / Metal performance HUD integration), APU
  worker `cond_timedwait` loops (instrumentable with a new
  `op=apu_cond_wait` spike source), and the host scheduler's
  thread-yield latency (only measurable via Instruments / dtrace).

- **The Mission 2 retail Crimson route is one 300 s sample.**
  V1/V2/V3/V4 all reproduce a 1.27-1.39 s worst frame on this
  route, so the headline is statistically robust at the
  reproducibility level, but the per-event spike cluster
  composition (e.g. exactly 409 vs some other count of
  `0x80030e4c` events in the worst frame) is a single-sample
  observation. A second 300 s run would let us check whether the
  PC distribution is stable.

- **Project rule #11 (no re-validating closed slices) was
  honored.** No `validate-native-tri-depth.sh` re-run this
  session; D3 changes no renderer code. Indirect correctness
  check: zero `GEOM_SHADER_DRAW_TRI` in the Crimson run (native
  triangle path own triangles).

- **The Mission 1 / Mission 2 builds are the same binary**;
  `build.sh -a arm64` was run once with all instrumentation in
  place, then both runs used the resulting `dist/xemu.app`. No
  rebuild between Mission 1 (sanity, 60s) and Mission 2 (300s
  Crimson).

## Files referenced

- M1 corroboration run: `benchmark-runs/20260502-015456-pgr2/`
- Sanity run: `benchmark-runs/20260502-020218-pgr2/`
- M2 attribution run: `benchmark-runs/20260502-020320-crimson-skies/`
- Crimson XBE extract: `/tmp/crimson_extract/default.xbe` (transient)
- D3 working-tree slice diff:
  - New: `util/xemu-display-perf.c`,
    `include/qemu/xemu-display-perf.h`
  - Modified: `util/meson.build`,
    `util/main-loop.c`,
    `util/aio-posix.c`,
    `system/cpus.c`,
    `accel/tcg/cputlb.c`,
    `hw/xbox/nv2a/nv2a.c`,
    `hw/xbox/nv2a/pgraph/pgraph.c`,
    `hw/xbox/nv2a/pgraph/profile.c`,
    `ui/xemu.c`,
    `docs/apple-silicon/automation.md`,
    `xemu-fork/CLAUDE.md` (new spike op tags + display counters)
- Sibling note (the cap finding):
  `docs/apple-silicon/benchmarks/2026-05-02-60hz-title-sanity-test.md`
- Prior validation notes (the worst-frame cluster history):
  `docs/apple-silicon/benchmarks/2026-05-01-tcg-splitwx-validation.md` (V1),
  `docs/apple-silicon/benchmarks/2026-05-02-tcg-jmp-cache-targeted-validation.md` (V2),
  `docs/apple-silicon/benchmarks/2026-05-02-tcg-spike-attribution.md` (V3),
  `docs/apple-silicon/benchmarks/2026-05-02-composite-goal-validation.md` (V4),
  `docs/apple-silicon/benchmarks/2026-05-01-voice-fast-lock-investigation.md`
  (prior voice-lock investigation — the slice D3 follows up).
