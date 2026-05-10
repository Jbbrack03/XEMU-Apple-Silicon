#!/usr/bin/env python3
"""Drive the OGX360 bridge while running controller-readback on the Xbox."""
from __future__ import annotations

import argparse, os, sys, struct, time, threading, ftplib, io
from pathlib import Path

_VENV_PY = Path(__file__).resolve().parents[1] / "mac-side/.venv/bin/python"
if _VENV_PY.exists() and Path(sys.executable) != _VENV_PY:
    os.execv(str(_VENV_PY), [str(_VENV_PY), *sys.argv])

sys.path.insert(0, '/Users/jbbrack03/XEMU_MacOS/xemu-fork/scripts/apple-silicon')
import serial

# Use ONLY two distinctive bits; check for them precisely
STATE = dict(wB=0x0008,            # DRIGHT only
             A=0xAA, B=0,X=0,Y=0,
             BLACK=0,WHITE=0,L=0,R=0,
             lx=25000, ly=0, rx=0, ry=0)

def make_frame(port, wB, A,B,X,Y, BLACK,WHITE, L,R, lx,ly,rx,ry):
    payload = struct.pack('<BBHBBBBBBBBhhhh', 0,20,wB, A,B,X,Y, BLACK,WHITE, L,R, lx,ly,rx,ry)
    body = bytes([port, 1]) + payload
    cksum = 0
    for b in body: cksum ^= b
    return bytes([0xAB, 0xCD]) + body + bytes([cksum & 0xFF])

def sender(device, stop, sent, current_frame):
    ser = serial.Serial(device, 115200, timeout=0, write_timeout=2.0)
    time.sleep(0.2)
    while not stop.is_set():
        try:
            ser.write(current_frame[0]); ser.flush()
            sent[0] += 1
        except Exception as e:
            print(f'  [sender] err: {e}', file=sys.stderr); break
        time.sleep(0.005)  # 200Hz
    try:
        ser.write(make_frame(1, 0, 0,0,0,0, 0,0, 0,0, 0,0,0,0)); ser.flush()
    except Exception: pass
    ser.close()

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--device', default='/dev/cu.usbmodem3101',
                    help='slot 1 CDC serial device')
    ap.add_argument('--host', default=os.environ.get('ORACLE_HOST', '192.168.0.200'))
    ap.add_argument('--xbe-path', default=r'E:\Apps\controller-readback\default.xbe')
    ap.add_argument('--report-remote', default='/E/Apps/controller-readback/controller-readback.txt')
    ap.add_argument('--warmup-s', type=float, default=3.0)
    ap.add_argument('--hold-s', type=float, default=18.0)
    ap.add_argument('--transition-s', type=float, default=6.0,
                    help='toggle target/neutral after chainload so the Xbox '
                         'receives fresh interrupt reports')
    args = ap.parse_args()

    stop = threading.Event()
    sent = [0]
    target = make_frame(1, **STATE)
    neutral = make_frame(1, 0, 0,0,0,0, 0,0, 0,0, 0,0,0,0)
    current_frame = [neutral]

    # Start neutral before chainload. After the XBE starts, deliberately
    # transition/toggle so the Xbox host receives fresh interrupt reports.
    print('[host] sender starting at 200Hz neutral')
    t = threading.Thread(target=sender,
                         args=(args.device, stop, sent, current_frame),
                         daemon=True)
    t.start()
    time.sleep(args.warmup_s)
    print(f'[host] sender warmed up, sent so far: {sent[0]} frames')

    print('[host] chainloading XBE')
    ftp = ftplib.FTP(args.host, timeout=10)
    ftp.login('xbox','xbox')
    print('  ', ftp.sendcmd('SITE EXEC ' + args.xbe_path))
    try: ftp.quit()
    except: pass

    print(f'[host] toggling target/neutral for {args.transition_s:g}s')
    t0 = time.monotonic()
    while time.monotonic() - t0 < args.transition_s:
        phase = int((time.monotonic() - t0) / 0.05)
        current_frame[0] = target if (phase & 1) else neutral
        time.sleep(0.005)

    current_frame[0] = target
    print(f'[host] holding target for {args.hold_s:g}s')
    t0 = time.monotonic()
    while time.monotonic() - t0 < args.hold_s:
        time.sleep(0.5)

    print(f'[host] stopping sender (sent={sent[0]} frames)')
    stop.set(); t.join(timeout=2)

    print('[host] waiting for Xbox FTP to return...')
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        try:
            ftp = ftplib.FTP(args.host, timeout=4)
            ftp.login('xbox','xbox'); ftp.quit(); break
        except Exception: time.sleep(2)
    else:
        print('  Xbox did not return'); return 3
    print('[host] Xbox back')
    time.sleep(1)

    ftp = ftplib.FTP(args.host, timeout=10)
    ftp.login('xbox','xbox')
    buf = io.BytesIO()
    ftp.retrbinary('RETR ' + args.report_remote, buf.write)
    try: ftp.quit()
    except: pass
    text = buf.getvalue().decode(errors='replace')
    print('=== controller-readback.txt ===')
    print(text)
    report = {l.split('=',1)[0].strip(): l.split('=',1)[1].strip()
              for l in text.splitlines() if '=' in l}
    print('button.dpad_right=' + report.get('button.dpad_right','?'),
          'button.a=' + report.get('button.a','?'),
          'axis.leftx=' + report.get('axis.leftx','?'))
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
