#!/usr/bin/env python3
"""Aggregate host-test line coverage from gcov data with no external tooling.

Only `gcov` (shipped with gcc, already required to build the host suite) and
the Python standard library are used, so this runs on any pod that can compile
the tests. gcovr is deliberately avoided: a required gate must not depend on a
pip install landing on the runner.

The host suite compiles many product sources into more than one test binary, so
the same source file has several independent .gcno/.gcda pairs. A source line is
counted as covered when ANY binary executed it (a union across binaries), which
is the right question for "does the suite exercise this line at all". Coverage is
reported over product code only: files under components/ and main/, excluding the
test harness itself (test/, unity/, stubs/).

Usage:
    host_coverage.py <build_dir> [--repo-root DIR] [--json OUT]

Prints a human summary and, on the final line, the bare line-coverage percentage
(e.g. "85.53") so a shell can capture it with `tail -1`.
"""
import argparse
import gzip
import json
import os
import subprocess
import sys

INCLUDE_PREFIXES = ("components/", "main/")
EXCLUDE_SUBSTRINGS = ("/test/", "/unity/", "/stubs/")


def find_gcda(build_dir):
    for root, _dirs, files in os.walk(build_dir):
        for name in files:
            if name.endswith(".gcda"):
                yield os.path.join(root, name)


def gcov_json(gcda_path, build_dir):
    """Return the parsed gcov JSON for one .gcda file, or None on failure."""
    # --stdout streams one JSON document and avoids output-file name collisions
    # when the same source is instrumented in several object directories.
    proc = subprocess.run(
        ["gcov", "--json-format", "--stdout", os.path.abspath(gcda_path)],
        cwd=build_dir,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    if proc.returncode != 0 or not proc.stdout:
        return None
    raw = proc.stdout
    # Older gcov may gzip even the --stdout stream; detect the gzip magic.
    if raw[:2] == b"\x1f\x8b":
        try:
            raw = gzip.decompress(raw)
        except OSError:
            return None
    try:
        return json.loads(raw.decode("utf-8", "replace"))
    except ValueError:
        return None


def normalize(path, repo_root):
    absolute = os.path.normpath(
        path if os.path.isabs(path) else os.path.join(repo_root, path)
    )
    rel = os.path.relpath(absolute, repo_root)
    return absolute, rel


def is_product_source(rel, absolute):
    if any(sub in absolute for sub in EXCLUDE_SUBSTRINGS):
        return False
    return rel.startswith(INCLUDE_PREFIXES)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("build_dir")
    ap.add_argument("--repo-root", default=None)
    ap.add_argument("--json", default=None, help="write per-file detail here")
    args = ap.parse_args()

    build_dir = os.path.abspath(args.build_dir)
    repo_root = os.path.abspath(
        args.repo_root
        or os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    )

    # Per product source file: line_number -> covered (bool), unioned across
    # every binary that instrumented it.
    covered = {}

    gcda_files = list(find_gcda(build_dir))
    if not gcda_files:
        print(f"::error::no .gcda files under {build_dir}; was the coverage build run?", file=sys.stderr)
        return 1

    for gcda in gcda_files:
        doc = gcov_json(gcda, build_dir)
        if not doc:
            continue
        for file_entry in doc.get("files", []):
            absolute, rel = normalize(file_entry.get("file", ""), repo_root)
            if not is_product_source(rel, absolute):
                continue
            table = covered.setdefault(rel, {})
            for line in file_entry.get("lines", []):
                ln = line.get("line_number")
                if ln is None:
                    continue
                hit = line.get("count", 0) > 0
                if hit or ln not in table:
                    table[ln] = table.get(ln, False) or hit

    total_lines = 0
    total_hit = 0
    per_file = {}
    for rel, table in covered.items():
        n = len(table)
        h = sum(1 for v in table.values() if v)
        total_lines += n
        total_hit += h
        per_file[rel] = {"lines": n, "covered": h,
                         "rate": (100.0 * h / n) if n else 0.0}

    if total_lines == 0:
        print("::error::no instrumented product lines found (components/, main/)", file=sys.stderr)
        return 1

    rate = 100.0 * total_hit / total_lines

    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(
                {"total_lines": total_lines, "covered_lines": total_hit,
                 "line_rate_pct": rate, "files": per_file},
                fh, indent=2, sort_keys=True,
            )

    print(f"[host-coverage] product files: {len(per_file)}")
    print(f"[host-coverage] lines covered: {total_hit}/{total_lines}")
    print(f"[host-coverage] line coverage: {rate:.2f}%")
    # Final line: bare number for shell capture.
    print(f"{rate:.2f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
