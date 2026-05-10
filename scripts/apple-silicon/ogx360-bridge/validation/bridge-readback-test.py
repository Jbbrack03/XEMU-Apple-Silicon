"""More robust: longer sender, log timing."""
import os, sys, struct, time, threading, ftplib, io
sys.path.insert(0, '/Users/jbbrack03/XEMU_MacOS/xemu-fork/scripts/apple-silicon')
import serial

DEVICE = '/dev/cu.usbmodem3101'
HOST = '192.168.0.200'
XBE_PATH = r'E:\Apps\controller-readback\default.xbe'
REPORT_REMOTE = '/E/Apps/controller-readback/controller-readback.txt'

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

stop = threading.Event()
sent = [0]
def sender():
    ser = serial.Serial(DEVICE, 115200, timeout=0, write_timeout=2.0)
    time.sleep(0.2)
    f = make_frame(1, **STATE)
    while not stop.is_set():
        try:
            ser.write(f); ser.flush()
            sent[0] += 1
        except Exception as e:
            print(f'  [sender] err: {e}', file=sys.stderr); break
        time.sleep(0.005)  # 200Hz
    try:
        ser.write(make_frame(1, 0, 0,0,0,0, 0,0, 0,0, 0,0,0,0)); ser.flush()
    except Exception: pass
    ser.close()

# Warm up sender FIRST so the bridge state is stable BEFORE we chainload
print('[host] sender starting at 200Hz with DRIGHT + A=0xAA + lstickX=+25000')
t = threading.Thread(target=sender, daemon=True); t.start()
time.sleep(3.0)
print(f'[host] sender warmed up, sent so far: {sent[0]} frames')

print('[host] chainloading XBE')
ftp = ftplib.FTP(HOST, timeout=10)
ftp.login('xbox','xbox')
print('  ', ftp.sendcmd('SITE EXEC ' + XBE_PATH))
try: ftp.quit()
except: pass

print('[host] sustaining state for 18s (XBE polls 4.8s + boot/reboot ~3s + buffer)')
t0 = time.monotonic()
while time.monotonic() - t0 < 18.0:
    time.sleep(0.5)

print(f'[host] stopping sender (sent={sent[0]} frames)')
stop.set(); t.join(timeout=2)

print('[host] waiting for Xbox FTP to return...')
deadline = time.monotonic() + 60
while time.monotonic() < deadline:
    try:
        ftp = ftplib.FTP(HOST, timeout=4)
        ftp.login('xbox','xbox'); ftp.quit(); break
    except Exception: time.sleep(2)
else:
    print('  Xbox didnt return'); sys.exit(3)
print('[host] Xbox back')
time.sleep(1)

ftp = ftplib.FTP(HOST, timeout=10)
ftp.login('xbox','xbox')
buf = io.BytesIO()
ftp.retrbinary('RETR ' + REPORT_REMOTE, buf.write)
try: ftp.quit()
except: pass
text = buf.getvalue().decode(errors='replace')
print('=== controller-readback.txt ===')
print(text)
report = {l.split('=',1)[0].strip(): l.split('=',1)[1].strip() for l in text.splitlines() if '=' in l}
print('button.dpad_right=' + report.get('button.dpad_right','?'),
      'button.a=' + report.get('button.a','?'),
      'axis.leftx=' + report.get('axis.leftx','?'))
