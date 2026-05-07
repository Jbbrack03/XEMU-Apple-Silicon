/*
 * xbed_input_synth — diag-XBE shim that reads the oracle agent's
 * synthetic controller state buffer.
 *
 * Architecture
 * ------------
 * The oracle agent (`scripts/apple-silicon/xbe-tests/oracle-agent/`)
 * allocates a persistent kernel-pool buffer at startup that carries a
 * 4-port `oracle_ctrl_buffer` (see `oracle-agent/controller.h`). The
 * agent stores the buffer's physical address in the persistence anchor
 * file `E:\Apps\oracle-agent\state\ctrl-addr.txt` and flags the
 * allocation persistent via `MmPersistContiguousMemory`, so the buffer
 * survives the agent's own process death across an `XLaunchXBE`
 * chainload.
 *
 * A diag XBE that wants to read synthetic controller state (set by
 * Mac-side `controller.set` RPCs before the chainload):
 *
 *   1. Calls `xbed_input_synth_attach()` once at startup. The shim
 *      mounts E: if needed, reads the anchor file, validates the
 *      kernel-pool buffer's magic + version, and caches a pointer to
 *      it.
 *   2. Per frame, calls `xbed_input_synth_read(port, &state)` to copy
 *      the latest port state out of the buffer.
 *
 * The shim layer is intentionally separate from the agent's
 * `controller.h` so it can be vendored into any diag XBE without
 * dragging in the agent's lwIP / RPC dependencies.
 *
 * Vocabulary parity
 * -----------------
 * The state struct, button bit layout, and axis ranges are
 * byte-for-byte identical to the agent's `oracle_ctrl_port_state` and
 * to xemu's own `ControllerState` (see `ui/xemu-input.h`); a
 * `XEMU_RECORD_INPUT` CSV replays through the agent and into this shim
 * without any value-domain translation.
 *
 * Threading
 * ---------
 * Reads are non-atomic at the byte level; if a Mac-side
 * `controller.set` lands mid-read the shim may observe a partial
 * update for one frame. `xbed_input_synth_read` performs a
 * seq-stamped two-pass read (read seq, copy state, re-read seq, retry
 * up to N times if seq differs) so the caller always sees a
 * consistent snapshot.
 *
 * Failure modes
 * -------------
 * Every error path is recoverable; the shim never aborts the host
 * process. `xbed_input_synth_attach` returns one of the
 * `XBED_INPUT_SYNTH_*` codes so the caller can decide whether to fall
 * back to a CPU-painted oracle pattern, log + continue, or hard-stop.
 */
#ifndef XBED_INPUT_SYNTH_H
#define XBED_INPUT_SYNTH_H

#include <stdint.h>

#define XBED_INPUT_SYNTH_NUM_PORTS 4
#define XBED_INPUT_SYNTH_MAGIC     0x58435452u   /* 'XCTR' little-endian */
#define XBED_INPUT_SYNTH_VERSION   1u

/* Mirrors `enum controller_state_buttons_mask` in xemu's
 * `ui/xemu-input.h:41-57` and the agent's `ORACLE_BTN_*` constants in
 * `oracle-agent/controller.h:77-92`. Bit-for-bit compatible. */
#define XBED_BTN_A          (1u << 0)
#define XBED_BTN_B          (1u << 1)
#define XBED_BTN_X          (1u << 2)
#define XBED_BTN_Y          (1u << 3)
#define XBED_BTN_DPAD_LEFT  (1u << 4)
#define XBED_BTN_DPAD_UP    (1u << 5)
#define XBED_BTN_DPAD_RIGHT (1u << 6)
#define XBED_BTN_DPAD_DOWN  (1u << 7)
#define XBED_BTN_BACK       (1u << 8)
#define XBED_BTN_START      (1u << 9)
#define XBED_BTN_WHITE      (1u << 10)
#define XBED_BTN_BLACK      (1u << 11)
#define XBED_BTN_LSTICK     (1u << 12)
#define XBED_BTN_RSTICK     (1u << 13)
#define XBED_BTN_GUIDE      (1u << 14)

/* Per-port state — 26 bytes, packed. ABI-equivalent to the agent's
 * `oracle_ctrl_port_state` and to xemu's
 * `ControllerState.{buttons,axis[]}`. Triggers are int16 0..32767 in
 * xemu's range (NOT the post-`>> 7` Xbox HID-report u8); sticks are
 * int16 -32768..32767. */
struct __attribute__((packed)) xbed_port_state {
    uint16_t buttons;
    int16_t  ltrigger;
    int16_t  rtrigger;
    int16_t  lstick_x;
    int16_t  lstick_y;
    int16_t  rstick_x;
    int16_t  rstick_y;
    uint32_t seq;
    uint64_t timestamp_us;
};

typedef enum {
    XBED_INPUT_SYNTH_OK = 0,
    XBED_INPUT_SYNTH_NO_AGENT,        /* anchor file absent / unreadable */
    XBED_INPUT_SYNTH_BAD_ANCHOR,      /* anchor parse / format error */
    XBED_INPUT_SYNTH_BAD_BUFFER,      /* kernel-pool magic/version mismatch */
    XBED_INPUT_SYNTH_E_MOUNT_FAILED,  /* could not mount E: drive */
} xbed_input_synth_status_t;

/* One-time initialization. Mounts E: if needed; reads the anchor file;
 * validates the buffer. Subsequent calls are idempotent. Returns
 * XBED_INPUT_SYNTH_OK on success. Caller should log non-OK return and
 * decide whether to fall back to a CPU-painted oracle. */
xbed_input_synth_status_t xbed_input_synth_attach(void);

/* Returns 1 if attach succeeded, 0 otherwise. */
int xbed_input_synth_attached(void);

/* Copy the latest state for `port` (0..3) into `out`. Returns 0 on
 * success, -1 if attach has not run / failed. The copy is seq-stamped
 * so a partial update from a concurrent Mac-side `controller.set`
 * yields a consistent snapshot (or a brief retry loop). */
int xbed_input_synth_read(uint32_t port, struct xbed_port_state *out);

/* Diagnostic accessors — useful for logging the wiring at startup. */
uintptr_t xbed_input_synth_phys_addr(void);
uintptr_t xbed_input_synth_virt_addr(void);
const char *xbed_input_synth_anchor_path(void);

#endif /* XBED_INPUT_SYNTH_H */
