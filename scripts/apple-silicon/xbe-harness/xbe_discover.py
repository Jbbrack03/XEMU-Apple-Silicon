"""
xbe_discover — locate diag-XBE manifests under `xbe-tests/`.

Each diag XBE lives at `xbe-tests/<id>/` with manifest.json,
expected.py, main.c, Makefile, README.md, and bin/default.xbe (after
`make`). This module walks the tree and returns a list of XBE
descriptors the orchestrator can iterate.
"""
from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional


_HERE = Path(__file__).resolve().parent
XBE_TESTS_DIR = (_HERE.parent / "xbe-tests").resolve()


@dataclass
class XbeManifest:
    id: str
    title: str
    self_validation_tier: int
    oracle_priority: List[str]
    requires_flags: List[str]
    expected_results: Dict[str, dict]
    artifacts: dict
    duration_seconds: int
    capture_at_flip_stall_ordinal: Optional[int]
    expected_fail_renderers: List[str] = field(default_factory=list)
    raw: dict = field(default_factory=dict)
    dir: Path = field(default=Path("."))

    @property
    def iso_path(self) -> Path:
        return self.dir / f"{self.id}.iso"

    @property
    def xbe_path(self) -> Path:
        return self.dir / "bin" / "default.xbe"

    @property
    def expected_py(self) -> Path:
        return self.dir / "expected.py"

    @property
    def real_xbox_reference_dir(self) -> Path:
        """Where canonical real-Xbox reference frames live for this XBE.
        Created on demand by `xbe-harness capture-reference`."""
        return (_HERE.parent.parent.parent / "docs" / "apple-silicon"
                / "xbox-real-references" / self.id).resolve()


def discover_all(only: Optional[List[str]] = None) -> List[XbeManifest]:
    """Walk `xbe-tests/<id>/manifest.json`. Skips `lib/`, `oracle-agent/`,
    and other non-XBE subdirectories. Returns alphabetical by id.

    `only`: optional list of XBE ids; when set, returns only matching
    XBEs in the order they appear in `only`."""
    out: List[XbeManifest] = []
    if not XBE_TESTS_DIR.is_dir():
        return out
    for d in sorted(XBE_TESTS_DIR.iterdir()):
        if not d.is_dir():
            continue
        m = d / "manifest.json"
        if not m.exists():
            continue
        try:
            data = json.loads(m.read_text())
        except json.JSONDecodeError as e:
            raise RuntimeError(f"manifest {m}: {e}") from e
        out.append(_from_dict(data, d))
    if only:
        order = {name: i for i, name in enumerate(only)}
        out = [m for m in out if m.id in order]
        out.sort(key=lambda m: order[m.id])
    return out


def get(xbe_id: str) -> XbeManifest:
    """Return the single XBE manifest matching `xbe_id`. Raises
    KeyError if absent."""
    for m in discover_all():
        if m.id == xbe_id:
            return m
    raise KeyError(f"no xbe-tests/{xbe_id}/manifest.json found")


def _from_dict(data: dict, d: Path) -> XbeManifest:
    return XbeManifest(
        id=data["id"],
        title=data.get("title", ""),
        self_validation_tier=int(data.get("self_validation_tier", 1)),
        oracle_priority=list(data.get("oracle_priority", [])),
        requires_flags=list(data.get("requires_flags", [])),
        expected_results=dict(data.get("expected_results", {})),
        artifacts=dict(data.get("artifacts", {})),
        duration_seconds=int(data.get("duration_seconds", 4)),
        capture_at_flip_stall_ordinal=data.get("capture_at_flip_stall_ordinal"),
        expected_fail_renderers=list(data.get("expected_fail_renderers", [])),
        raw=data,
        dir=d,
    )


if __name__ == "__main__":
    import sys
    items = discover_all()
    if not items:
        print("no XBEs discovered (looked under "
              f"{XBE_TESTS_DIR})", file=sys.stderr)
        sys.exit(1)
    for m in items:
        built = "BUILT" if m.xbe_path.exists() else "not-built"
        print(f"{m.id:18}  tier={m.self_validation_tier}  {built}  {m.title}")
