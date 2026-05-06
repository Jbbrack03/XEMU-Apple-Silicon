#!/usr/bin/env python3
"""
oracle-client.py — Mac-side wrapper around the xbox-oracle-agent
TCP-9001 protocol.

The agent (scripts/apple-silicon/xbe-tests/oracle-agent/) speaks an
SMTP-style line protocol with three response codes:

    200- <text>            single-line success
    201- OK [<count>]      multi-line text payload; ends with '.'
    202- BINARY <length>   exactly <length> raw bytes follow
    500- <text>            error

This module exposes those commands as a Python class plus a
command-line interface usable from shell scripts.

Module API
----------
    with OracleClient("192.168.0.200") as oracle:
        info = oracle.info()
        eeprom = oracle.eeprom()                     # bytes(256)
        regs = oracle.nv2a_read(0x600800)            # int (32-bit)
        mem = oracle.mem_read(0x80000000, 0x100)     # bytes
        rgba, w, h = oracle.screenshot()             # PIL-friendly
        oracle.runxbe(r"C:\\xboxdash.xbe")           # one-way

CLI
---
    oracle-client.py [--host H] [--port P] [--timeout S] CMD [ARGS]

    info                                       agent + console info
    eeprom [--out PATH]                        dump EEPROM hex (or write to .bin)
    mem-read ADDR LEN [--out PATH]             binary memory read
    mem-write ADDR HEX                         hex-encoded write (unsafe.enable first)
    nv2a-read OFF                              one 32-bit register
    nv2a-write OFF VAL                         write 32-bit register
    vram-read OFF LEN [--out PATH]             VRAM aperture read
    screenshot [--out PNG] [--raw RAW]         capture front buffer
    runxbe XBOXPATH                            chainload another XBE
    unsafe-enable                              arm writes for this session
    reboot                                     reboot the console
    bye                                        close cleanly
    raw VERB [ARGS...]                         send any verb verbatim
    wait-ready [--retries N] [--delay S]       poll until port 9001 listens

Exit codes: 0 on success, 1 on protocol/IO error, 2 on remote 500-.
"""
from __future__ import annotations

import argparse
import os
import socket
import struct
import sys
import time
from contextlib import contextmanager
from typing import Iterator, Optional, Tuple


DEFAULT_HOST = "192.168.0.200"
DEFAULT_PORT = 9001
DEFAULT_TIMEOUT = 15.0
SCREENSHOT_HEADER_MAGIC = b"XOSS"


class OracleError(RuntimeError):
    """Remote 500-, malformed response, or transport error.

    Subclasses distinguish the case:
      OracleRemoteError  — agent sent a 500- (verb rejected)
      OracleProtocolError — bytes received but framing was malformed
      OracleTransportError — socket error or premature EOF before/in payload
    """


class OracleRemoteError(OracleError):
    """Agent returned a 500- response (the verb was understood but
    rejected — e.g. bad arguments, gating, address out of allowlist)."""


class OracleProtocolError(OracleError):
    """Response bytes were received but framing was malformed
    (unknown code, bad 202- length, etc.)."""


class OracleTransportError(OracleError):
    """Socket-level failure: closed mid-payload, bad shape, etc."""


class OracleClient:
    """Line + binary protocol client for xbox-oracle-agent."""

    def __init__(self, host: str = DEFAULT_HOST, port: int = DEFAULT_PORT,
                 timeout: float = DEFAULT_TIMEOUT) -> None:
        self.host = host
        self.port = port
        self.timeout = timeout
        self.sock: Optional[socket.socket] = None
        self._buf = bytearray()
        self.greeting: Optional[str] = None

    # ---- connection lifecycle ----

    def connect(self) -> None:
        if self.sock is not None:
            return
        s = socket.create_connection((self.host, self.port), timeout=self.timeout)
        s.settimeout(self.timeout)
        self.sock = s
        self.greeting = self._recv_line()
        if not self.greeting.startswith("200-"):
            raise OracleError(f"unexpected greeting: {self.greeting}")

    def close(self, polite: bool = True) -> None:
        """Close the connection. If `polite`, attempt to send `bye` first
        so the agent's state machine sees a clean FIN instead of a
        socket-level RST. Polite close is best-effort — any error is
        swallowed because the close path must always succeed."""
        if self.sock is not None:
            if polite:
                try:
                    self.sock.settimeout(2.0)
                    self.sock.sendall(b"bye\r\n")
                    # Drain remaining response (the 200- bye line) so
                    # the agent finishes writing before we close. Best
                    # effort.
                    try:
                        self.sock.recv(256)
                    except OSError:
                        pass
                except OSError:
                    pass
            try:
                self.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None
            self._buf = bytearray()

    def __enter__(self) -> "OracleClient":
        self.connect()
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.close()

    # ---- low-level read primitives ----

    def _ensure_sock(self) -> socket.socket:
        if self.sock is None:
            raise OracleError("not connected")
        return self.sock

    def _recv_more(self) -> None:
        """Pull one chunk from the socket into the buffer."""
        s = self._ensure_sock()
        chunk = s.recv(8192)
        if not chunk:
            raise OracleTransportError("connection closed by agent")
        self._buf.extend(chunk)

    def _recv_line(self) -> str:
        """Read one CRLF-terminated line, stripping the terminator."""
        while True:
            i = self._buf.find(b"\n")
            if i >= 0:
                line = bytes(self._buf[:i])
                del self._buf[: i + 1]
                if line.endswith(b"\r"):
                    line = line[:-1]
                return line.decode("utf-8", "replace")
            self._recv_more()

    def _recv_exactly(self, n: int) -> bytes:
        """Read exactly n bytes, satisfying first from buffer."""
        if n <= len(self._buf):
            out = bytes(self._buf[:n])
            del self._buf[:n]
            return out
        out = bytearray(self._buf)
        del self._buf[:]
        remaining = n - len(out)
        s = self._ensure_sock()
        while remaining > 0:
            chunk = s.recv(min(remaining, 65536))
            if not chunk:
                raise OracleTransportError("connection closed mid-payload")
            out.extend(chunk)
            remaining -= len(chunk)
        return bytes(out)

    # ---- mid-level protocol ----

    def _send_line(self, line: str) -> None:
        s = self._ensure_sock()
        s.sendall(line.encode("utf-8") + b"\r\n")

    def _read_response(self) -> Tuple[str, bytes]:
        """Read one full response. Returns ('200', text) or ('201', payload_text)
        or ('202', binary_bytes). Raises:
          OracleRemoteError on 500- (agent rejected the verb)
          OracleProtocolError on malformed framing
          OracleTransportError on socket-level failure (via _recv_*).
        """
        first = self._recv_line()
        if len(first) < 4 or first[3] != "-":
            raise OracleProtocolError(f"malformed response: {first!r}")
        code = first[:3]
        rest = first[4:].lstrip()
        if code == "200":
            return ("200", rest.encode())
        if code == "500":
            raise OracleRemoteError(rest)
        if code == "201":
            lines = []
            while True:
                line = self._recv_line()
                if line == ".":
                    break
                lines.append(line)
            return ("201", "\n".join(lines).encode())
        if code == "202":
            # rest = "BINARY <length>"
            parts = rest.split()
            if len(parts) != 2 or parts[0] != "BINARY":
                raise OracleProtocolError(f"malformed 202 header: {first!r}")
            try:
                length = int(parts[1])
            except ValueError as e:
                raise OracleProtocolError(f"malformed 202 length: {first!r}") from e
            if length < 0 or length > 64 * 1024 * 1024:
                raise OracleProtocolError(f"unreasonable 202 length: {length}")
            return ("202", self._recv_exactly(length))
        raise OracleProtocolError(f"unknown response code: {first!r}")

    # ---- high-level commands ----

    def info(self) -> str:
        self._send_line("info")
        code, payload = self._read_response()
        return payload.decode()

    def eeprom(self) -> bytes:
        self._send_line("eeprom")
        code, payload = self._read_response()
        if code != "201":
            raise OracleError(f"unexpected eeprom response: code={code}")
        text = payload.decode().replace("\n", "")
        if len(text) % 2:
            raise OracleError("eeprom hex has odd length")
        try:
            return bytes.fromhex(text)
        except ValueError as e:
            raise OracleError(f"eeprom hex decode failed: {e}") from e

    def mem_read(self, addr: int, length: int) -> bytes:
        self._send_line(f"mem.read addr=0x{addr:08x} len={length}")
        code, payload = self._read_response()
        if code != "202":
            raise OracleError(f"unexpected mem.read response: code={code}")
        return payload

    def mem_write(self, addr: int, data: bytes) -> str:
        hex_data = data.hex()
        self._send_line(f"mem.write addr=0x{addr:08x} data={hex_data}")
        code, payload = self._read_response()
        return payload.decode()

    def nv2a_read(self, off: int) -> int:
        self._send_line(f"nv2a.read off=0x{off:08x}")
        code, payload = self._read_response()
        text = payload.decode().strip()
        return int(text, 16)

    def nv2a_write(self, off: int, val: int) -> str:
        self._send_line(f"nv2a.write off=0x{off:08x} val=0x{val:08x}")
        code, payload = self._read_response()
        return payload.decode()

    def vram_read(self, off: int, length: int) -> bytes:
        self._send_line(f"vram.read off=0x{off:08x} len={length}")
        code, payload = self._read_response()
        if code != "202":
            raise OracleError(f"unexpected vram.read response: code={code}")
        return payload

    def screenshot(self) -> Tuple[bytes, int, int, int]:
        """Returns (raw_pixels_bgrx, width, height, stride). Pixel bytes
        are the front-buffer's native layout — typically 32-bit
        X8R8G8B8 little-endian (B G R X in memory). Convert to RGBA
        in the caller (helper save_screenshot_png does this)."""
        self._send_line("screenshot")
        code, payload = self._read_response()
        if code != "202":
            raise OracleError(f"unexpected screenshot response: code={code}")
        if len(payload) < 16:
            raise OracleError(f"screenshot payload too short: {len(payload)}")
        if payload[:4] != SCREENSHOT_HEADER_MAGIC:
            raise OracleError(f"bad screenshot magic: {payload[:4]!r}")
        w, h, stride = struct.unpack_from("<III", payload, 4)
        pixels = payload[16:]
        expected = stride * h
        if len(pixels) != expected:
            raise OracleError(
                f"screenshot pixel size mismatch: got {len(pixels)} "
                f"expected {expected} (w={w} h={h} stride={stride})"
            )
        return pixels, w, h, stride

    def runxbe(self, path: str) -> str:
        """Issue runxbe; agent acks then dies. Connection drops as the
        kernel chainloads the new image. The Xbox does NOT auto-relaunch
        the agent — the orchestrator must SITE RunXBE it again later."""
        self._send_line(f"runxbe path={path}")
        code, payload = self._read_response()
        return payload.decode()

    def unsafe_enable(self) -> str:
        self._send_line("unsafe.enable")
        code, payload = self._read_response()
        return payload.decode()

    def reboot(self) -> str:
        self._send_line("reboot")
        code, payload = self._read_response()
        return payload.decode()

    def bye(self) -> str:
        """Send `bye` and read the response. After this the connection
        is being torn down by the agent — the caller should drop the
        socket. Most callers should use close() instead, which is
        non-throwing and handles the bye + shutdown."""
        self._send_line("bye")
        code, payload = self._read_response()
        return payload.decode()

    def help(self) -> str:
        self._send_line("help")
        code, payload = self._read_response()
        return payload.decode()

    def raw(self, line: str) -> Tuple[str, bytes]:
        """Send any verb verbatim. Returns the raw (code, payload) pair."""
        self._send_line(line)
        return self._read_response()


# ---- helpers ----

def wait_until_ready(host: str = DEFAULT_HOST, port: int = DEFAULT_PORT,
                     retries: int = 60, delay: float = 1.0) -> bool:
    """Poll the agent's listener until it accepts connections. Returns
    True on success, False on timeout. Each poll does a polite-close
    oracle handshake (greeting + `bye` + shutdown) so the agent's
    lwIP PCB pool stays healthy across many polls."""
    for _ in range(retries):
        try:
            with socket.create_connection((host, port), timeout=2.0) as s:
                s.settimeout(2.0)
                data = b""
                try:
                    data = s.recv(128)
                except OSError:
                    pass
                # Polite close so the agent sees a FIN, not a RST.
                try:
                    s.sendall(b"bye\r\n")
                    s.recv(128)
                except OSError:
                    pass
                try:
                    s.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                if data.startswith(b"200-"):
                    return True
                # If we got data of any kind, treat as alive — some
                # builds may delay the greeting fractionally.
                if data:
                    return True
        except (OSError, socket.timeout):
            time.sleep(delay)
    return False


def bgrx_to_rgba(pixels: bytes, w: int, h: int, stride: int) -> bytes:
    """Convert front-buffer X8R8G8B8 little-endian (B G R X bytes) to
    R G B A bytes, dropping leading padding per scan line."""
    out = bytearray(w * h * 4)
    src_row_used = w * 4
    for y in range(h):
        src_off = y * stride
        dst_off = y * w * 4
        row = pixels[src_off : src_off + src_row_used]
        for x in range(w):
            b = row[4 * x + 0]
            g = row[4 * x + 1]
            r = row[4 * x + 2]
            out[dst_off + 4 * x + 0] = r
            out[dst_off + 4 * x + 1] = g
            out[dst_off + 4 * x + 2] = b
            out[dst_off + 4 * x + 3] = 255
    return bytes(out)


def save_screenshot_png(rgba: bytes, w: int, h: int, path: str) -> None:
    """Write rgba bytes as a PNG. Uses Pillow if available, otherwise a
    minimal stdlib-only encoder via zlib + struct."""
    try:
        from PIL import Image  # type: ignore
        Image.frombytes("RGBA", (w, h), rgba).save(path)
        return
    except ImportError:
        pass
    # Stdlib-only fallback: minimal PNG encoder. Works for any (w, h).
    import struct as _struct
    import zlib as _zlib

    def _chunk(tag: bytes, data: bytes) -> bytes:
        crc = _zlib.crc32(tag + data) & 0xFFFFFFFF
        return _struct.pack(">I", len(data)) + tag + data + _struct.pack(">I", crc)

    # IHDR: 8-bit RGBA = bit depth 8, color type 6, no interlace
    ihdr = _struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)
    # Add filter byte (0 = None) before each scan line
    raw = bytearray()
    row_size = w * 4
    for y in range(h):
        raw.append(0)
        raw.extend(rgba[y * row_size : (y + 1) * row_size])
    idat = _zlib.compress(bytes(raw), 6)
    png = b"\x89PNG\r\n\x1a\n"
    png += _chunk(b"IHDR", ihdr)
    png += _chunk(b"IDAT", idat)
    png += _chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


# ---- CLI ----

def _parse_int(s: str) -> int:
    s = s.strip()
    return int(s, 0)


def _read_path_or_stdout(payload: bytes, path: Optional[str]) -> None:
    """Write a binary payload to PATH (`--out`) or stdout. Stdout is
    written byte-exact: NO trailing newline. Pipe to `xxd` or `hexdump`
    to view, or use `--out`. The earlier `\\n` append corrupted any
    consumer parsing exact byte counts."""
    if path:
        with open(path, "wb") as f:
            f.write(payload)
    else:
        sys.stdout.buffer.write(payload)
        sys.stdout.buffer.flush()


def main(argv: Optional[list[str]] = None) -> int:
    p = argparse.ArgumentParser(description="Mac-side oracle-agent client")
    p.add_argument("--host", default=os.environ.get("ORACLE_HOST", DEFAULT_HOST))
    p.add_argument("--port", type=int, default=int(os.environ.get("ORACLE_PORT", DEFAULT_PORT)))
    p.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("info")
    pe = sub.add_parser("eeprom")
    pe.add_argument("--out", help="write 256-byte raw to PATH instead of hex stdout")

    pmr = sub.add_parser("mem-read")
    pmr.add_argument("addr", type=_parse_int)
    pmr.add_argument("length", type=_parse_int)
    pmr.add_argument("--out", help="write raw bytes to PATH instead of stdout")

    pmw = sub.add_parser("mem-write")
    pmw.add_argument("addr", type=_parse_int)
    pmw.add_argument("hex_data", help="hex-encoded payload")

    pnr = sub.add_parser("nv2a-read")
    pnr.add_argument("off", type=_parse_int)

    pnw = sub.add_parser("nv2a-write")
    pnw.add_argument("off", type=_parse_int)
    pnw.add_argument("val", type=_parse_int)

    pvr = sub.add_parser("vram-read")
    pvr.add_argument("off", type=_parse_int)
    pvr.add_argument("length", type=_parse_int)
    pvr.add_argument("--out", help="write raw bytes to PATH instead of stdout")

    ps = sub.add_parser("screenshot")
    ps.add_argument("--out", help="PNG output path")
    ps.add_argument("--raw", help="raw payload (header+pixels) output path")

    pr = sub.add_parser("runxbe")
    pr.add_argument("path")

    sub.add_parser("unsafe-enable")
    sub.add_parser("reboot")
    sub.add_parser("bye")
    sub.add_parser("help")

    pra = sub.add_parser("raw")
    pra.add_argument("verb")
    pra.add_argument("rest", nargs=argparse.REMAINDER)

    pwr = sub.add_parser("wait-ready")
    pwr.add_argument("--retries", type=int, default=60)
    pwr.add_argument("--delay", type=float, default=1.0)

    args = p.parse_args(argv)

    if args.cmd == "wait-ready":
        ok = wait_until_ready(args.host, args.port, args.retries, args.delay)
        if ok:
            print(f"agent at {args.host}:{args.port} is ready")
            return 0
        print(f"agent at {args.host}:{args.port} did not respond", file=sys.stderr)
        return 1

    try:
        with OracleClient(args.host, args.port, args.timeout) as oracle:
            if args.cmd == "info":
                print(oracle.info())
            elif args.cmd == "eeprom":
                data = oracle.eeprom()
                if args.out:
                    with open(args.out, "wb") as f:
                        f.write(data)
                    print(f"wrote {len(data)} bytes to {args.out}")
                else:
                    print(data.hex())
            elif args.cmd == "mem-read":
                data = oracle.mem_read(args.addr, args.length)
                _read_path_or_stdout(data, args.out)
            elif args.cmd == "mem-write":
                data = bytes.fromhex(args.hex_data)
                print(oracle.mem_write(args.addr, data))
            elif args.cmd == "nv2a-read":
                val = oracle.nv2a_read(args.off)
                print(f"0x{val:08x}")
            elif args.cmd == "nv2a-write":
                print(oracle.nv2a_write(args.off, args.val))
            elif args.cmd == "vram-read":
                data = oracle.vram_read(args.off, args.length)
                _read_path_or_stdout(data, args.out)
            elif args.cmd == "screenshot":
                pixels, w, h, stride = oracle.screenshot()
                if args.raw:
                    header = struct.pack("<4sIII", SCREENSHOT_HEADER_MAGIC, w, h, stride)
                    with open(args.raw, "wb") as f:
                        f.write(header + pixels)
                    print(f"wrote {16 + len(pixels)} bytes to {args.raw}")
                if args.out:
                    rgba = bgrx_to_rgba(pixels, w, h, stride)
                    save_screenshot_png(rgba, w, h, args.out)
                    print(f"wrote {w}x{h} PNG to {args.out}")
                if not args.out and not args.raw:
                    print(f"captured {w}x{h} stride={stride} bytes={len(pixels)}")
            elif args.cmd == "runxbe":
                print(oracle.runxbe(args.path))
            elif args.cmd == "unsafe-enable":
                print(oracle.unsafe_enable())
            elif args.cmd == "reboot":
                print(oracle.reboot())
            elif args.cmd == "bye":
                print(oracle.bye())
            elif args.cmd == "help":
                print(oracle.help())
            elif args.cmd == "raw":
                line = " ".join([args.verb] + args.rest)
                code, payload = oracle.raw(line)
                if code == "202":
                    sys.stdout.buffer.write(payload)
                else:
                    print(f"{code} {payload.decode(errors='replace')}")
            else:
                p.print_help()
                return 1
    except OracleError as e:
        print(f"oracle error: {e}", file=sys.stderr)
        return 2
    except OSError as e:
        print(f"transport error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
