#!/usr/bin/env bash
set -euo pipefail

script="scripts/ci-publish-ota.sh"

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

assert_eq() {
  local got=$1 expected=$2 msg=$3
  if [[ "$got" != "$expected" ]]; then
    echo "FAIL: $msg (got='$got' expected='$expected')"
    exit 1
  fi
}

assert_fail() {
  local msg=$1
  shift
  if "$@" >/dev/null 2>&1; then
    echo "FAIL: expected command to fail: $msg"
    exit 1
  fi
}

# 1) Runtime constant is the primary source.
cat >"$tmpdir/rpc_ok.c" <<'EOF'
#define BRAMBLE_VERSION_STR "1.2.3"
EOF
v=$(RPC_VERSION_FILE="$tmpdir/rpc_ok.c" bash "$script" --print-version)
assert_eq "$v" "v1.2.3" "runtime constant should normalize to semver with leading v"

# 2) VERSION override remains available for manual/emergency use.
v=$(VERSION="9.9.9-hotfix" RPC_VERSION_FILE="$tmpdir/does-not-matter.c" bash "$script" --print-version)
assert_eq "$v" "v9.9.9-hotfix" "VERSION override should take priority"

# 3) CI path fails hard when BRAMBLE_VERSION_STR is missing/unreadable.
assert_fail "missing runtime version file in CI must fail" \
  env CI=1 RPC_VERSION_FILE="$tmpdir/missing.c" bash "$script" --print-version

# 4) CI path fails hard when runtime constant is malformed / non-semver.
cat >"$tmpdir/rpc_bad.c" <<'EOF'
#define BRAMBLE_VERSION_STR "not-a-semver"
EOF
assert_fail "invalid runtime semver in CI must fail" \
  env CI=1 RPC_VERSION_FILE="$tmpdir/rpc_bad.c" bash "$script" --print-version

# 5) Non-CI path may fall back, but must still output a version string.
cat >"$tmpdir/rpc_empty.c" <<'EOF'
/* no BRAMBLE_VERSION_STR present */
EOF
v=$(RPC_VERSION_FILE="$tmpdir/rpc_empty.c" bash "$script" --print-version)
[[ -n "$v" ]] || { echo "FAIL: expected non-empty fallback version"; exit 1; }

echo "OK: ota semver source lock + CI fail-hard checks"
