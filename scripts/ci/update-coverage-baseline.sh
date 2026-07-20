#!/usr/bin/env bash
# Deliberately re-measure every coverage suite and rewrite the floors in
# ci/coverage-baseline.json. Run this by hand when you have intentionally
# changed coverage (added tests: floors go up; removed a feature and its tests:
# a floor may go down with a justification in the PR). The file is committed, so
# the baseline only ever moves in a reviewed diff, never on its own.
#
# The measured numbers are toolchain-sensitive for the host C suite (gcc
# version), so the canonical baseline is the value CI measures on the runner.
# When this script's local numbers differ from CI, prefer the CI value and edit
# the JSON to match.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BASELINE="$REPO_ROOT/ci/coverage-baseline.json"

echo "=== host-c ==="
HOST_C="$(COVERAGE_ONLY_MEASURE=1 bash "$REPO_ROOT/scripts/ci/run-host-coverage.sh" | tail -1)"

echo "=== gosim ==="
( cd "$REPO_ROOT/simulator/gosim"
  go test -covermode=set -coverprofile=/tmp/gosim-cover.out -count=1 ./... >/dev/null 2>&1 || true )
GOSIM="$(go -C "$REPO_ROOT/simulator/gosim" tool cover -func=/tmp/gosim-cover.out \
    | awk '/^total:/ {gsub(/%/,"",$NF); print $NF}')"

echo "=== webapp ==="
( cd "$REPO_ROOT/webapp" && npx vitest run --coverage >/dev/null 2>&1 || true )
WEBAPP="$(python3 -c "import json;print(json.load(open('$REPO_ROOT/webapp/coverage/coverage-summary.json'))['total']['lines']['pct'])")"

echo "measured: host-c=$HOST_C gosim=$GOSIM webapp=$WEBAPP"

python3 - "$BASELINE" "$HOST_C" "$GOSIM" "$WEBAPP" <<'PY'
import json, sys
path, host_c, gosim, webapp = sys.argv[1], float(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4])
with open(path, encoding="utf-8") as fh:
    data = json.load(fh)
# Round down to one decimal so the floor sits at or just under the measurement.
def floor1(x):
    return int(x * 10) / 10.0
data["suites"]["host-c"] = floor1(host_c)
data["suites"]["gosim"] = floor1(gosim)
data["suites"]["webapp"] = floor1(webapp)
with open(path, "w", encoding="utf-8") as fh:
    json.dump(data, fh, indent=2)
    fh.write("\n")
print("wrote", path, data["suites"])
PY
echo "Review the diff and commit ci/coverage-baseline.json deliberately."
