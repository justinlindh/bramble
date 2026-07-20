#!/usr/bin/env python3
"""Ratchet gate for per-suite line coverage.

Reads the committed floor from ci/coverage-baseline.json and fails when a
suite's measured line coverage drops more than tolerance_pct below it. The
baseline never auto-drifts: it is a checked-in file a developer updates
deliberately (raising it after adding tests, or lowering it with a written
justification) via scripts/ci/update-coverage-baseline.sh.

Usage:
    check_coverage.py <suite> <measured_pct>

Exit status is 0 when measured + tolerance >= baseline, 1 otherwise (or on any
configuration error, so a broken baseline file fails the gate rather than
silently passing).
"""
import json
import os
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BASELINE_PATH = os.path.join(REPO_ROOT, "ci", "coverage-baseline.json")


def fail(msg):
    print(f"::error::{msg}", file=sys.stderr)
    sys.exit(1)


def main():
    if len(sys.argv) != 3:
        fail("usage: check_coverage.py <suite> <measured_pct>")
    suite = sys.argv[1]
    try:
        measured = float(sys.argv[2])
    except ValueError:
        fail(f"measured_pct '{sys.argv[2]}' is not a number")

    try:
        with open(BASELINE_PATH, encoding="utf-8") as fh:
            data = json.load(fh)
    except (OSError, ValueError) as exc:
        fail(f"cannot read {BASELINE_PATH}: {exc}")

    tolerance = float(data.get("tolerance_pct", 0.0))
    suites = data.get("suites", {})
    if suite not in suites:
        fail(f"suite '{suite}' is not in {BASELINE_PATH} (suites: {sorted(suites)})")
    baseline = float(suites[suite])

    floor = baseline - tolerance
    status = "OK" if measured >= floor else "REGRESSION"
    print(
        f"[coverage] suite={suite} measured={measured:.2f}% "
        f"baseline={baseline:.2f}% tolerance={tolerance:.2f}pp floor={floor:.2f}% -> {status}"
    )

    if measured < floor:
        fail(
            f"coverage for '{suite}' dropped to {measured:.2f}%, below the "
            f"committed baseline {baseline:.2f}% (floor {floor:.2f}% after a "
            f"{tolerance:.2f}pp tolerance). A change removed test coverage. Add "
            f"tests to restore it, or if the drop is intentional lower the baseline "
            f"deliberately: run scripts/ci/update-coverage-baseline.sh and commit "
            f"ci/coverage-baseline.json with a justification."
        )

    # Encourage (never require) ratcheting the floor up when coverage climbs
    # well past it, so the baseline tracks real gains over time.
    if measured >= baseline + 2.0:
        print(
            f"[coverage] note: {suite} is {measured - baseline:.2f}pp above its "
            f"baseline; consider scripts/ci/update-coverage-baseline.sh to ratchet up."
        )


if __name__ == "__main__":
    main()
