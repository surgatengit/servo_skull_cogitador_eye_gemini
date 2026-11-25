#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check that the VOX fork of esp-mosaico-bsp differs from the submodule only by the
documented change (third_party/esp-mosaico-bsp/VOX_FORK.md). Exit 0 when it does.

Usage (from the project root): python tools/check_bsp_fork.py
"""

import difflib
import subprocess
import sys
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
FORK = PROJECT / "third_party" / "esp-mosaico-bsp"
UPSTREAM = PROJECT.parents[1] / "submodule" / "esp-mosaico-bsp" / "components" / "esp-mosaico-bsp"
FORK_ONLY = {"VOX_FORK.md"}
PATCHED = "onboard/display.c"
EXPECTED_LINE = "io_config.trans_queue_depth = 2;"


def files(root: Path) -> set[str]:
    return {p.relative_to(root).as_posix() for p in root.rglob("*") if p.is_file()}


# Unmodified copies that must resolve `../esp-mosaico-bsp` to the fork above.
# Their unit-test folders are not copied. Files listed in UPSTREAM_FIXES are taken
# verbatim from a later official esp-mosaico-bsp commit instead of the submodule
# checkout (see third_party/esp-mosaico-bsp/VOX_FORK.md).
VERBATIM = ("mosaico_module_camera", "mosaico_module_mgr")
SKIPPED_DIRS = ("test/",)
UPSTREAM_FIXES = {
    # b60f5d8 (2026-10-19) "fix: update camera app": the module drives CAM_PWDN with
    # board polarity (0 normal, 1 sleep, including SC101IOT); test 13.
    "mosaico_module_camera/mosaico_camera.c": "b60f5d8",
    "mosaico_module_camera/README.md": "b60f5d8",
}
BSP_REPO = UPSTREAM.parents[1]


def reference_bytes(name: str, rel: str) -> bytes:
    commit = UPSTREAM_FIXES.get(f"{name}/{rel}")
    if commit is None:
        return (UPSTREAM.parent / name / rel).read_bytes()
    return subprocess.run(["git", "-C", str(BSP_REPO), "show", f"{commit}:components/{name}/{rel}"],
                          check=True, capture_output=True).stdout


def check_verbatim(name: str) -> list[str]:
    fork, upstream = PROJECT / "third_party" / name, UPSTREAM.parent / name
    if not fork.is_dir():
        return []
    up = {f for f in files(upstream) if not f.startswith(SKIPPED_DIRS)}
    fk = files(fork)
    errors = [f"{name}: only in fork: {f}" for f in sorted(fk - up)]
    errors += [f"{name}: missing in fork: {f}" for f in sorted(up - fk)]
    for f in sorted(fk & up):
        try:
            if (fork / f).read_bytes() != reference_bytes(name, f):
                errors.append(f"{name}: changed: {f}")
        except subprocess.CalledProcessError:
            errors.append(f"{name}: upstream commit for {f} not available (git fetch in the BSP)")
    return errors


def main() -> int:
    if not UPSTREAM.is_dir():
        print(f"submodule BSP not found: {UPSTREAM}")
        return 1
    errors = []
    fork, upstream = files(FORK) - FORK_ONLY, files(UPSTREAM)
    errors += [f"only in fork: {f}" for f in sorted(fork - upstream)]
    errors += [f"missing in fork: {f}" for f in sorted(upstream - fork)]
    for rel in sorted(fork & upstream):
        a = (UPSTREAM / rel).read_bytes()
        b = (FORK / rel).read_bytes()
        if a == b:
            continue
        if rel != PATCHED:
            errors.append(f"unexpected change: {rel}")
            continue
        added = [line[1:].strip() for line in difflib.unified_diff(
            a.decode("utf-8").splitlines(), b.decode("utf-8").splitlines(), lineterm="", n=0)
            if line.startswith("+") and not line.startswith("+++")]
        removed = [line for line in difflib.unified_diff(
            a.decode("utf-8").splitlines(), b.decode("utf-8").splitlines(), lineterm="", n=0)
            if line.startswith("-") and not line.startswith("---")]
        code = [line for line in added if line and not line.startswith(("/*", "*", "//"))]
        if removed or code != [EXPECTED_LINE]:
            errors.append(f"{rel}: expected only the added line '{EXPECTED_LINE}' (plus comments)")
    for name in VERBATIM:
        errors += check_verbatim(name)
    for e in errors:
        print(e)
    print("BSP fork OK" if not errors else f"BSP fork check failed ({len(errors)})")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
