#!/usr/bin/env python3
"""Dump Xbox kernel PE exports through the oracle agent.

This is deliberately narrow: it probes only known kernel base candidates,
parses the PE export directory, and reads only the export tables / names.
It does not scan RAM. The output is a JSON + TSV symbol map that can drive
future retail-game input work without guessing at kernel entry points.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
import struct
import sys
from typing import Any, Callable


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_OUT = ROOT.parent / "xbox-oracle-backup/2026-05-06/kernel-symbols"
# Keep defaults conservative. 0x80010000 is the common loaded kernel base on
# the project Xbox; 0x80000000 is a cheap sanity candidate. Do not probe broad
# high-memory ranges by default: some invalid reads can leave the oracle agent
# slow to respond until relaunched.
BASE_CANDIDATES = (0x80010000, 0x80000000)


def load_oracle_client():
    path = ROOT / "scripts/apple-silicon/oracle-client.py"
    spec = importlib.util.spec_from_file_location("oracle_client", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load {path}")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)  # type: ignore[union-attr]
    return mod


class RemoteImage:
    def __init__(self, oracle: Any, base: int):
        self.oracle = oracle
        self.base = base

    def read_rva(self, rva: int, size: int) -> bytes:
        if size < 0 or size > 1024 * 1024:
            raise ValueError(f"refusing unreasonable read size: {size}")
        return self.oracle.mem_read(self.base + rva, size)


class OfflineImage:
    def __init__(self, data: bytes):
        self.data = data
        self.sections: list[dict[str, int | str]] = []

    def set_sections(self, sections: list[dict[str, int | str]]) -> None:
        self.sections = sections

    def _rva_to_off(self, rva: int) -> int:
        for sec in self.sections:
            va = int(sec["virtual_address"])
            raw = int(sec["raw_pointer"])
            raw_size = int(sec["raw_size"])
            virt_size = max(int(sec["virtual_size"]), raw_size)
            if va <= rva < va + virt_size:
                return raw + (rva - va)
        return rva

    def read_rva(self, rva: int, size: int) -> bytes:
        off = self._rva_to_off(rva)
        if off < 0 or size < 0 or off + size > len(self.data):
            raise ValueError(f"RVA 0x{rva:x} size {size} outside offline image")
        return self.data[off: off + size]


def u16(data: bytes, off: int) -> int:
    return struct.unpack_from("<H", data, off)[0]


def u32(data: bytes, off: int) -> int:
    return struct.unpack_from("<I", data, off)[0]


def read_cstr(read_rva: Callable[[int, int], bytes], rva: int, limit: int = 256) -> str:
    out = bytearray()
    for off in range(0, limit, 64):
        chunk = read_rva(rva + off, min(64, limit - off))
        nul = chunk.find(b"\0")
        if nul >= 0:
            out.extend(chunk[:nul])
            break
        out.extend(chunk)
    return bytes(out).decode("ascii", "replace")


def parse_headers(read_rva: Callable[[int, int], bytes]) -> dict[str, Any]:
    dos = read_rva(0, 0x1000)
    if dos[:2] != b"MZ":
        raise ValueError("missing MZ header")
    pe_off = u32(dos, 0x3C)
    if pe_off < 0x40 or pe_off > 0x10000:
        raise ValueError(f"unreasonable PE header offset 0x{pe_off:x}")
    hdr = read_rva(0, pe_off + 0x400)
    if hdr[pe_off:pe_off + 4] != b"PE\0\0":
        raise ValueError("missing PE signature")
    file_header = pe_off + 4
    machine = u16(hdr, file_header)
    section_count = u16(hdr, file_header + 2)
    timestamp = u32(hdr, file_header + 4)
    optional_size = u16(hdr, file_header + 16)
    optional = file_header + 20
    magic = u16(hdr, optional)
    if magic != 0x10B:
        raise ValueError(f"unsupported optional-header magic 0x{magic:x}")
    image_base = u32(hdr, optional + 28)
    export_rva = u32(hdr, optional + 96)
    export_size = u32(hdr, optional + 100)
    section_table = optional + optional_size
    sections: list[dict[str, int | str]] = []
    for i in range(section_count):
        off = section_table + i * 40
        name = hdr[off:off + 8].split(b"\0", 1)[0].decode("ascii", "replace")
        sections.append({
            "name": name,
            "virtual_size": u32(hdr, off + 8),
            "virtual_address": u32(hdr, off + 12),
            "raw_size": u32(hdr, off + 16),
            "raw_pointer": u32(hdr, off + 20),
        })
    return {
        "machine": machine,
        "timestamp": timestamp,
        "image_base": image_base,
        "section_count": section_count,
        "sections": sections,
        "export_rva": export_rva,
        "export_size": export_size,
    }


def parse_exports(image: RemoteImage | OfflineImage) -> dict[str, Any]:
    headers = parse_headers(image.read_rva)
    if isinstance(image, OfflineImage):
        image.set_sections(headers["sections"])
        headers = parse_headers(image.read_rva)
    export_rva = int(headers["export_rva"])
    if export_rva == 0:
        raise ValueError("PE export directory absent")
    exp = image.read_rva(export_rva, 40)
    ordinal_base = u32(exp, 16)
    function_count = min(u32(exp, 20), 8192)
    name_count = min(u32(exp, 24), 8192)
    if function_count == 0:
        raise ValueError(
            f"empty export table: functions={function_count} names={name_count}")
    funcs_rva = u32(exp, 28)
    names_rva = u32(exp, 32)
    ords_rva = u32(exp, 36)
    dll_name = read_cstr(image.read_rva, u32(exp, 12))

    funcs = image.read_rva(funcs_rva, function_count * 4)
    symbols: list[dict[str, Any]] = []
    if name_count > 0:
        names = image.read_rva(names_rva, name_count * 4)
        ords = image.read_rva(ords_rva, name_count * 2)
        for i in range(name_count):
            name_rva = u32(names, i * 4)
            ord_index = u16(ords, i * 2)
            if ord_index >= function_count:
                continue
            func_rva = u32(funcs, ord_index * 4)
            symbols.append({
                "name": read_cstr(image.read_rva, name_rva),
                "ordinal": ordinal_base + ord_index,
                "rva": func_rva,
                "va": image.base + func_rva if isinstance(image, RemoteImage) else int(headers["image_base"]) + func_rva,
            })
    else:
        # Retail Xbox kernels export by ordinal. Preserve the table instead of
        # inventing names; downstream tooling can join ordinals against nxdk's
        # xboxkrnl ordinal map.
        for ord_index in range(function_count):
            func_rva = u32(funcs, ord_index * 4)
            symbols.append({
                "name": f"ordinal_{ordinal_base + ord_index}",
                "ordinal": ordinal_base + ord_index,
                "rva": func_rva,
                "va": image.base + func_rva if isinstance(image, RemoteImage) else int(headers["image_base"]) + func_rva,
            })
    symbols.sort(key=lambda row: (row["ordinal"], row["name"]))
    return {
        "headers": headers,
        "export_name": dll_name,
        "ordinal_base": ordinal_base,
        "function_count": function_count,
        "name_count": name_count,
        "symbols": symbols,
    }


def dump_outputs(out_dir: Path, payload: dict[str, Any], stem: str) -> tuple[Path, Path]:
    out_dir.mkdir(parents=True, exist_ok=True)
    json_path = out_dir / f"{stem}.json"
    tsv_path = out_dir / f"{stem}.tsv"
    json_path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    with tsv_path.open("w", encoding="utf-8") as f:
        f.write("ordinal\tva\trva\tname\n")
        for row in payload["exports"]["symbols"]:
            f.write(f"{row['ordinal']}\t0x{row['va']:08x}\t0x{row['rva']:08x}\t{row['name']}\n")
    return json_path, tsv_path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="192.168.0.200")
    parser.add_argument("--port", type=int, default=9001)
    parser.add_argument("--timeout", type=float, default=15.0)
    parser.add_argument("--base", type=lambda s: int(s, 0), action="append",
                        help="Kernel base candidate. May be repeated.")
    parser.add_argument("--offline-image", type=Path,
                        help="Parse a local kernel PE image instead of oracle memory.")
    parser.add_argument("--out-dir", type=Path, default=DEFAULT_OUT)
    args = parser.parse_args()

    if args.offline_image:
        data = args.offline_image.read_bytes()
        image = OfflineImage(data)
        exports = parse_exports(image)
        payload = {
            "schema": "xbox-kernel-symbol-dump-v1",
            "source": str(args.offline_image.resolve()),
            "base": exports["headers"]["image_base"],
            "exports": exports,
        }
        paths = dump_outputs(args.out_dir, payload, "xboxkrnl-offline-exports")
        print(f"wrote {paths[0]}")
        print(f"wrote {paths[1]}")
        return 0

    oracle_mod = load_oracle_client()
    bases = tuple(args.base or BASE_CANDIDATES)
    errors: list[str] = []
    with oracle_mod.OracleClient(args.host, args.port, args.timeout) as oracle:
        for base in bases:
            try:
                image = RemoteImage(oracle, base)
                exports = parse_exports(image)
                payload = {
                    "schema": "xbox-kernel-symbol-dump-v1",
                    "source": f"oracle://{args.host}:{args.port}",
                    "base": base,
                    "probed_bases": [f"0x{b:08x}" for b in bases],
                    "exports": exports,
                }
                paths = dump_outputs(args.out_dir, payload, f"xboxkrnl-0x{base:08x}-exports")
                print(f"base 0x{base:08x}: {len(exports['symbols'])} exports")
                print(f"wrote {paths[0]}")
                print(f"wrote {paths[1]}")
                return 0
            except Exception as exc:
                errors.append(f"0x{base:08x}: {exc}")
    print("kernel export dump failed:", file=sys.stderr)
    for err in errors:
        print(f"  {err}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
