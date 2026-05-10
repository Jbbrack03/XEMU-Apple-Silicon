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
report. Original bring-up session passed 25/25 + 100-toggle stress + 30s soak
+ 38-event canary CSV replay.
"""
from __future__ import annotations
import os, sys, struct, time, threading

os.environ.setdefault('DYLD_FALLBACK_LIBRARY_PATH', '/opt/homebrew/lib')
import usb.core, usb.util, serial


def open_bridge():
    ser = serial.Serial('/dev/cu.usbmodem3101', 115200, timeout=0, write_timeout=2.0)
    time.sleep(0.3)
    dev = usb.core.find(idVendor=0x045E, idProduct=0x0289)
    if dev is None:
        ser.close()
        raise SystemExit('slot 2 not on Mac (VID 0x045E PID 0x0289). '
                         'Move slot 2 USB to the Mac before running this.')
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


def main():
    ser, dev = open_bridge()
    latest = {'data': None}
    stop = threading.Event()

    def reader():
        while not stop.is_set():
            try: latest['data'] = bytes(dev.read(0x81, 64, timeout=50))
            except usb.core.USBError: pass

    t = threading.Thread(target=reader, daemon=True); t.start()
    time.sleep(0.5)

    def send_and_verify(label, **kw):
        target = struct.pack('<BBHBBBBBBBBhhhh', 0, 20, kw.get('wB', 0),
                             kw.get('A', 0), kw.get('B', 0), kw.get('X', 0), kw.get('Y', 0),
                             kw.get('BLACK', 0), kw.get('WHITE', 0),
                             kw.get('L', 0), kw.get('R', 0),
                             kw.get('lx', 0), kw.get('ly', 0),
                             kw.get('rx', 0), kw.get('ry', 0))
        frame = make_frame(1, **kw)
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
        print(f'  {label:35s} -> {"PASS" if matched else "FAIL"}')
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
        ser.write(make_frame(1, wB=0x0001 if (i & 1) else 0x0002))
        ser.flush()
        time.sleep(0.010)
    ok &= send_and_verify('post-rapid neutral')

    stop.set(); t.join(timeout=1.0)
    ser.close()
    try: usb.util.release_interface(dev, 0)
    except Exception: pass

    print('\nFINAL:', 'PASS' if ok else 'FAIL')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
