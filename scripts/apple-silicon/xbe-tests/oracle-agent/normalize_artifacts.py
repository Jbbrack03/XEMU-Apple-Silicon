#!/usr/bin/env python3

import argparse
import os
import struct
import sys

FILETIME_TICKS_PER_SECOND = 10_000_000
UNIX_TO_FILETIME_SECONDS = 11_644_473_600

XBE_HEADER_TIME_OFFSET = 0x114
XBE_PE_TIME_OFFSET = 0x148
XBE_CERT_TIME_OFFSET = 0x17C

ISO_FILETIME_OFFSET = 0x10000 + 20 + 4 + 4


def patch_u32le(path: str, offset: int, value: int) -> None:
    with open(path, "r+b") as handle:
        handle.seek(offset)
        handle.write(struct.pack("<I", value))


def patch_u64le(path: str, offset: int, value: int) -> None:
    with open(path, "r+b") as handle:
        handle.seek(offset)
        handle.write(struct.pack("<Q", value))


def to_filetime(epoch: int) -> int:
    return (epoch + UNIX_TO_FILETIME_SECONDS) * FILETIME_TICKS_PER_SECOND


def ensure_exists(path: str) -> None:
    if not os.path.exists(path):
        raise FileNotFoundError(path)


def normalize_xbe(path: str, epoch: int) -> None:
    ensure_exists(path)
    for offset in (XBE_HEADER_TIME_OFFSET, XBE_PE_TIME_OFFSET, XBE_CERT_TIME_OFFSET):
        patch_u32le(path, offset, epoch)


def normalize_iso(path: str, epoch: int) -> None:
    ensure_exists(path)
    patch_u64le(path, ISO_FILETIME_OFFSET, to_filetime(epoch))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Normalize oracle-agent packaged artifact timestamps for deterministic rebuilds."
    )
    parser.add_argument("--epoch", type=int, required=True, help="Unix epoch seconds to embed.")
    parser.add_argument("--xbe", help="Path to the XBE to normalize.")
    parser.add_argument("--iso", help="Path to the ISO to normalize.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    if args.epoch < 0 or args.epoch > 0xFFFFFFFF:
        print("epoch must fit in an unsigned 32-bit XBE timestamp field", file=sys.stderr)
        return 1

    if not args.xbe and not args.iso:
        print("at least one of --xbe or --iso is required", file=sys.stderr)
        return 1

    if args.xbe:
        normalize_xbe(args.xbe, args.epoch)
    if args.iso:
        normalize_iso(args.iso, args.epoch)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
