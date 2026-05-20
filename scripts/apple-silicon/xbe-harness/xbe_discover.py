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
    real_xbox_only: bool = False
    # Optional per-XBE list of additional Metal recipe variants the
    # orchestrator runs as extra cells. Each entry is a dict:
    #
    #   {"name": "<short-id>", "env": {"XEMU_*": "value", ...}}
    #
    # The orchestrator runs the canonical Metal recipe AND one extra
    # cell per entry, applying the env overrides on top of the
    # canonical recipe. Used by `crtc-publish` to force a second cell
    # at XEMU_METAL_FRONT_FB_FALLBACK=0 so the matrix exercises BOTH
    # publish paths the XBE distinguishes (the canonical recipe
    # already covers fallback=1).
    additional_metal_recipes: List[dict] = field(default_factory=list)
    # Optional per-renderer min-counter assertion. Schema:
    #
    #   {"gl":    {"NATIVE_QUAD_DRAW": 100, ...},
    #    "metal": {"METAL_NATIVE_QUAD_DRAWS": 100, ...}}
    #
    # After a run on a given renderer, the harness sums the named
    # `xemu-perf:` counters across every interval in xemu.log; the
    # cell PASSes only if every named counter accumulated >= the
    # declared minimum. real-Xbox cells skip this assertion (no
    # xemu.log). Used by `native-quad-tri-depth` to prove that
    # NATIVE_QUAD / NATIVE_TRI_DEPTH bypass paths actually engaged
    # (pixel-only oracles can't distinguish that from a silent GS
    # fallback). See diagnostic-xbe-plan.md v2 §4.5 + xbe-harness
    # README "Counter-based path-activation assertion".
    required_counters_min: Dict[str, Dict[str, int]] = field(
        default_factory=dict)
    # Optional per-XBE pixel-compare gate overrides. Schema:
    #
    #   {"threshold":            <int>,   # per-channel byte threshold
    #    "max_changed_pct":      <float>, # max changed pixels percent
    #    "min_signal_match_pct": <float>} # min reference-non-black match
    #
    # Defaults (when key absent): use the CLI-supplied --threshold /
    # --max-changed-pct and the harness default min_signal_match_pct=99.0.
    # Used by grid-pattern XBEs (e.g. `native-quad-tri-depth`) whose
    # many internal cell-edges produce more retina-downsample AA
    # boundary pixels than the strict defaults tuned for sparse-signal
    # XBEs (mirror / depth-floor / crtc-publish) accept.
    compare_overrides: Dict[str, float] = field(default_factory=dict)
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
        real_xbox_only=bool(data.get("real_xbox_only", False)),
        additional_metal_recipes=list(
            data.get("additional_metal_recipes", [])),
        required_counters_min={
            str(renderer): {
                str(name): int(min_val) for name, min_val in counters.items()
            }
            for renderer, counters in
            data.get("required_counters_min", {}).items()
        },
        compare_overrides={
            str(k): float(v) for k, v in
            data.get("compare_overrides", {}).items()
        },
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
