#!/usr/bin/env python3
"""Patch known retail title XBEs for oracle-controlled smoke probes.

The first production proof for each retail title is deliberately small:
replace the XBE entrypoint with a standalone stub that waits for a fixed
duration, then calls the project Xbox kernel's HalReturnToFirmware export.

This does not yet drive gameplay. It proves the autonomous dashboard-return
half of the per-title patching ladder without depending on title assets or on
the crashed Tier-2 controller hook path.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import subprocess
import struct
import tempfile
import time
from typing import Any


XBE_MAGIC = 0x48454258
XOR_EP_RETAIL = 0xA8FC57AB

DEFAULT_LIBRARY = Path(
    os.environ.get(
        "XEMU_GAME_LIBRARY",
        "/Volumes/Josh-Backup-Files/Console Games/Original Xbox",
    )
)
DEFAULT_OUT_ROOT = Path("benchmark-runs/retail-title-patches")

# Project-Xbox kernel exports from
# xbox-oracle-backup/2026-05-06/kernel-symbols/xboxkrnl-0x80010000-exports.tsv.
PROJECT_XBOX_EXPORTS = {
    "HalReturnToFirmware": 0x8001542D,
    "KeStallExecutionProcessor": 0x80015530,
    "KeTickCount": 0x8003B81C,
}

FIRMWARE_REENTRY = {
    "halt": 0,
    "reboot": 1,
    "quick-reboot": 2,
    "kd-reboot": 3,
    "fatal-reboot": 4,
}

TARGETS: dict[str, dict[str, Any]] = {
    "pgr2": {
        "title": "Project Gotham Racing 2",
        "title_id": "0x4d53004b",
        "source": "XBOX HDD ready (J-Q)/PGR2/default.xbe",
        "sha256": "29e401b5ee1cf79a1e7d964fc5600e86421673d1f42111e0254c91915b99da08",
        "route_csv": "scripts/apple-silicon/input-scripts/pgr2-gameplay.csv",
    },
    "crimson": {
        "title": "Crimson Skies",
        "title_id": "0x4d530021",
        "source": "XBOX HDD ready (#-I)/Crimson skies/default.xbe",
        "sha256": "5ba0017a3039977d0fbcf84d02e1a65f76ea78c1642c69f901b06da477a233a3",
        "route_csv": "scripts/apple-silicon/input-scripts/crimson-gameplay.csv",
    },
    "rainbow": {
        "title": "Rainbow Six 3",
        "title_id": "0x55530013",
        "source": "XBOX HDD ready (R-Z)/Rainbow Six 3/default.xbe",
        "sha256": "e9b7557b9bfc9c97aafa78b3b2d29294b41ea72dc573e052c9d39d2933a768de",
        "route_csv": "scripts/apple-silicon/input-scripts/rainbow-gameplay.csv",
    },
    "sc2": {
        "title": "Soul Calibur 2",
        "title_id": "0x4e4d0003",
        "source": "XBOX HDD ready (R-Z)/Soul Calibur 2/Default.xbe",
        "sha256": "d28c9fff8ec7dad06617792f03e42cc46b4b21bd6f846156170b887d05bfef8f",
        "route_csv": "scripts/apple-silicon/input-scripts/sc2-gameplay.csv",
    },
    "halo": {
        "title": "Halo - Combat Evolved",
        "title_id": "0x4d530004",
        "source": "XBOX HDD ready (#-I)/Halo - Combat Evolved/default.xbe",
        "sha256": "60348317917a026d807c8b175bbc8f48053a8a0264fdeb4746014faf54b5ac40",
    },
    "burnout3": {
        "title": "Burnout 3 Takedown",
        "title_id": None,
        "source": "XBOX HDD ready (#-I)/Burnout 3 Takedown/default.xbe",
        "sha256": "cda533745c8bbab29f2133ae62bfd2f94ff29a67f273a0aa1b64d4c4222ef8ba",
    },
    "outrun2": {
        "title": "Outrun 2",
        "title_id": None,
        "source": "XBOX HDD ready (J-Q)/Outrun 2/default.xbe",
        "sha256": "f91a3a5190eb11b752f2306c7e1352cff5c42bfc71d047adcb2e749bed9129be",
    },
}

XINPUT_SIGNATURES: dict[str, list[tuple[int, int]]] = {
    # Signatures are copied from Cxbx-Reloaded/XbSymbolDatabase OOVPA entries
    # for the static XAPI library variants seen in the target retail XBEs.
    "XInputGetState_3911": [
        (0x0E, 0x80), (0x0F, 0xBA), (0x10, 0xA3), (0x14, 0x01),
        (0x17, 0x6A), (0x18, 0x57), (0x1A, 0xEB), (0x1B, 0x46),
        (0x28, 0xBB), (0x29, 0x8F), (0x2A, 0x04), (0x6E, 0xC2),
        (0x6F, 0x08),
    ],
    "XInputGetState_5455": [
        (0x0D, 0x0C), (0x1C, 0x5E), (0x2B, 0xBB), (0x3A, 0x80),
        (0x49, 0x08), (0x58, 0xF3), (0x67, 0x15),
    ],
    "XGetDevices_3911": [
        (0x07, 0x8B), (0x08, 0x54), (0x09, 0x24), (0x0A, 0x08),
        (0x0D, 0x83), (0x0E, 0x62), (0x0F, 0x04), (0x10, 0x00),
        (0x11, 0x8A), (0x12, 0xC8), (0x16, 0xFF), (0x17, 0x15),
        (0x1F, 0xC2), (0x20, 0x04),
    ],
    "XGetDeviceChanges_3911": [
        (0x00, 0x55), (0x07, 0x33), (0x08, 0xC0), (0x33, 0xF7),
        (0x34, 0xD2), (0x42, 0x0B), (0x43, 0xD7), (0x51, 0x8A),
        (0x52, 0xC8),
    ],
    "XInputOpen_4242plus": [
        (0x14, 0x6A), (0x15, 0x57), (0x1D, 0xEB), (0x1E, 0x33),
    ],
    "XInputOpen_3911": [
        (0x20, 0xEB), (0x21, 0x0B), (0x29, 0x75), (0x2A, 0x3D),
        (0x4A, 0x83), (0x4B, 0xC2), (0x4C, 0x10), (0x66, 0xEB),
        (0x67, 0x09), (0x68, 0x6A), (0x69, 0x57),
    ],
    "XInputGetCapabilities_3911": [
        (0x00, 0x55), (0x0F, 0x15), (0x1E, 0x0F), (0x1F, 0x84),
        (0x36, 0x8B), (0x37, 0xFA), (0x38, 0xF3), (0x39, 0xAB),
        (0x3A, 0xAA), (0x3B, 0x8A), (0x3C, 0x46), (0x3D, 0x0B),
        (0x3E, 0x88),
    ],
    "XInputGetCapabilities_4831plus": [
        (0x00, 0x55), (0x1F, 0x0F), (0x37, 0x8B), (0x38, 0xFA),
        (0x39, 0xF3), (0x3A, 0xAB), (0x3B, 0xAA), (0x3C, 0x8A),
        (0x3D, 0x46), (0x3E, 0x0B), (0x3F, 0x88), (0x59, 0x0F),
        (0x5A, 0xB6),
    ],
    "XInputSetState_3911": [
        (0x04, 0x8D), (0x05, 0x81), (0x06, 0xA3), (0x0F, 0x6A),
        (0x10, 0x57), (0x12, 0xEB), (0x13, 0x21), (0x2D, 0x88),
        (0x2E, 0x42), (0x2F, 0x41), (0x35, 0xC2), (0x36, 0x08),
    ],
    "XInputSetState_4831plus": [
        (0x00, 0x8B), (0x01, 0x4C), (0x02, 0x24), (0x03, 0x04),
        (0x04, 0x8D), (0x05, 0x81), (0x06, 0xA3), (0x07, 0x00),
        (0x08, 0x00), (0x09, 0x00), (0x21, 0x8B), (0x22, 0x40),
        (0x30, 0xC2), (0x31, 0x08),
    ],
    "XInputClose_3911": [
        (0x00, 0x8B), (0x01, 0x4C), (0x02, 0x24), (0x03, 0x04),
        (0x04, 0xE8), (0x09, 0xC2), (0x0A, 0x04),
    ],
}

HOOK_SIGNATURE_GROUPS: dict[str, list[str]] = {
    "xgetdevices": ["XGetDevices_3911"],
    "xgetdevicechanges": ["XGetDeviceChanges_3911"],
    "xinputopen": ["XInputOpen_4242plus", "XInputOpen_3911"],
    "xinputgetcaps": ["XInputGetCapabilities_4831plus", "XInputGetCapabilities_3911"],
    "xinputsetstate": ["XInputSetState_4831plus", "XInputSetState_3911"],
    "xinputgetstate": ["XInputGetState_5455", "XInputGetState_3911"],
    "xinputclose": ["XInputClose_3911"],
}

ANALOG_CONTROLS = {
    "a": 0,
    "b": 1,
    "x": 2,
    "y": 3,
    "black": 4,
    "white": 5,
    "ltrigger": 6,
    "rtrigger": 7,
}

BINARY_CONTROLS = {
    "dpad_up": 8,
    "dpad_down": 9,
    "dpad_left": 10,
    "dpad_right": 11,
    "start": 12,
    "back": 13,
    "lstick_btn": 14,
    "rstick_btn": 15,
}

STICK_CONTROLS = {
    "lstick_x": 16,
    "lstick_y": 17,
    "rstick_x": 18,
    "rstick_y": 19,
}

PROOF_EVENTS = [
    (1500, "start", 1),
    (1900, "start", 0),
    (2500, "a", 1),
    (3100, "a", 0),
    (4200, "dpad_down", 1),
    (4500, "dpad_down", 0),
    (5600, "a", 1),
    (6200, "a", 0),
]


def u32(data: bytes | bytearray, off: int) -> int:
    return struct.unpack_from("<I", data, off)[0]


def put_u32(data: bytearray, off: int, value: int) -> None:
    struct.pack_into("<I", data, off, value & 0xFFFFFFFF)


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def read_utf16_title(data: bytes | bytearray, cert_off: int) -> str:
    raw = bytes(data[cert_off + 0x0C:cert_off + 0x0C + 80])
    return raw.decode("utf-16le", "replace").split("\0", 1)[0]


def va_to_off(data: bytes | bytearray, va: int, base: int) -> int:
    off = va - base
    if off < 0 or off >= len(data):
        raise ValueError(f"VA 0x{va:08x} outside XBE file")
    return off


def off_to_va(sections: list[dict[str, Any]], off: int) -> int:
    for section in sections:
        raw_off = section["raw_offset"]
        raw_size = section["raw_size"]
        if raw_off <= off < raw_off + raw_size:
            return section["virtual_address"] + (off - raw_off)
    raise ValueError(f"file offset 0x{off:x} is outside XBE sections")


def parse_certificate(data: bytes | bytearray, base: int) -> dict[str, Any]:
    cert_va = u32(data, 0x118)
    cert_off = va_to_off(data, cert_va, base)
    return {
        "title": read_utf16_title(data, cert_off),
        "title_id": f"0x{u32(data, cert_off + 0x08):08x}",
        "version": u32(data, cert_off + 0xAC),
    }


def parse_sections(data: bytes | bytearray, base: int) -> list[dict[str, Any]]:
    count = u32(data, 0x11C)
    table_off = va_to_off(data, u32(data, 0x120), base)
    out: list[dict[str, Any]] = []
    for idx in range(count):
        off = table_off + idx * 0x38
        out.append({
            "index": idx,
            "header_off": off,
            "flags": u32(data, off + 0x00),
            "virtual_address": u32(data, off + 0x04),
            "virtual_size": u32(data, off + 0x08),
            "raw_offset": u32(data, off + 0x0C),
            "raw_size": u32(data, off + 0x10),
        })
    return out


def find_signature(data: bytes | bytearray, sig: list[tuple[int, int]]) -> list[int]:
    if not sig:
        return []
    bdata = bytes(data)
    first_off, first_byte = sig[0]
    max_off = max(off for off, _ in sig)
    matches: list[int] = []
    start = 0
    needle = bytes([first_byte])
    while True:
        idx = bdata.find(needle, start)
        if idx < 0:
            break
        off = idx - first_off
        if (
            off >= 0
            and off + max_off < len(data)
            and all(data[off + sig_off] == value for sig_off, value in sig)
        ):
            matches.append(off)
        start = idx + 1
    return matches


def find_hook_offsets(data: bytes | bytearray,
                      sections: list[dict[str, Any]]) -> dict[str, dict[str, Any]]:
    hooks: dict[str, dict[str, Any]] = {}
    for hook_name, sig_names in HOOK_SIGNATURE_GROUPS.items():
        all_matches: list[tuple[str, int]] = []
        for sig_name in sig_names:
            for off in find_signature(data, XINPUT_SIGNATURES[sig_name]):
                all_matches.append((sig_name, off))
        unique = sorted(set(all_matches), key=lambda item: item[1])
        if len(unique) == 1:
            sig_name, off = unique[0]
            hooks[hook_name] = {
                "signature": sig_name,
                "file_offset": off,
                "virtual_address": off_to_va(sections, off),
            }
        elif unique:
            hooks[hook_name] = {
                "status": "ambiguous",
                "matches": [
                    {
                        "signature": sig_name,
                        "file_offset": f"0x{off:08x}",
                        "virtual_address": f"0x{off_to_va(sections, off):08x}",
                    }
                    for sig_name, off in unique[:12]
                ],
            }
    close = hooks.get("xinputclose")
    open_hook = hooks.get("xinputopen")
    if (
        close
        and close.get("status") == "ambiguous"
        and open_hook
        and open_hook.get("status") != "ambiguous"
    ):
        open_off = open_hook["file_offset"]
        nearby = [
            row for row in close["matches"]
            if open_off <= int(row["file_offset"], 16) < open_off + 0x100
        ]
        if len(nearby) == 1:
            row = nearby[0]
            hooks["xinputclose"] = {
                "signature": row["signature"],
                "file_offset": int(row["file_offset"], 16),
                "virtual_address": int(row["virtual_address"], 16),
            }
    return hooks


def parse_input_csv(path: Path) -> list[tuple[int, str, int]]:
    events: list[tuple[int, str, int]] = []
    with path.open("r", encoding="utf-8", errors="replace") as f:
        for line_no, raw in enumerate(f, start=1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            row = next(csv.reader([line.replace("\t", ",")]))
            if len(row) != 3:
                row = line.split()
            if len(row) != 3:
                raise ValueError(f"{path}:{line_no}: expected time,control,value")
            try:
                t = int(row[0])
                control = row[1].strip().lower()
                value = int(row[2])
            except ValueError as exc:
                raise ValueError(f"{path}:{line_no}: bad integer field") from exc
            if (
                control not in ANALOG_CONTROLS
                and control not in BINARY_CONTROLS
                and control not in STICK_CONTROLS
            ):
                raise ValueError(f"{path}:{line_no}: unknown control {control!r}")
            events.append((t, control, value))
    events.sort(key=lambda event: event[0])
    return events


def encode_route_events(events: list[tuple[int, str, int]]) -> bytes:
    encoded = bytearray()
    for t, control, value in events:
        if control in ANALOG_CONTROLS:
            code = ANALOG_CONTROLS[control]
            if control in ("ltrigger", "rtrigger"):
                encoded_value = max(0, min(255, value >> 7))
            else:
                encoded_value = 0xFF if value else 0
        elif control in BINARY_CONTROLS:
            code = BINARY_CONTROLS[control]
            encoded_value = 1 if value else 0
        else:
            code = STICK_CONTROLS[control]
            encoded_value = max(-32768, min(32767, value))
        encoded += struct.pack("<IBbh", max(0, t), code, 0, encoded_value)
    return bytes(encoded)


def route_summary(events: list[tuple[int, str, int]], source: str) -> dict[str, Any]:
    controls = sorted({control for _, control, _ in events})
    first = events[0][0] if events else None
    last = events[-1][0] if events else None
    return {
        "source": source,
        "events": len(events),
        "first_ms": first,
        "last_ms": last,
        "duration_s": None if first is None or last is None else round((last - first) / 1000, 3),
        "controls": controls,
    }


def asm_hex(data: bytes, indent: str = "    ") -> str:
    if not data:
        return f"{indent}; no route events"
    lines = []
    for idx in range(0, len(data), 16):
        chunk = data[idx:idx + 16]
        lines.append(indent + "db " + ", ".join(f"0x{b:02x}" for b in chunk))
    return "\n".join(lines)


def assemble_nasm(source: str) -> bytes:
    nasm = os.environ.get("NASM", "nasm")
    with tempfile.TemporaryDirectory(prefix="retail-title-patch-") as td:
        asm_path = Path(td) / "stub.asm"
        bin_path = Path(td) / "stub.bin"
        asm_path.write_text(source, encoding="utf-8")
        proc = subprocess.run(
            [nasm, "-f", "bin", "-o", str(bin_path), str(asm_path)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        if proc.returncode != 0:
            raise RuntimeError(f"nasm failed:\n{proc.stderr}")
        return bin_path.read_bytes()


def build_automation_stub(
    stub_va: int,
    route_blob: bytes,
    *,
    event_count: int,
    exit_after_ms: int,
    routine: int,
) -> tuple[bytes, dict[str, int]]:
    asm = f"""
bits 32
org 0x{stub_va:08x}

entry_xgetdevices:
    jmp xgetdevices_impl
entry_xgetdevicechanges:
    jmp xgetdevicechanges_impl
entry_xinputopen:
    jmp xinputopen_impl
entry_xinputgetcaps:
    jmp xinputgetcaps_impl
entry_xinputsetstate:
    jmp xinputsetstate_impl
entry_xinputgetstate:
    jmp xinputgetstate_impl
entry_xinputclose:
    jmp xinputclose_impl

xgetdevices_impl:
    mov eax, 1
    ret 4

xgetdevicechanges_impl:
    mov edx, [esp+8]
    test edx, edx
    jz .skip_insertions
    mov dword [edx], 0
.skip_insertions:
    mov edx, [esp+12]
    test edx, edx
    jz .skip_removals
    mov dword [edx], 0
.skip_removals:
    xor eax, eax
    ret 12

xinputopen_impl:
    mov eax, 0x0a110ce0
    ret 16

xinputgetcaps_impl:
    push edi
    mov edi, [esp+12]
    test edi, edi
    jz .done
    xor eax, eax
    mov ecx, 25
    rep stosb
    mov edi, [esp+12]
    mov byte [edi], 1
    mov word [edi+3], 0x00ff
    mov dword [edi+5], 0xffffffff
    mov dword [edi+9], 0xffffffff
.done:
    xor eax, eax
    pop edi
    ret 8

xinputsetstate_impl:
    xor eax, eax
    ret 8

xinputclose_impl:
    ret 4

xinputgetstate_impl:
    push esi
    push edi
    push ebx
    mov edi, [esp+20]
    test edi, edi
    jz .return_success
    inc dword [call_count]
    cmp dword [initialized], 0
    jne .have_start
    mov eax, [0x{PROJECT_XBOX_EXPORTS["KeTickCount"]:08x}]
    mov [start_tick], eax
    mov dword [initialized], 1
.have_start:
    mov eax, [0x{PROJECT_XBOX_EXPORTS["KeTickCount"]:08x}]
    sub eax, [start_tick]
    mov [elapsed_ms], eax
    cmp eax, {exit_after_ms}
    jb .process_events
    push dword {routine}
    mov eax, 0x{PROJECT_XBOX_EXPORTS["HalReturnToFirmware"]:08x}
    call eax
.halt:
    jmp .halt

.process_events:
    mov esi, [next_event_idx]
.event_loop:
    cmp esi, {event_count}
    jae .store_idx_fill
    mov ebx, [route_events + esi*8]
    cmp ebx, eax
    ja .store_idx_fill
    movzx edx, byte [route_events + esi*8 + 4]
    movsx ecx, word [route_events + esi*8 + 6]
    cmp edx, 8
    jb .apply_analog
    cmp edx, 16
    jb .apply_binary
    cmp edx, 16
    je .apply_lx
    cmp edx, 17
    je .apply_ly
    cmp edx, 18
    je .apply_rx
    cmp edx, 19
    je .apply_ry
    jmp .advance_event
.apply_analog:
    mov [current_gamepad + 2 + edx], cl
    jmp .advance_event
.apply_binary:
    sub edx, 8
    mov bl, [button_masks + edx]
    cmp cx, 0
    je .clear_binary
    or [current_gamepad], bl
    jmp .advance_event
.clear_binary:
    not bl
    and [current_gamepad], bl
    jmp .advance_event
.apply_lx:
    mov [current_gamepad + 10], cx
    jmp .advance_event
.apply_ly:
    mov [current_gamepad + 12], cx
    jmp .advance_event
.apply_rx:
    mov [current_gamepad + 14], cx
    jmp .advance_event
.apply_ry:
    mov [current_gamepad + 16], cx
.advance_event:
    inc esi
    jmp .event_loop

.store_idx_fill:
    mov [next_event_idx], esi
    mov eax, [call_count]
    mov [edi], eax
    lea esi, [current_gamepad]
    lea edi, [edi+4]
    mov ecx, 4
    rep movsd
    movsw
.return_success:
    xor eax, eax
    pop ebx
    pop edi
    pop esi
    ret 8

align 4
initialized: dd 0
start_tick: dd 0
elapsed_ms: dd 0
call_count: dd 0
next_event_idx: dd 0
current_gamepad:
    dw 0
    times 8 db 0
    dw 0, 0, 0, 0
button_masks:
    db 1, 2, 4, 8, 16, 32, 64, 128
route_events:
{asm_hex(route_blob)}
    db "XEMU_ORACLE_XINPUT_ROUTE", 0
"""
    blob = assemble_nasm(asm)
    entries = {
        "xgetdevices": stub_va + 0,
        "xgetdevicechanges": stub_va + 5,
        "xinputopen": stub_va + 10,
        "xinputgetcaps": stub_va + 15,
        "xinputsetstate": stub_va + 20,
        "xinputgetstate": stub_va + 25,
        "xinputclose": stub_va + 30,
    }
    return blob, entries


def write_rel32_jump(data: bytearray, src_off: int, src_va: int, dst_va: int) -> bytes:
    old = bytes(data[src_off:src_off + 5])
    rel = dst_va - (src_va + 5)
    if not -(1 << 31) <= rel < (1 << 31):
        raise ValueError(f"jump from 0x{src_va:08x} to 0x{dst_va:08x} is out of range")
    data[src_off:src_off + 5] = b"\xE9" + struct.pack("<i", rel)
    return old


def build_return_stub(
    delay_ms: int,
    routine: int,
    hal_return_va: int,
    stall_va: int,
) -> bytes:
    if delay_ms < 1:
        raise ValueError("--delay-ms must be >= 1")
    code = bytearray()
    code += b"\xBE" + struct.pack("<I", delay_ms)      # mov esi, delay_ms
    loop_start = len(code)
    code += b"\xB8" + struct.pack("<I", stall_va)      # mov eax, KeStall...
    code += b"\x68" + struct.pack("<I", 1000)          # push 1000 usec
    code += b"\xFF\xD0"                                # call eax
    code += b"\x4E"                                    # dec esi
    rel = loop_start - (len(code) + 2)
    if not -128 <= rel <= 127:
        raise AssertionError("return stub loop grew beyond rel8 range")
    code += b"\x75" + struct.pack("b", rel)            # jnz loop_start
    code += b"\x68" + struct.pack("<I", routine)       # push routine enum
    code += b"\xB8" + struct.pack("<I", hal_return_va) # mov eax, HalReturn...
    code += b"\xFF\xD0"                                # call eax
    code += b"\xEB\xFE"                                # hang if it returns
    code += b"XEMU_ORACLE_RETURN_ONLY\0"
    while len(code) % 16:
        code += b"\x90"
    return bytes(code)


def update_section_digest(data: bytearray, section: dict[str, Any]) -> str:
    raw_off = section["raw_offset"]
    raw_size = section["raw_size"]
    payload = struct.pack("<I", raw_size) + bytes(data[raw_off:raw_off + raw_size])
    digest = hashlib.sha1(payload).digest()
    data[section["header_off"] + 0x24:section["header_off"] + 0x24 + 20] = digest
    return digest.hex()


def patch_return_only(
    src: Path,
    dst: Path,
    *,
    delay_ms: int,
    routine_name: str,
    expected_sha256: str | None,
    expected_title_id: str | None,
    force: bool,
) -> dict[str, Any]:
    if dst.exists() and not force:
        raise FileExistsError(f"{dst} exists; pass --force to overwrite")
    if not src.exists():
        raise FileNotFoundError(src)

    src_sha = sha256_file(src)
    if expected_sha256 and src_sha.lower() != expected_sha256.lower():
        raise ValueError(
            f"{src}: sha256 mismatch: got {src_sha}, expected {expected_sha256}"
        )

    data = bytearray(src.read_bytes())
    if len(data) < 0x180 or u32(data, 0) != XBE_MAGIC:
        raise ValueError(f"{src}: not an XBE")
    base = u32(data, 0x104)
    cert = parse_certificate(data, base)
    if expected_title_id and cert["title_id"].lower() != expected_title_id.lower():
        raise ValueError(
            f"{src}: title_id mismatch: got {cert['title_id']}, "
            f"expected {expected_title_id}"
        )

    sections = parse_sections(data, base)
    last = sections[-1]
    stub = build_return_stub(
        delay_ms,
        FIRMWARE_REENTRY[routine_name],
        PROJECT_XBOX_EXPORTS["HalReturnToFirmware"],
        PROJECT_XBOX_EXPORTS["KeStallExecutionProcessor"],
    )

    stub_raw_off = last["raw_offset"] + last["raw_size"]
    stub_va = last["virtual_address"] + last["raw_size"]
    needed = stub_raw_off + len(stub)
    if needed > len(data):
        data.extend(b"\0" * (needed - len(data)))
    data[stub_raw_off:stub_raw_off + len(stub)] = stub

    last["raw_size"] += len(stub)
    last["virtual_size"] = max(last["virtual_size"], last["raw_size"])
    # The entrypoint runs before title code has a chance to demand-load later
    # sections. Ensure the extended tail section is both preloaded and
    # executable; otherwise the loader can jump into an unmapped page.
    last["flags"] |= 0x6
    put_u32(data, last["header_off"] + 0x00, last["flags"])
    put_u32(data, last["header_off"] + 0x08, last["virtual_size"])
    put_u32(data, last["header_off"] + 0x10, last["raw_size"])

    image_end = last["virtual_address"] + last["virtual_size"] - base
    put_u32(data, 0x10C, max(u32(data, 0x10C), image_end))
    old_entry = u32(data, 0x128) ^ XOR_EP_RETAIL
    put_u32(data, 0x128, stub_va ^ XOR_EP_RETAIL)
    digest = update_section_digest(data, last)

    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(data)
    dst_sha = hashlib.sha256(data).hexdigest()
    meta = {
        "schema": "retail-title-patch-v1",
        "mode": "return-only",
        "created_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "source": str(src),
        "output": str(dst),
        "source_sha256": src_sha,
        "output_sha256": dst_sha,
        "certificate": cert,
        "old_entry_retail": f"0x{old_entry:08x}",
        "new_entry_retail": f"0x{stub_va:08x}",
        "stub_raw_offset": f"0x{stub_raw_off:08x}",
        "stub_size": len(stub),
        "patched_section_index": last["index"],
        "patched_section_digest_sha1": digest,
        "delay_ms": delay_ms,
        "firmware_reentry": routine_name,
        "kernel_exports": {
            k: f"0x{v:08x}" for k, v in PROJECT_XBOX_EXPORTS.items()
        },
    }
    meta_path = dst.with_suffix(dst.suffix + ".patch.json")
    meta_path.write_text(json.dumps(meta, indent=2, sort_keys=True) + "\n",
                         encoding="utf-8")
    return meta


def patch_xinput_automation(
    src: Path,
    dst: Path,
    *,
    events: list[tuple[int, str, int]],
    route_source: str,
    exit_after_ms: int,
    device_mode: str,
    routine_name: str,
    expected_sha256: str | None,
    expected_title_id: str | None,
    force: bool,
) -> dict[str, Any]:
    if dst.exists() and not force:
        raise FileExistsError(f"{dst} exists; pass --force to overwrite")
    if not src.exists():
        raise FileNotFoundError(src)
    if not events:
        raise ValueError("automation route has no events")

    src_sha = sha256_file(src)
    if expected_sha256 and src_sha.lower() != expected_sha256.lower():
        raise ValueError(
            f"{src}: sha256 mismatch: got {src_sha}, expected {expected_sha256}"
        )

    data = bytearray(src.read_bytes())
    if len(data) < 0x180 or u32(data, 0) != XBE_MAGIC:
        raise ValueError(f"{src}: not an XBE")
    base = u32(data, 0x104)
    cert = parse_certificate(data, base)
    if expected_title_id and cert["title_id"].lower() != expected_title_id.lower():
        raise ValueError(
            f"{src}: title_id mismatch: got {cert['title_id']}, "
            f"expected {expected_title_id}"
        )

    sections = parse_sections(data, base)
    hooks = find_hook_offsets(data, sections)
    state_hook = hooks.get("xinputgetstate")
    if not state_hook or state_hook.get("status") == "ambiguous":
        raise ValueError(f"{src}: could not locate a unique XInputGetState hook")
    if device_mode == "fake":
        open_hook = hooks.get("xinputopen")
        if not open_hook or open_hook.get("status") == "ambiguous":
            raise ValueError(f"{src}: could not locate a unique XInputOpen hook")
        setstate_hook = hooks.get("xinputsetstate")
        if not setstate_hook or setstate_hook.get("status") == "ambiguous":
            raise ValueError(f"{src}: could not locate a unique XInputSetState hook")

    route_blob = encode_route_events(events)
    if exit_after_ms <= events[-1][0]:
        raise ValueError("--exit-after-ms must be after the final route event")

    last = sections[-1]
    stub_raw_off = last["raw_offset"] + last["raw_size"]
    stub_va = last["virtual_address"] + last["raw_size"]
    stub, entries = build_automation_stub(
        stub_va,
        route_blob,
        event_count=len(events),
        exit_after_ms=exit_after_ms,
        routine=FIRMWARE_REENTRY[routine_name],
    )

    needed = stub_raw_off + len(stub)
    if needed > len(data):
        data.extend(b"\0" * (needed - len(data)))
    data[stub_raw_off:stub_raw_off + len(stub)] = stub

    last["raw_size"] += len(stub)
    last["virtual_size"] = max(last["virtual_size"], last["raw_size"])
    # The hook thunk is reached from ordinary title code, so its section must
    # be resident, executable, and writable for the route cursor/state fields.
    last["flags"] |= 0x7
    put_u32(data, last["header_off"] + 0x00, last["flags"])
    put_u32(data, last["header_off"] + 0x08, last["virtual_size"])
    put_u32(data, last["header_off"] + 0x10, last["raw_size"])
    image_end = last["virtual_address"] + last["virtual_size"] - base
    put_u32(data, 0x10C, max(u32(data, 0x10C), image_end))

    patched_hooks: dict[str, Any] = {}
    fake_device_hooks = {"xgetdevices", "xgetdevicechanges", "xinputopen", "xinputclose"}
    for hook_name, entry_va in entries.items():
        if device_mode != "fake" and hook_name in fake_device_hooks:
            continue
        hook = hooks.get(hook_name)
        if not hook or hook.get("status") == "ambiguous":
            continue
        old = write_rel32_jump(
            data,
            hook["file_offset"],
            hook["virtual_address"],
            entry_va,
        )
        patched_hooks[hook_name] = {
            "signature": hook["signature"],
            "file_offset": f"0x{hook['file_offset']:08x}",
            "virtual_address": f"0x{hook['virtual_address']:08x}",
            "stub_entry": f"0x{entry_va:08x}",
            "original_first_5": old.hex(),
        }

    digest = update_section_digest(data, last)
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(data)
    dst_sha = hashlib.sha256(data).hexdigest()
    summary = route_summary(events, route_source)
    meta = {
        "schema": "retail-title-patch-v1",
        "mode": "xinput-automation",
        "created_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "source": str(src),
        "output": str(dst),
        "source_sha256": src_sha,
        "output_sha256": dst_sha,
        "certificate": cert,
        "entry_retail": f"0x{u32(data, 0x128) ^ XOR_EP_RETAIL:08x}",
        "stub_raw_offset": f"0x{stub_raw_off:08x}",
        "stub_va": f"0x{stub_va:08x}",
        "stub_size": len(stub),
        "patched_section_index": last["index"],
        "patched_section_flags": f"0x{last['flags']:08x}",
        "patched_section_digest_sha1": digest,
        "hooks": patched_hooks,
        "hook_scan": {
            name: (
                row if row.get("status") == "ambiguous"
                else {
                    "signature": row["signature"],
                    "file_offset": f"0x{row['file_offset']:08x}",
                    "virtual_address": f"0x{row['virtual_address']:08x}",
                }
            )
            for name, row in hooks.items()
        },
        "route": summary,
        "route_blob_size": len(route_blob),
        "exit_after_ms": exit_after_ms,
        "device_mode": device_mode,
        "firmware_reentry": routine_name,
        "kernel_exports": {
            k: f"0x{v:08x}" for k, v in PROJECT_XBOX_EXPORTS.items()
        },
    }
    meta_path = dst.with_suffix(dst.suffix + ".patch.json")
    meta_path.write_text(json.dumps(meta, indent=2, sort_keys=True) + "\n",
                         encoding="utf-8")
    return meta


def resolve_target_source(target: str, library: Path) -> Path:
    return library / TARGETS[target]["source"]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "target",
        choices=sorted(TARGETS) + ["all"],
        help="Known title to patch, or all known titles.",
    )
    parser.add_argument("--library", type=Path, default=DEFAULT_LIBRARY)
    parser.add_argument("--xbe", type=Path,
                        help="Explicit source XBE; only valid with one target.")
    parser.add_argument("--out-dir", type=Path, default=DEFAULT_OUT_ROOT)
    parser.add_argument("--mode", choices=("return-only", "input-proof", "route"),
                        default="return-only")
    parser.add_argument("--input-csv", type=Path,
                        help="Route CSV for --mode route; defaults to target config.")
    parser.add_argument("--delay-ms", type=int, default=5000)
    parser.add_argument("--exit-after-ms", type=int,
                        help="Automation mode exit time from first XInput poll.")
    parser.add_argument("--exit-tail-ms", type=int, default=10000,
                        help="Automation exit tail after the final route event.")
    parser.add_argument("--device-mode", choices=("fake", "physical"), default="fake",
                        help="fake hooks XInput device discovery/open; physical only "
                             "overrides state reads from an already-opened pad.")
    parser.add_argument("--firmware-reentry", choices=sorted(FIRMWARE_REENTRY),
                        default="reboot")
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args(argv)

    targets = sorted(TARGETS) if args.target == "all" else [args.target]
    if args.xbe and len(targets) != 1:
        parser.error("--xbe can only be used with one concrete target")

    run_dir = args.out_dir / time.strftime(
        f"{args.mode}-%Y%m%dT%H%M%SZ", time.gmtime()
    )
    results = []
    for target in targets:
        cfg = TARGETS[target]
        src = args.xbe if args.xbe else resolve_target_source(target, args.library)
        out = run_dir / target / "default.xbe"
        if args.mode == "return-only":
            meta = patch_return_only(
                src,
                out,
                delay_ms=args.delay_ms,
                routine_name=args.firmware_reentry,
                expected_sha256=cfg["sha256"],
                expected_title_id=cfg["title_id"],
                force=args.force,
            )
        else:
            if args.mode == "input-proof":
                events = list(PROOF_EVENTS)
                route_source = "built-in proof pulse route"
            else:
                route_csv = args.input_csv
                if route_csv is None:
                    route_cfg = cfg.get("route_csv")
                    if route_cfg:
                        route_csv = Path(route_cfg)
                if route_csv is None:
                    raise ValueError(
                        f"{target}: --mode route requires --input-csv "
                        "or a configured route_csv"
                    )
                events = parse_input_csv(route_csv)
                route_source = str(route_csv)
            last_ms = events[-1][0] if events else 0
            exit_after_ms = args.exit_after_ms or max(25000, last_ms + args.exit_tail_ms)
            meta = patch_xinput_automation(
                src,
                out,
                events=events,
                route_source=route_source,
                exit_after_ms=exit_after_ms,
                device_mode=args.device_mode,
                routine_name=args.firmware_reentry,
                expected_sha256=cfg["sha256"],
                expected_title_id=cfg["title_id"],
                force=args.force,
            )
        meta["target"] = target
        meta["title"] = cfg["title"]
        meta_path = out.with_suffix(out.suffix + ".patch.json")
        meta_path.write_text(json.dumps(meta, indent=2, sort_keys=True) + "\n",
                             encoding="utf-8")
        results.append(meta)
        print(f"{target}: {out}")

    summary = {
        "schema": "retail-title-patch-summary-v1",
        "mode": args.mode,
        "targets": results,
    }
    summary_path = run_dir / "summary.json"
    summary_path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n",
                            encoding="utf-8")
    print(f"summary: {summary_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
