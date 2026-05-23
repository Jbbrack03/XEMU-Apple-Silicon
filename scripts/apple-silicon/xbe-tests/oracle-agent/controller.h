/*
 * xbox-oracle-agent — synthetic controller state buffer + RPC commands.
 *
 * THIS LAYER IS PHASE 1 (PROTOCOL-ONLY).
 *   The agent maintains an in-process synthetic input state for up to
 *   four Xbox controller ports. Mac-side clients (controller-replay.py)
 *   write into it via the controller.* RPCs and read it back via
 *   `controller.get`. The state is published with a deterministic
 *   in-memory layout so two follow-on consumers can attach to it
 *   without an RPC round-trip:
 *
 *     1. a diag-XBE shim that links against this header and reads the
 *        same struct out of agent-allocated kernel memory before each
 *        rendered frame (Tier 1 — works only inside our diag XBEs);
 *
 *     2. a future kernel-mode hook on XInputGetState that returns the
 *        synthetic state to retail games (Tier 2 — out of scope for
 *        the current session, see
 *        docs/apple-silicon/controller-injection-research.md).
 *
 *   Phase 1 ships the state buffer + RPCs. Neither (1) nor (2) is
 *   implemented yet; the agent just owns the canonical state struct.
 *
 * Wire-protocol commands (registered in main.c):
 *   controller.set port=N [buttons=0xHHHH] [lt=N] [rt=N]
 *                  [lx=N] [ly=N] [rx=N] [ry=N]
 *     Update one or more fields for port N (0..3). Missing keys are
 *     left at their current values. `seq` increments on every call.
 *
 *   controller.button port=N name=<id> value=<0|1>
 *     Set/clear a single named button (xemu vocabulary: a, b, x, y,
 *     dpad_left, dpad_up, dpad_right, dpad_down, back, start, white,
 *     black, lstick_btn, rstick_btn, guide).
 *
 *   controller.axis port=N name=<id> value=<int>
 *     Set a named axis (ltrigger, rtrigger, lstick_x, lstick_y,
 *     rstick_x, rstick_y). Triggers are clamped to int16 0..32767
 *     (xemu axis range; XID HID-report u8 is produced by `>> 7` at
 *     the eventual shim/hook layer). Sticks are clamped int16
 *     -32768..32767.
 *
 *   controller.get [port=N]
 *     Return the synthetic state for port N (or all four ports if
 *     port= is omitted), as a 201-OK multi-line text block keyed by
 *     `port.<n>.<field>`.
 *
 *   controller.clear [port=N]
 *     Zero one or all ports (buttons=0, axes=0, seq incremented).
 *
 *   controller.buffer-info
 *     Return the synthetic-state buffer's virtual address + size +
 *     ABI version. A diag-XBE shim or kernel hook can use this to
 *     locate the buffer at runtime.
 *
 * Button bit layout matches xemu's CONTROLLER_BUTTON_* enum so the
 * Mac-side replay tool can pass values straight through. The numeric
 * values are reproduced here (not pulled from an Xbox header) so the
 * agent stays self-contained.
 */
#ifndef ORACLE_CONTROLLER_H
#define ORACLE_CONTROLLER_H

#include <lwip/api.h>
#include <stdint.h>

#define ORACLE_CTRL_NUM_PORTS 4
#define ORACLE_CTRL_MAGIC     0x58435452u   /* 'XCTR' little-endian */
#define ORACLE_CTRL_VERSION   1u

/* Bit layout — VALUES lifted verbatim from xemu's
 * `enum controller_state_buttons_mask` at `ui/xemu-input.h:41-57`.
 * Keeping the values identical means a XEMU_RECORD_INPUT CSV
 * round-trips through `controller.set port=N buttons=0xHHHH`
 * without any per-bit translation: the buttons field on this
 * buffer IS the same field xemu's ControllerState.buttons holds,
 * with the same bit numbering. */
#define ORACLE_BTN_A          (1u << 0)   /* matches CONTROLLER_BUTTON_A */
#define ORACLE_BTN_B          (1u << 1)   /* CONTROLLER_BUTTON_B */
#define ORACLE_BTN_X          (1u << 2)   /* CONTROLLER_BUTTON_X */
#define ORACLE_BTN_Y          (1u << 3)   /* CONTROLLER_BUTTON_Y */
#define ORACLE_BTN_DPAD_LEFT  (1u << 4)   /* CONTROLLER_BUTTON_DPAD_LEFT */
#define ORACLE_BTN_DPAD_UP    (1u << 5)   /* CONTROLLER_BUTTON_DPAD_UP */
#define ORACLE_BTN_DPAD_RIGHT (1u << 6)   /* CONTROLLER_BUTTON_DPAD_RIGHT */
#define ORACLE_BTN_DPAD_DOWN  (1u << 7)   /* CONTROLLER_BUTTON_DPAD_DOWN */
#define ORACLE_BTN_BACK       (1u << 8)   /* CONTROLLER_BUTTON_BACK */
#define ORACLE_BTN_START      (1u << 9)   /* CONTROLLER_BUTTON_START */
#define ORACLE_BTN_WHITE      (1u << 10)  /* CONTROLLER_BUTTON_WHITE */
#define ORACLE_BTN_BLACK      (1u << 11)  /* CONTROLLER_BUTTON_BLACK */
#define ORACLE_BTN_LSTICK     (1u << 12)  /* CONTROLLER_BUTTON_LSTICK */
#define ORACLE_BTN_RSTICK     (1u << 13)  /* CONTROLLER_BUTTON_RSTICK */
#define ORACLE_BTN_GUIDE      (1u << 14)  /* CONTROLLER_BUTTON_GUIDE (xemu ext.) */
/* bit 15 reserved */

/* Sequence-protocol convention (Codex 2026-05-07).
 *
 * Writers (cmd_controller_set/button/axis/clear in this file) update
 * the seq field as a "seqlock":
 *
 *   1. seq = previous_seq + 1   (now ODD; "in-flight" marker)
 *   2. <write all fields>
 *   3. seq = previous_seq + 2   (now EVEN; "stable" marker)
 *
 * Readers (cmd_controller_get; xbed_input_synth_read; future
 * kernel-mode hook) accept the snapshot only when the pre-copy and
 * post-copy `seq` values are equal AND even. An odd or mismatched
 * pre/post pair indicates an in-flight write; the reader retries
 * (or, on its last retry, returns the most-recent stable value
 * captured in an earlier iteration).
 *
 * Initial value is 0 (even, no writes yet). seq overflows on wrap;
 * the wrap is harmless because both sides only test equality and
 * parity. */
#define ORACLE_CTRL_SEQ_BUSY(s) (((s) & 1u) != 0)

/* One port's synthetic state. 26 bytes. Packed so the on-the-wire
 * binary format and the in-memory layout agree byte-for-byte across
 * the agent and any future shim.
 *
 * Field ABI matches xemu's `ControllerState.buttons` + `axis[]` (see
 * `ui/xemu-input.h:41-69` and `ui/xemu-input.h:90-91`):
 *   - buttons: 16-bit OR of ORACLE_BTN_* (== CONTROLLER_BUTTON_*) bits
 *   - {l,r}trigger: int16 in xemu's range 0..32767 (NOT the Xbox XID
 *     HID-report u8 0..255 — XID reports are produced from these by
 *     `>> 7` at hw/xbox/xid.c:108-109; future shim does the same).
 *   - {l,r}stick_{x,y}: int16 -32768..32767 (matches xemu directly,
 *     and matches XID's sShortLX/Y exactly).
 * This means a XEMU_RECORD_INPUT CSV row replays through
 * `controller.set port=N buttons=… lt=… rt=… lx=… …` without any
 * value-domain translation: the buffer field IS the same field xemu
 * fills in. */
struct __attribute__((packed)) oracle_ctrl_port_state {
    uint16_t buttons;       /* OR of ORACLE_BTN_* bits */
    int16_t  ltrigger;      /* 0..32767 (xemu axis range; XID = >> 7) */
    int16_t  rtrigger;
    int16_t  lstick_x;      /* -32768..32767 */
    int16_t  lstick_y;
    int16_t  rstick_x;
    int16_t  rstick_y;
    uint32_t seq;           /* monotonic per-port; bumped on every change */
    uint64_t timestamp_us;  /* xboxkrnl KeQueryPerformanceCounter at write */
};

/* Top-level synthetic-input state. 16 + 4*26 = 120 bytes. Lives in a
 * persistent kernel-pool allocation (MmAllocateContiguousMemoryEx +
 * MmPersistContiguousMemory) so the buffer survives the agent's own
 * process death across an `XLaunchXBE` chainload. The physical address
 * of the allocation is stored in
 * `E:\Apps\oracle-agent\state\ctrl-addr.txt`; a chainloaded diag XBE
 * (`xbe-tests/lib/xbed_input_synth.c`) reads that file on startup,
 * maps the same physical address via the kseg0 identity map (virtual
 * = physical | 0x80000000), and reads this struct directly to consume
 * synthetic controller input set by Mac-side controller.set RPCs. */
struct __attribute__((packed)) oracle_ctrl_buffer {
    uint32_t magic;         /* ORACLE_CTRL_MAGIC */
    uint32_t version;       /* ORACLE_CTRL_VERSION */
    uint32_t reserved[2];
    struct oracle_ctrl_port_state port[ORACLE_CTRL_NUM_PORTS];
};

/* Path of the persistence anchor file. Diag XBEs use the same path
 * to discover the buffer's physical address across an XLaunchXBE
 * chainload. Format: "XCTR\n0x<HEX_PHYS_ADDR>\n0x<HEX_VIRT_ADDR>\n". */
#define ORACLE_CTRL_ADDR_FILE "E:\\Apps\\oracle-agent\\state\\ctrl-addr.txt"

/* Public accessor — returns the (kernel-pool) virtual base address of
 * the controller buffer, or NULL if init has not run / failed. */
struct oracle_ctrl_buffer *oracle_ctrl_get(void);

/* Initialize the controller-state buffer:
 *   Allocate a fresh persistent contiguous page, write the persistence
 *   anchor, and reset the port state. A previous opt-in cross-restart
 *   reattach build was removed because the anchor-recorded physical
 *   page was not a sufficient allocator-ownership proof for production.
 *
 *   Cycle 27 option (a) refinement: if the kernel pool returns a page
 *   whose first 16 bytes already match a plausible `oracle_ctrl_buffer`
 *   witness header (XCTR magic + version 1 + reserved[0] either 0 or
 *   A.4-tagged + reserved[1] within the cycle-23 plausibility ceiling),
 *   preserve the header fields (magic / version / reserved[0,1]) and
 *   ONLY clear the `port[]` payload. Otherwise zero the full buffer and
 *   re-stamp magic / version (legacy behavior). This keeps the witness
 *   header that a chainloaded diagnostic XBE (cycle-25 `witness-only`,
 *   future variants) may have landed on the same kernel-pool page
 *   visible to a subsequent `witness.scan` after agent restart, which
 *   is the cycle-25/26 stamp-vs-no-stamp discriminator that cycle 27
 *   exists to break. */
void oracle_ctrl_init(void);

/* RPC handlers — same shape as cmd_*. */
int cmd_controller_set(struct netconn *c, const char *args);
int cmd_controller_get(struct netconn *c, const char *args);
int cmd_controller_button(struct netconn *c, const char *args);
int cmd_controller_axis(struct netconn *c, const char *args);
int cmd_controller_clear(struct netconn *c, const char *args);
int cmd_controller_buffer_info(struct netconn *c, const char *args);

#endif /* ORACLE_CONTROLLER_H */
