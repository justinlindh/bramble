#!/usr/bin/env python3
"""Ratchet gate for firmware flash and static-RAM size, per board.

Reads committed ceilings from ci/size-baseline.json and fails when a board's
measured flash image or static DRAM grows more than tolerance_bytes above the
ceiling. Like the coverage baseline, this file NEVER auto-drifts: a developer
updates it deliberately with scripts/ci/update-size-baseline.sh (raising a
ceiling when a change legitimately grows the image, with the reason in the PR)
and commits it. See docs/quality-policy.md.

Usage:
    check_size.py <board> <flash_bytes> <static_ram_bytes>

Exit status is 0 when both measurements are within ceiling + tolerance, 1
otherwise (or on any configuration error, so a broken baseline fails the gate
rather than silently passing). The failure message names exactly what grew and
by how much.
"""
import json
import os
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BASELINE_PATH = os.path.join(REPO_ROOT, "ci", "size-baseline.json")

# RAM-constrained target: a shipped main-task stack overflow was caught only on
# real hardware, and RAM headroom is the documented T1000-E port blocker, so
# static DRAM is tracked as tightly as flash.
METRICS = (
    ("flash_bytes", "app flash (code + rodata)"),
    ("static_ram_bytes", "static DRAM (data + bss)"),
)


def fail(msg):
    print(f"::error::{msg}", file=sys.stderr)
    sys.exit(1)


def human(n):
    return f"{n} B ({n / 1024:.1f} KiB)"


def main():
    if len(sys.argv) != 4:
        fail("usage: check_size.py <board> <flash_bytes> <static_ram_bytes>")
    board = sys.argv[1]
    measured = {}
    try:
        measured["flash_bytes"] = int(sys.argv[2])
        measured["static_ram_bytes"] = int(sys.argv[3])
    except ValueError:
        fail(f"flash/ram must be integers, got {sys.argv[2]!r} {sys.argv[3]!r}")

    try:
        with open(BASELINE_PATH, encoding="utf-8") as fh:
            data = json.load(fh)
    except (OSError, ValueError) as exc:
        fail(f"cannot read {BASELINE_PATH}: {exc}")

    boards = data.get("boards", {})
    if board not in boards:
        fail(f"board '{board}' is not in {BASELINE_PATH} (boards: {sorted(boards)})")
    base = boards[board]

    regressions = []
    for key, label in METRICS:
        got = measured[key]
        ceiling = int(base[key])
        tol = int(base.get("tolerance_bytes", 0))
        limit = ceiling + tol
        delta = got - ceiling
        sign = "+" if delta >= 0 else ""
        status = "OK" if got <= limit else "REGRESSION"
        print(
            f"[size] {board} {label}: measured {human(got)} baseline {human(ceiling)} "
            f"delta {sign}{delta} B tolerance {tol} B -> {status}"
        )
        if got > limit:
            regressions.append(
                f"{label} grew to {human(got)}, {delta} B over the committed "
                f"baseline {human(ceiling)} (limit {human(limit)} after a {tol} B tolerance)"
            )

    if regressions:
        detail = "; ".join(regressions)
        fail(
            f"{board} size regression: {detail}. If the growth is intentional, "
            f"raise the ceiling deliberately: run scripts/ci/update-size-baseline.sh "
            f"and commit ci/size-baseline.json with the reason in the PR."
        )


if __name__ == "__main__":
    main()
