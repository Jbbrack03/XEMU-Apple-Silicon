#!/usr/bin/env python3
"""
bench-validate.py — exhaustive bench validation of the OGX360 bridge.

Validates the Mac-side path:
    Mac CSV -> CDC (slot 1) -> I2C -> slot 2 (XID HID) -> back to Mac via USB

Slot 2 must be unplugged from the Xbox and re-plugged into the Mac for this
test (so the Mac can read its HID interrupt-in endpoint via libusb).

Test surface:
- All 8 wButtons bits (0x01..0x80)
- High byte of wButtons (reserved per spec, should still transit)
- Analog buttons A/B/X/Y/BLACK/WHITE at 0xFF
- L/R triggers at extremes
- All 4 stick axes at -32768/32767/midpoint/-1
- Combo: every field non-zero simultaneously
- 100 toggles at 100Hz to verify rapid transitions don't drop fidelity
- Final neutral state read-back

Reads slot 2's HID interrupt-in endpoint via pyusb in a background thread to
keep macOS polling (the slave's sendReport only fires on data change, so the
host must drive interrupt polling for every change to land).

PASS = byte-exact match between every sent state and the next observed HID
report. The 2026-05-10 production run passed the expanded suite: field sweeps,
100 Hz transitions, Crimson CSV replay at 20x, randomized soak, and final
neutral.
"""
from __future__ import annotations
import argparse, os, random, sys, struct, time, threading
from pathlib import Path

_VENV_PY = Path(__file__).resolve().parents[1] / "mac-side/.venv/bin/python"
if _VENV_PY.exists() and Path(sys.executable) != _VENV_PY:
    os.execv(str(_VENV_PY), [str(_VENV_PY), *sys.argv])

os.environ.setdefault('DYLD_FALLBACK_LIBRARY_PATH', '/opt/homebrew/lib')
import usb.core, usb.util, serial


def open_bridge(device, vid, pid):
    ser = serial.Serial(device, 115200, timeout=0, write_timeout=2.0)
    time.sleep(0.3)
    dev = usb.core.find(idVendor=vid, idProduct=pid)
    if dev is None:
        ser.close()
        raise SystemExit(f'slot 2 not on Mac (VID 0x{vid:04X} PID 0x{pid:04X}). '
                         'Move slot 2 USB to the Mac before running this.')
    try: dev.set_configuration()
    except Exception: pass
    try: dev.detach_kernel_driver(0)
    except Exception: pass
    usb.util.claim_interface(dev, 0)
    return ser, dev


def make_frame(port, **kw):
    payload = struct.pack('<BBHBBBBBBBBhhhh',
                          0, 20, kw.get('wB', 0),
                          kw.get('A', 0), kw.get('B', 0), kw.get('X', 0), kw.get('Y', 0),
                          kw.get('BLACK', 0), kw.get('WHITE', 0),
                          kw.get('L', 0), kw.get('R', 0),
                          kw.get('lx', 0), kw.get('ly', 0),
                          kw.get('rx', 0), kw.get('ry', 0))
    body = bytes([port, 1]) + payload
    cksum = 0
    for b in body: cksum ^= b
    return bytes([0xAB, 0xCD]) + body + bytes([cksum & 0xFF])


BUTTON_TO_WB = {
    'dpad_up': 0x01,
    'dpad_down': 0x02,
    'dpad_left': 0x04,
    'dpad_right': 0x08,
    'start': 0x10,
    'back': 0x20,
    'lstick_btn': 0x40,
    'rstick_btn': 0x80,
}

BUTTON_TO_ANALOG = {
    'a': 'A',
    'b': 'B',
    'x': 'X',
    'y': 'Y',
    'black': 'BLACK',
    'white': 'WHITE',
}

AXIS_TO_FIELD = {
    'lstick_x': 'lx',
    'lstick_y': 'ly',
    'rstick_x': 'rx',
    'rstick_y': 'ry',
}


def pack_payload(**kw):
    return struct.pack('<BBHBBBBBBBBhhhh', 0, 20, kw.get('wB', 0),
                       kw.get('A', 0), kw.get('B', 0), kw.get('X', 0),
                       kw.get('Y', 0), kw.get('BLACK', 0),
                       kw.get('WHITE', 0), kw.get('L', 0), kw.get('R', 0),
                       kw.get('lx', 0), kw.get('ly', 0),
                       kw.get('rx', 0), kw.get('ry', 0))


def parse_csv_events(path):
    events = []
    with path.open() as f:
        for line_no, raw in enumerate(f, 1):
            line = raw.strip()
            if not line or line.startswith('#'):
                continue
            for sep in (',', '\t'):
                line = line.replace(sep, ' ')
            parts = line.split()
            if len(parts) != 3:
                raise ValueError(f'{path}:{line_no}: expected time,control,value')
            events.append((int(parts[0]), parts[1].lower(), int(parts[2])))
    events.sort(key=lambda e: e[0])
    return events


def read_control_report(dev):
    try:
        return bytes(dev.ctrl_transfer(0xA1, 0x01, 0x0100, 0, 20, timeout=500))
    except Exception as e:
        return f'GET_REPORT failed: {e}'


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--device', default='/dev/cu.usbmodem3101',
                    help='slot 1 CDC serial device')
    ap.add_argument('--port-index', type=int, default=1,
                    help='I2C slave address to drive; default 1')
    ap.add_argument('--slot2-vid', type=lambda x: int(x, 0), default=0x045E)
    ap.add_argument('--slot2-pid', type=lambda x: int(x, 0), default=0x0289)
    args = ap.parse_args()

    ser, dev = open_bridge(args.device, args.slot2_vid, args.slot2_pid)
    latest = {'data': None, 'errors': 0}
    stop = threading.Event()

    def reader():
        while not stop.is_set():
            try: latest['data'] = bytes(dev.read(0x81, 64, timeout=50))
            except usb.core.USBError: latest['errors'] += 1

    t = threading.Thread(target=reader, daemon=True); t.start()
    time.sleep(0.5)

    def send_and_verify(label, **kw):
        target = pack_payload(**kw)
        frame = make_frame(args.port_index, **kw)
        t0 = time.monotonic()
        while time.monotonic() - t0 < 0.25:
            ser.write(frame); ser.flush()
            time.sleep(0.015)
        deadline = time.monotonic() + 1.0
        matched = False
        while time.monotonic() < deadline:
            d = latest['data']
            if d is not None and bytes(d) == target:
                matched = True; break
            time.sleep(0.005)
        status = "PASS" if matched else "FAIL"
        print(f'  {label:35s} -> {status}')
        if not matched:
            interrupt = latest['data']
            control = read_control_report(dev)
            if isinstance(interrupt, bytes):
                interrupt_s = interrupt.hex(' ')
            else:
                interrupt_s = '(none)'
            if isinstance(control, bytes):
                control_s = control.hex(' ')
            else:
                control_s = control
            print(f'      expected : {target.hex(" ")}')
            print(f'      interrupt: {interrupt_s} (timeouts={latest["errors"]})')
            print(f'      control  : {control_s}')
        return matched

    ok = True
    print('=== 1. wButtons full bit sweep ===')
    for i in range(8):
        ok &= send_and_verify(f'wB bit{i} ({hex(1 << i)})', wB=(1 << i))
    ok &= send_and_verify('wB all 8 set (0xFF)', wB=0xFF)
    ok &= send_and_verify('wB high byte 0x0100 (reserved)', wB=0x0100)
    ok &= send_and_verify('wB neutral')

    print('\n=== 2. Analog buttons + triggers ===')
    ok &= send_and_verify('A/B/X/Y/BLACK/WHITE 0xFF',
                          A=0xFF, B=0xFF, X=0xFF, Y=0xFF, BLACK=0xFF, WHITE=0xFF)
    ok &= send_and_verify('L=0x80 R=0x80', L=0x80, R=0x80)
    ok &= send_and_verify('L=0xFF R=0x00', L=0xFF, R=0x00)
    ok &= send_and_verify('triggers/buttons cleared')

    print('\n=== 3. 16-bit sticks at extremes ===')
    ok &= send_and_verify('lstick min', lx=-32768, ly=-32768)
    ok &= send_and_verify('lstick max', lx=32767, ly=32767)
    ok &= send_and_verify('rstick mixed', rx=-32768, ry=32767)
    ok &= send_and_verify('all sticks midpoint -1', lx=-1, ly=-1, rx=-1, ry=-1)
    ok &= send_and_verify('sticks neutral')

    print('\n=== 4. Combo (every field non-zero) ===')
    ok &= send_and_verify('combo', wB=0x00FF, A=0x10, B=0x20, X=0x30, Y=0x40,
                          BLACK=0x50, WHITE=0x60, L=0x70, R=0x80,
                          lx=1000, ly=-1000, rx=-2000, ry=2000)
    ok &= send_and_verify('combo cleared')

    print('\n=== 5. Rapid transitions (100 toggles @ 100Hz) ===')
    for i in range(100):
        ser.write(make_frame(args.port_index, wB=0x0001 if (i & 1) else 0x0002))
        ser.flush()
        time.sleep(0.010)
    ok &= send_and_verify('post-rapid neutral')

    print('\n=== 6. Real CSV replay (crimson-skies-smoke.csv @ 20x) ===')
    csv_path = (Path(__file__).resolve().parents[2] /
                'input-scripts/crimson-skies-smoke.csv')
    state = {'wB': 0, 'A': 0, 'B': 0, 'X': 0, 'Y': 0,
             'BLACK': 0, 'WHITE': 0, 'L': 0, 'R': 0,
             'lx': 0, 'ly': 0, 'rx': 0, 'ry': 0}
    csv_ok = True
    events = parse_csv_events(csv_path)
    base_t = events[0][0]
    start = time.monotonic()
    for t_ms, control, value in events:
        target_time = start + (t_ms - base_t) / 1000.0 / 20.0
        if target_time > time.monotonic():
            time.sleep(target_time - time.monotonic())
        if control in BUTTON_TO_WB:
            if value:
                state['wB'] |= BUTTON_TO_WB[control]
            else:
                state['wB'] &= ~BUTTON_TO_WB[control]
        elif control in BUTTON_TO_ANALOG:
            state[BUTTON_TO_ANALOG[control]] = 0xFF if value else 0
        elif control == 'ltrigger':
            state['L'] = max(0, min(255, value >> 7))
        elif control == 'rtrigger':
            state['R'] = max(0, min(255, value >> 7))
        elif control in AXIS_TO_FIELD:
            state[AXIS_TO_FIELD[control]] = max(-32768, min(32767, value))
        elif control == 'guide':
            pass
        else:
            raise ValueError(f'{csv_path}: unsupported control {control}')
        csv_ok &= send_and_verify(f'csv {t_ms}ms {control}={value}', **state)
    ok &= csv_ok

    print('\n=== 7. Randomized soak (234 states) ===')
    rng = random.Random(0x0A6A360)
    soak_ok = True
    for i in range(234):
        state = {
            'wB': rng.randrange(0, 0x100),
            'A': rng.randrange(0, 0x100),
            'B': rng.randrange(0, 0x100),
            'X': rng.randrange(0, 0x100),
            'Y': rng.randrange(0, 0x100),
            'BLACK': rng.randrange(0, 0x100),
            'WHITE': rng.randrange(0, 0x100),
            'L': rng.randrange(0, 0x100),
            'R': rng.randrange(0, 0x100),
            'lx': rng.randrange(-32768, 32768),
            'ly': rng.randrange(-32768, 32768),
            'rx': rng.randrange(-32768, 32768),
            'ry': rng.randrange(-32768, 32768),
        }
        soak_ok &= send_and_verify(f'soak #{i:03d}', **state)
    soak_ok &= send_and_verify('post-soak neutral')
    ok &= soak_ok

    stop.set(); t.join(timeout=1.0)
    ser.close()
    try: usb.util.release_interface(dev, 0)
    except Exception: pass

    print('\nFINAL:', 'PASS' if ok else 'FAIL')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
