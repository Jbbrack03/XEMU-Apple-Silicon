#!/usr/bin/env python3
"""
Tier-1 FTP mirror for the OG Xbox at 192.168.0.200.

Walks the requested remote directories, downloads every file, builds a
SHA-256 manifest. Skips transient/replaceable trees by name (CACHE, etc.).

Outputs:
  <local_root>/                     mirrored tree
  <local_root>/MANIFEST.tsv         path \t size \t sha256
  <local_root>/SKIPPED.tsv          path \t reason (for transparency)
  <local_root>/mirror.log           diagnostic log
"""
import argparse
import hashlib
import io
import os
import re
import sys
import time
from datetime import datetime
from ftplib import FTP, error_perm

HOST = "192.168.0.200"
USER = "xbox"
PASS = "xbox"

# Skip these directory names anywhere in the tree (transient or replaceable):
SKIP_DIRS = {"CACHE", "Cache", "cache"}

LIST_RE = re.compile(
    r'^([d-])[rwx-]{9}\s+\d+\s+\S+\s+\S+\s+(\d+)\s+\S+\s+\S+\s+\S+\s+(.*)$'
)


def parse_listing(line: str):
    m = LIST_RE.match(line.rstrip("\r\n"))
    if not m:
        return None
    is_dir = m.group(1) == "d"
    size = int(m.group(2))
    name = m.group(3)
    return is_dir, size, name


def mirror(ftp: FTP, remote_path: str, local_path: str, manifest, skipped, log):
    os.makedirs(local_path, exist_ok=True)
    try:
        ftp.cwd(remote_path)
    except error_perm as e:
        log.write(f"CWD failed for {remote_path}: {e}\n")
        return

    entries = []
    ftp.retrlines("LIST", entries.append)

    for line in entries:
        parsed = parse_listing(line)
        if not parsed:
            log.write(f"unparsed: {line}\n")
            continue
        is_dir, size, name = parsed
        if name in (".", ".."):
            continue
        remote_child = remote_path.rstrip("/") + "/" + name
        local_child = os.path.join(local_path, name)
        if is_dir:
            if name in SKIP_DIRS:
                skipped.write(f"{remote_child}\tdir-skip-rule\n")
                log.write(f"SKIP dir {remote_child}\n")
                continue
            log.write(f"DIR  {remote_child}\n")
            mirror(ftp, remote_child, local_child, manifest, skipped, log)
            ftp.cwd(remote_path)  # restore CWD on the way back up
        else:
            # Download to a temp file, verify byte count vs LIST-reported
            # size, then atomically rename and only then write the
            # manifest entry. A partial transfer (network glitch / FTP
            # close mid-stream) must not leave a misleading manifest.
            t0 = time.time()
            sha = hashlib.sha256()
            counter = {"n": 0}
            tmp_path = local_child + ".part"
            with open(tmp_path, "wb") as f:
                def writer(chunk):
                    f.write(chunk)
                    sha.update(chunk)
                    counter["n"] += len(chunk)
                ftp.retrbinary(f"RETR {name}", writer)
            elapsed = time.time() - t0
            received = counter["n"]
            if size != received:
                skipped.write(
                    f"{remote_child}\tsize-mismatch:expected={size}:got={received}\n"
                )
                skipped.flush()
                log.write(
                    f"FILE {remote_child} SIZE-MISMATCH expected={size} got={received} elapsed={elapsed:.2f}s (kept as {tmp_path})\n"
                )
                log.flush()
                continue
            os.replace(tmp_path, local_child)
            digest = sha.hexdigest()
            manifest.write(f"{remote_child}\t{size}\t{digest}\n")
            manifest.flush()
            log.write(f"FILE {remote_child} ({size} B in {elapsed:.2f}s)\n")
            log.flush()


def inventory_only(ftp: FTP, remote_path: str, manifest_path: str, log):
    """Walk a remote subtree but only record path+size, no download."""
    with open(manifest_path, "w") as inv:
        inv.write("path\tsize\ttype\n")

        def walk(path):
            try:
                ftp.cwd(path)
            except error_perm as e:
                log.write(f"CWD fail {path}: {e}\n")
                return
            entries = []
            ftp.retrlines("LIST", entries.append)
            for line in entries:
                p = parse_listing(line)
                if not p:
                    continue
                is_dir, size, name = p
                if name in (".", ".."):
                    continue
                child = path.rstrip("/") + "/" + name
                inv.write(f"{child}\t{size}\t{'dir' if is_dir else 'file'}\n")
                inv.flush()
                if is_dir:
                    walk(child)
                    ftp.cwd(path)

        walk(remote_path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", choices=["mirror", "inventory"], required=True)
    ap.add_argument("--remote", required=True, help="remote path, e.g. /C")
    ap.add_argument("--local", required=True, help="local destination dir")
    ap.add_argument("--label", default=None, help="manifest label suffix")
    args = ap.parse_args()

    os.makedirs(args.local, exist_ok=True)
    label = args.label or args.remote.strip("/").replace("/", "_") or "root"

    log_path = os.path.join(args.local, f"mirror-{label}.log")
    log = open(log_path, "w")
    log.write(f"# Started {datetime.now().isoformat()} mode={args.mode} remote={args.remote}\n")
    log.flush()

    ftp = FTP()
    ftp.connect(HOST, 21, timeout=30)
    ftp.login(USER, PASS)
    log.write(f"# Logged in. Welcome: {ftp.welcome}\n")

    try:
        if args.mode == "mirror":
            manifest_path = os.path.join(args.local, f"MANIFEST-{label}.tsv")
            skipped_path = os.path.join(args.local, f"SKIPPED-{label}.tsv")
            with open(manifest_path, "w") as manifest, open(skipped_path, "w") as skipped:
                manifest.write("path\tsize\tsha256\n")
                skipped.write("path\treason\n")
                mirror(ftp, args.remote, args.local, manifest, skipped, log)
        else:
            manifest_path = os.path.join(args.local, f"INVENTORY-{label}.tsv")
            inventory_only(ftp, args.remote, manifest_path, log)
        log.write(f"# Finished {datetime.now().isoformat()}\n")
    finally:
        log.close()
        try:
            ftp.quit()
        except Exception:
            pass


if __name__ == "__main__":
    main()
