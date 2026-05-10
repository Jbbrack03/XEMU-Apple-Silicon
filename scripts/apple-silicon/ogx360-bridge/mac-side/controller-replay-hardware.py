#!/usr/bin/env python3
"""
controller-replay-hardware.py — replay a XEMU_RECORD_INPUT CSV through
the OGX360 bridge (custom slot 1 master firmware + unmodified Ryzee119
slave firmware).

Mac end-to-end path:
    CSV (xemu input recorder format)
      -> in-memory port state (16-bit xemu button mask + 6 axes)
      -> 20-byte usbd_duke_in_t bytes (Ryzee119 slave wire format)
      -> 25-byte framed serial packet (SYNC1 SYNC2 PORT TYPE PAYLOAD CKSUM)
      -> /dev/cu.usbmodemXXXX (USB CDC serial to slot 1 Pro Micro)
      -> I2C to slot 2 Pro Micro (Ryzee119 slave firmware, unmodified)
      -> XID HID to Xbox via slot 2's USB-C / Xbox-controller-port adapter

Vocabulary (button + axis names) intentionally identical to the existing
`scripts/apple-silicon/controller-replay.py` and `ui/xemu-input.c:101-127`
so the same input-script CSV library drives both engines.

USAGE
    controller-replay-hardware.py CSV [--device PATH] [--port-index N]
                                       [--rate-multiplier M]
                                       [--start-at-ms N] [--stop-at-ms N]
                                       [--time-origin {first-event,zero}]
                                       [--dry-run] [--clear-on-start]
                                       [--neutral-on-exit] [--list-devices]

DEVICE AUTODETECT
    --device defaults to the first /dev/cu.usbmodem* on macOS. The
    custom slot 1 firmware enumerates with the standard Arduino
    Leonardo VID/PID (0x2341:0x8036) so this is the obvious match.
    Pass --device explicitly if multiple Pro Micros are connected.

WIRE FORMAT (per the slot 1 firmware spec)
    [0xAB][0xCD][PORT 1..3][TYPE 1=DUKE][PAYLOAD 20 bytes][CKSUM XOR of bytes 2..23]

EXIT
    0 success
    1 transport failure (couldn't open serial / unplugged mid-run)
    2 bad CSV
"""
from __future__ import annotations

import argparse
import csv
import glob
import json
import os
import statistics
import struct
import sys
import time
from pathlib import Path
from typing import Dict, List, Optional, Tuple

_VENV_PY = Path(__file__).resolve().parent / ".venv/bin/python"
if _VENV_PY.exists() and Path(sys.executable) != _VENV_PY:
    os.execv(str(_VENV_PY), [str(_VENV_PY), *sys.argv])

try:
    import serial  # pyserial
except ImportError:
    print("ERROR: pyserial not installed. Install with: pip3 install pyserial",
          file=sys.stderr)
    sys.exit(1)


# Same vocabulary as ui/xemu-input.c:101-127 + controller-replay.py.
BUTTON_NAMES = (
    "a", "b", "x", "y",
    "dpad_left", "dpad_up", "dpad_right", "dpad_down",
    "back", "start", "white", "black",
    "lstick_btn", "rstick_btn", "guide",
)
AXIS_NAMES = (
    "ltrigger", "rtrigger",
    "lstick_x", "lstick_y", "rstick_x", "rstick_y",
)

# xemu CONTROLLER_BUTTON_* bit positions (from ui/xemu-input.h:42-56).
XEMU_BUTTON_BITS = {
    "a":          1 << 0,
    "b":          1 << 1,
    "x":          1 << 2,
    "y":          1 << 3,
    "dpad_left":  1 << 4,
    "dpad_up":    1 << 5,
    "dpad_right": 1 << 6,
    "dpad_down":  1 << 7,
    "back":       1 << 8,
    "start":      1 << 9,
    "white":      1 << 10,
    "black":      1 << 11,
    "lstick_btn": 1 << 12,
    "rstick_btn": 1 << 13,
    "guide":      1 << 14,
}

# Frame constants (must match firmware/master/master.ino).
FRAME_SYNC1 = 0xAB
FRAME_SYNC2 = 0xCD
FRAME_TYPE_DUKE = 1


class PortState:
    """In-memory state for one Xbox controller port."""

    __slots__ = ("buttons_mask", "axes")

    def __init__(self) -> None:
        self.buttons_mask: int = 0
        # Default axis values (centered sticks, zero triggers).
        self.axes: Dict[str, int] = {name: 0 for name in AXIS_NAMES}

    def set_button(self, name: str, value: int) -> None:
        bit = XEMU_BUTTON_BITS[name]
        if value:
            self.buttons_mask |= bit
        else:
            self.buttons_mask &= ~bit

    def set_axis(self, name: str, value: int) -> None:
        self.axes[name] = int(value)

    def to_duke_payload(self) -> bytes:
        """Pack current state as the 20-byte Ryzee119 usbd_duke_in_t.

        Mapping from xemu CONTROLLER_BUTTON_* mask -> Duke wButtons:
            DUKE_DUP    = bit 0  <- xemu dpad_up
            DUKE_DDOWN  = bit 1  <- xemu dpad_down
            DUKE_DLEFT  = bit 2  <- xemu dpad_left
            DUKE_DRIGHT = bit 3  <- xemu dpad_right
            DUKE_START  = bit 4  <- xemu start
            DUKE_BACK   = bit 5  <- xemu back
            DUKE_LS     = bit 6  <- xemu lstick_btn
            DUKE_RS     = bit 7  <- xemu rstick_btn
        """
        m = self.buttons_mask
        wbuttons = 0
        if m & XEMU_BUTTON_BITS["dpad_up"]:    wbuttons |= 0x01
        if m & XEMU_BUTTON_BITS["dpad_down"]:  wbuttons |= 0x02
        if m & XEMU_BUTTON_BITS["dpad_left"]:  wbuttons |= 0x04
        if m & XEMU_BUTTON_BITS["dpad_right"]: wbuttons |= 0x08
        if m & XEMU_BUTTON_BITS["start"]:      wbuttons |= 0x10
        if m & XEMU_BUTTON_BITS["back"]:       wbuttons |= 0x20
        if m & XEMU_BUTTON_BITS["lstick_btn"]: wbuttons |= 0x40
        if m & XEMU_BUTTON_BITS["rstick_btn"]: wbuttons |= 0x80
        # `guide` doesn't exist on the OG Xbox Duke; intentionally
        # unmapped. White/black are analog buttons in the Duke struct.

        # Analog buttons: in xemu these are binary; the Duke spec
        # accepts 0..255 pressure but treats >=0x40 as pressed. 0xFF
        # == "fully pressed" is the conventional emulator output.
        a     = 0xFF if (m & XEMU_BUTTON_BITS["a"])     else 0
        b     = 0xFF if (m & XEMU_BUTTON_BITS["b"])     else 0
        x     = 0xFF if (m & XEMU_BUTTON_BITS["x"])     else 0
        y     = 0xFF if (m & XEMU_BUTTON_BITS["y"])     else 0
        white = 0xFF if (m & XEMU_BUTTON_BITS["white"]) else 0
        black = 0xFF if (m & XEMU_BUTTON_BITS["black"]) else 0

        # Triggers: xemu encodes them as int16 0..32767 (per
        # ui/xemu-input.c). Duke trigger field is uint8 0..255. Scale
        # by /128 (right-shift 7) and clamp.
        l_trigger = max(0, min(255, self.axes["ltrigger"] >> 7))
        r_trigger = max(0, min(255, self.axes["rtrigger"] >> 7))

        # Sticks: xemu int16 -32768..32767 maps directly to Duke int16.
        lx = max(-32768, min(32767, self.axes["lstick_x"]))
        ly = max(-32768, min(32767, self.axes["lstick_y"]))
        rx = max(-32768, min(32767, self.axes["rstick_x"]))
        ry = max(-32768, min(32767, self.axes["rstick_y"]))

        # Pack: u8,u8,u16,8x u8,4x i16 = 20 bytes, packed little-endian.
        # Format: '<' = little-endian, no padding (struct rules; with
        # only u8/u16/i16 there's no inserted padding anyway).
        return struct.pack(
            "<BBHBBBBBBBBhhhh",
            0,                # startByte
            20,               # bLength (sizeof(usbd_duke_in_t))
            wbuttons,
            a, b, x, y,
            black, white,     # NOTE: Ryzee119 struct order is BLACK then WHITE
            l_trigger, r_trigger,
            lx, ly, rx, ry,
        )


def build_frame(port: int, state: PortState) -> bytes:
    """Build a 25-byte serial frame for one port's state."""
    if not (1 <= port <= 3):
        raise ValueError(f"port must be 1..3, got {port}")
    payload = state.to_duke_payload()
    assert len(payload) == 20
    body = bytes([port, FRAME_TYPE_DUKE]) + payload
    cksum = 0
    for b in body:
        cksum ^= b
    return bytes([FRAME_SYNC1, FRAME_SYNC2]) + body + bytes([cksum & 0xFF])


def parse_csv(path: Path) -> List[Tuple[int, str, int]]:
    """Read xemu's CSV format (`time_ms,control,value`)."""
    out: List[Tuple[int, str, int]] = []
    with path.open() as f:
        for line_no, raw in enumerate(f, start=1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            for sep in (",", "\t"):
                line = line.replace(sep, " ")
            parts = line.split()
            if len(parts) != 3:
                raise ValueError(
                    f"{path}:{line_no}: expected `time_ms,control,value`, got {raw!r}"
                )
            try:
                t = int(parts[0])
                v = int(parts[2])
            except ValueError as e:
                raise ValueError(f"{path}:{line_no}: integer parse: {raw!r}") from e
            ctrl = parts[1].lower()
            if ctrl not in BUTTON_NAMES and ctrl not in AXIS_NAMES:
                raise ValueError(
                    f"{path}:{line_no}: unknown control '{ctrl}'"
                )
            out.append((t, ctrl, v))
    out.sort(key=lambda r: r[0])
    return out


def autodetect_device() -> Optional[str]:
    """Return the first /dev/cu.usbmodem* on macOS, or None."""
    candidates = sorted(glob.glob("/dev/cu.usbmodem*"))
    return candidates[0] if candidates else None


def list_devices() -> None:
    """Print all candidate serial devices."""
    candidates = sorted(glob.glob("/dev/cu.usbmodem*") +
                        glob.glob("/dev/cu.usbserial*") +
                        glob.glob("/dev/tty.usbmodem*"))
    if not candidates:
        print("(no /dev/cu.usbmodem* or /dev/cu.usbserial* devices found)")
        return
    for path in candidates:
        print(path)


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("csv", nargs="?",
                    help="Path to xemu input CSV. Required unless --list-devices.")
    ap.add_argument("--device", default=None,
                    help="Serial device path. Default: autodetect /dev/cu.usbmodem*")
    ap.add_argument("--port-index", type=int, default=1,
                    help="I2C slave address / Xbox port (1..3); default 1 "
                         "(matches typical OGX360 slot 2 jumper)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--rate-multiplier", type=float, default=1.0,
                    help="Time-warp the replay (e.g. 2.0 = 2x speed); default 1.0")
    ap.add_argument("--start-at-ms", type=int, default=0)
    ap.add_argument("--stop-at-ms", type=int, default=None)
    ap.add_argument("--time-origin", choices=("first-event", "zero"),
                    default="first-event",
                    help="Timing origin for replay. Default first-event preserves "
                         "legacy behavior. Use zero for retail-oracle runs so "
                         "xemu-recorded startup delays remain intact.")
    ap.add_argument("--dry-run", action="store_true",
                    help="Parse + simulate; do not open serial or send frames.")
    ap.add_argument("--clear-on-start", action="store_true",
                    help="Send a neutral state frame before replay.")
    ap.add_argument("--neutral-on-exit", action="store_true",
                    help="Send and flush a neutral controller frame before closing.")
    ap.add_argument("--list-devices", action="store_true",
                    help="Print candidate serial devices and exit.")
    ap.add_argument("--frame-rate-hz", type=float, default=250.0,
                    help="Maximum serial frame rate during replay; default 250 Hz "
                         "(matches the firmware's I2C cadence).")
    args = ap.parse_args(argv)

    if args.list_devices:
        list_devices()
        return 0

    if not args.csv:
        ap.error("CSV argument required (or use --list-devices)")

    csv_path = Path(args.csv).resolve()
    if not csv_path.exists():
        print(f"csv not found: {csv_path}", file=sys.stderr)
        return 2
    try:
        events = parse_csv(csv_path)
    except ValueError as e:
        print(f"parse error: {e}", file=sys.stderr)
        return 2

    if not events:
        print("[replay] no events to replay", file=sys.stderr)
        return 0

    if args.start_at_ms > 0:
        events = [e for e in events if e[0] >= args.start_at_ms]
    if args.stop_at_ms is not None:
        events = [e for e in events if e[0] <= args.stop_at_ms]
    if not events:
        print("[replay] all events filtered out by --start/--stop", file=sys.stderr)
        return 0

    port_idx = args.port_index
    if not (1 <= port_idx <= 3):
        print(f"--port-index must be 1..3, got {port_idx}", file=sys.stderr)
        return 2

    device_path = args.device or autodetect_device()
    if not args.dry_run and not device_path:
        print("ERROR: no /dev/cu.usbmodem* device found. Plug the slot 1 "
              "Pro Micro into the Mac, or pass --device PATH.", file=sys.stderr)
        return 1

    state = PortState()

    base_t = 0 if args.time_origin == "zero" else events[0][0]
    rate = max(args.rate_multiplier, 0.001)
    min_frame_interval = 1.0 / max(args.frame_rate_hz, 1.0)

    print(f"[replay] csv      : {csv_path}", file=sys.stderr)
    print(f"[replay] events   : {len(events)} "
          f"(origin={args.time_origin}; t={events[0][0]}..{events[-1][0]} ms; "
          f"rate={rate}x)", file=sys.stderr)
    print(f"[replay] device   : {device_path or '(dry-run)'}", file=sys.stderr)
    print(f"[replay] port_idx : {port_idx}", file=sys.stderr)
    if args.dry_run:
        print("[replay] DRY RUN — not opening serial port", file=sys.stderr)

    ser = None
    if not args.dry_run:
        try:
            ser = serial.Serial(device_path, args.baud, timeout=0,
                                write_timeout=2.0)
            # Some Mac drivers require a moment after open before
            # writes get through reliably.
            time.sleep(0.2)
        except (serial.SerialException, OSError) as e:
            print(f"[replay] cannot open {device_path}: {e}", file=sys.stderr)
            return 1

    if args.clear_on_start and ser is not None:
        try:
            ser.write(build_frame(port_idx, PortState()))
            ser.flush()
        except (serial.SerialException, OSError) as e:
            print(f"[replay] clear failed: {e}", file=sys.stderr)
            ser.close()
            return 1

    started_wall = time.monotonic()
    last_frame_sent = 0.0
    sent = 0
    schedule_jitters_ms: List[float] = []
    errors: List[str] = []
    last_progress_log = 0

    def maybe_send_frame(force: bool = False) -> None:
        nonlocal sent, last_frame_sent
        now = time.monotonic()
        if not force and (now - last_frame_sent) < min_frame_interval:
            return
        if ser is None:
            sent += 1
            last_frame_sent = now
            return
        try:
            ser.write(build_frame(port_idx, state))
            sent += 1
            last_frame_sent = now
        except (serial.SerialException, OSError) as e:
            errors.append(f"write failed: {e}")

    def sleep_until(target_wall: float) -> None:
        """Sleep in short slices so the bridge keeps seeing neutral/current
        state frames during long startup gaps in xemu-recorded routes."""
        while True:
            now = time.monotonic()
            if target_wall <= now:
                return
            maybe_send_frame()
            time.sleep(min(target_wall - now, min_frame_interval))

    try:
        for idx, (t_ms, ctrl, val) in enumerate(events):
            target_wall = started_wall + (t_ms - base_t) / 1000.0 / rate
            sleep_until(target_wall)
            schedule_actual = time.monotonic()
            schedule_jitters_ms.append((schedule_actual - target_wall) * 1000.0)

            # Apply this event to in-memory state, then push a frame.
            if ctrl in BUTTON_NAMES:
                if ctrl == "guide":
                    # Not present on Duke; latch in mask but don't emit
                    # a wire bit. Useful if other events depend on
                    # guide state in the future.
                    pass
                state.set_button(ctrl, val)
            else:
                state.set_axis(ctrl, val)

            # Force a frame on every event so a single-button-tap is
            # never lost to rate limiting.
            maybe_send_frame(force=True)

            if idx - last_progress_log >= 500:
                last_progress_log = idx
                elapsed = time.monotonic() - started_wall
                avg = statistics.mean(schedule_jitters_ms[-500:])
                print(f"[replay] {idx + 1}/{len(events)} events "
                      f"({elapsed:.1f}s wall; avg jitter {avg:.2f} ms)",
                      file=sys.stderr)
        if args.neutral_on_exit:
            state = PortState()
            maybe_send_frame(force=True)
        # Final flush.
        if ser is not None:
            ser.flush()
    finally:
        if ser is not None:
            ser.close()

    elapsed_total = time.monotonic() - started_wall

    def jitter_summary(samples: List[float]) -> dict:
        if not samples:
            return {"count": 0, "mean": None, "median": None,
                    "max": None, "p95": None}
        s = sorted(samples)
        return {
            "count": len(samples),
            "mean": round(statistics.mean(samples), 3),
            "median": round(statistics.median(samples), 3),
            "max": round(max(samples), 3),
            "p95": round(s[int(len(s) * 0.95)], 3) if len(s) >= 20 else None,
        }

    summary = {
        "csv": str(csv_path),
        "device": device_path,
        "port_index": port_idx,
        "events_total": len(events),
        "frames_sent": sent,
        "wall_elapsed_s": round(elapsed_total, 3),
        "rate_multiplier": rate,
        "time_origin": args.time_origin,
        "neutral_on_exit": args.neutral_on_exit,
        "schedule_jitter_ms": jitter_summary(schedule_jitters_ms),
        "errors_first_5": errors[:5],
        "dry_run": args.dry_run,
    }
    print(json.dumps(summary, indent=2))
    return 0 if not errors else 1


if __name__ == "__main__":
    sys.exit(main())
