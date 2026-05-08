#!/usr/bin/env python3
"""Inspect enough XBE metadata to evaluate retail input automation paths.

This is intentionally narrow and read-only. It parses the XBE header,
certificate, section table, library-version table, and a small string scan
without trying to patch or validate signatures. The main consumer is the
retail-oracle Tier-2 investigation: if a title-level controller patch is being
considered, this tool makes the evidence visible and repeatable.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import struct
import sys
from typing import Any


XBE_MAGIC = 0x48454258  # "XBEH" little-endian
XOR_EP_RETAIL = 0xA8FC57AB
XOR_EP_DEBUG = 0x94859D4B
XOR_KT_RETAIL = 0x5B6D40B6
XOR_KT_DEBUG = 0xEFB1F152

KEY_STRING_RE = re.compile(
    rb"(XInput|XGetDevice|XGetDevices|XGetState|XID|XAPI|controller|gamepad|"
    rb"LaunchData|XLaunch|reconnect)",
    re.IGNORECASE,
)


class XbeError(RuntimeError):
    pass


def u16(data: bytes, off: int) -> int:
    return struct.unpack_from("<H", data, off)[0]


def u32(data: bytes, off: int) -> int:
    return struct.unpack_from("<I", data, off)[0]


def read_cstr(data: bytes, off: int, limit: int = 4096) -> str:
    if off < 0 or off >= len(data):
        return ""
    chunk = data[off:min(len(data), off + limit)]
    nul = chunk.find(b"\0")
    if nul >= 0:
        chunk = chunk[:nul]
    return chunk.decode("utf-8", "replace")


def printable_strings(data: bytes, minimum: int = 5) -> list[tuple[int, str]]:
    out: list[tuple[int, str]] = []
    start = None
    for idx, b in enumerate(data):
        is_print = 32 <= b <= 126 or b in (9,)
        if is_print:
            if start is None:
                start = idx
        else:
            if start is not None and idx - start >= minimum:
                out.append((start, data[start:idx].decode("ascii", "replace")))
            start = None
    if start is not None and len(data) - start >= minimum:
        out.append((start, data[start:].decode("ascii", "replace")))
    return out


class XbeImage:
    def __init__(self, path: Path):
        self.path = path
        self.data = path.read_bytes()
        if len(self.data) < 0x178:
            raise XbeError(f"{path}: too small for XBE header")
        if u32(self.data, 0) != XBE_MAGIC:
            raise XbeError(f"{path}: missing XBEH magic")
        self.base = u32(self.data, 0x104)
        self.size_headers = u32(self.data, 0x108)
        self.size_image = u32(self.data, 0x10C)

    def va_to_off(self, va: int) -> int:
        off = va - self.base
        if off < 0 or off >= len(self.data):
            raise XbeError(
                f"{self.path}: VA 0x{va:08x} outside file "
                f"(base 0x{self.base:08x}, size 0x{len(self.data):x})"
            )
        return off

    def va_cstr(self, va: int) -> str:
        if va == 0:
            return ""
        try:
            return read_cstr(self.data, self.va_to_off(va))
        except XbeError:
            return ""

    def parse_sections(self) -> list[dict[str, Any]]:
        count = u32(self.data, 0x11C)
        addr = u32(self.data, 0x120)
        off = self.va_to_off(addr)
        sections = []
        for i in range(count):
            cur = off + i * 0x38
            if cur + 0x38 > len(self.data):
                raise XbeError(f"{self.path}: section table truncated")
            name_addr = u32(self.data, cur + 0x14)
            sections.append({
                "index": i,
                "name": self.va_cstr(name_addr),
                "flags": f"0x{u32(self.data, cur + 0x00):08x}",
                "virtual_address": f"0x{u32(self.data, cur + 0x04):08x}",
                "virtual_size": u32(self.data, cur + 0x08),
                "raw_offset": u32(self.data, cur + 0x0C),
                "raw_size": u32(self.data, cur + 0x10),
                "executable": bool(u32(self.data, cur + 0x00) & 0x4),
                "writable": bool(u32(self.data, cur + 0x00) & 0x1),
            })
        return sections

    def parse_libraries(self) -> list[dict[str, Any]]:
        count = u32(self.data, 0x160)
        addr = u32(self.data, 0x164)
        if count == 0 or addr == 0:
            return []
        off = self.va_to_off(addr)
        libs = []
        for i in range(count):
            cur = off + i * 0x10
            if cur + 0x10 > len(self.data):
                raise XbeError(f"{self.path}: library table truncated")
            name = self.data[cur:cur + 8].split(b"\0", 1)[0].decode("ascii", "replace")
            flags = u16(self.data, cur + 0x0E)
            libs.append({
                "index": i,
                "name": name,
                "major": u16(self.data, cur + 0x08),
                "minor": u16(self.data, cur + 0x0A),
                "build": u16(self.data, cur + 0x0C),
                "qfe": flags & 0x1FFF,
                "approved": (flags >> 13) & 0x3,
                "debug": bool(flags & 0x8000),
            })
        return libs

    def parse_certificate(self) -> dict[str, Any]:
        addr = u32(self.data, 0x118)
        if addr == 0:
            return {}
        off = self.va_to_off(addr)
        if off + 0xAC > len(self.data):
            raise XbeError(f"{self.path}: certificate truncated")
        title_raw = self.data[off + 0x0C:off + 0x0C + 80]
        title = title_raw.decode("utf-16le", "replace").split("\0", 1)[0]
        return {
            "size": u32(self.data, off + 0x00),
            "timestamp": f"0x{u32(self.data, off + 0x04):08x}",
            "title_id": f"0x{u32(self.data, off + 0x08):08x}",
            "title_name": title,
            "allowed_media": f"0x{u32(self.data, off + 0x9C):08x}",
            "game_region": f"0x{u32(self.data, off + 0xA0):08x}",
            "ratings": f"0x{u32(self.data, off + 0xA4):08x}",
            "disk_number": u32(self.data, off + 0xA8),
            "version": u32(self.data, off + 0xAC),
        }

    def key_strings(self, limit: int = 80) -> list[dict[str, Any]]:
        matches = []
        for off, text in printable_strings(self.data):
            if KEY_STRING_RE.search(text.encode("utf-8", "replace")):
                matches.append({"offset": f"0x{off:08x}", "text": text})
                if len(matches) >= limit:
                    break
        return matches

    def inspect(self, string_limit: int = 80) -> dict[str, Any]:
        entry_raw = u32(self.data, 0x128)
        kt_raw = u32(self.data, 0x158)
        debug_path_addr = u32(self.data, 0x14C)
        return {
            "schema": "xbe-inspect-v1",
            "path": str(self.path),
            "size": len(self.data),
            "base": f"0x{self.base:08x}",
            "sizeof_headers": self.size_headers,
            "sizeof_image": self.size_image,
            "timestamp": f"0x{u32(self.data, 0x114):08x}",
            "entry_raw": f"0x{entry_raw:08x}",
            "entry_retail": f"0x{entry_raw ^ XOR_EP_RETAIL:08x}",
            "entry_debug": f"0x{entry_raw ^ XOR_EP_DEBUG:08x}",
            "kernel_thunk_raw": f"0x{kt_raw:08x}",
            "kernel_thunk_retail": f"0x{kt_raw ^ XOR_KT_RETAIL:08x}",
            "kernel_thunk_debug": f"0x{kt_raw ^ XOR_KT_DEBUG:08x}",
            "debug_path": self.va_cstr(debug_path_addr),
            "certificate": self.parse_certificate(),
            "sections": self.parse_sections(),
            "libraries": self.parse_libraries(),
            "key_strings": self.key_strings(string_limit),
        }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("xbe", type=Path, nargs="+")
    parser.add_argument("--json", action="store_true", help="Emit JSON.")
    parser.add_argument("--strings", type=int, default=80,
                        help="Maximum matching key strings per XBE.")
    args = parser.parse_args()

    payload = []
    for xbe in args.xbe:
        payload.append(XbeImage(xbe).inspect(string_limit=args.strings))

    if args.json:
        print(json.dumps(payload if len(payload) != 1 else payload[0],
                         indent=2, sort_keys=True))
    else:
        for item in payload:
            cert = item.get("certificate") or {}
            print(f"{item['path']}")
            print(f"  title: {cert.get('title_name') or '<unknown>'} "
                  f"({cert.get('title_id') or 'no title id'})")
            print(f"  base={item['base']} image={item['sizeof_image']} "
                  f"debug_path={item['debug_path'] or '<none>'}")
            libs = ", ".join(lib["name"] for lib in item["libraries"])
            print(f"  libraries: {libs or '<none>'}")
            print("  key strings:")
            for row in item["key_strings"][:args.strings]:
                print(f"    {row['offset']} {row['text']}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except XbeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(1)
