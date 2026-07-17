#!/usr/bin/env bash
# Anti-regression gate: fails if the tracked tree carries references to this
# project's private infrastructure -- the internal Gitea org/host, the
# maintainer's home directory, secret file paths, or the real LAN hosts of
# the Gitea server and CI runner. A public repo must never carry these.
#
# Deliberately NOT flagged:
#   - 192.168.4.1: documented ESP32 SoftAP constant, not a real host.
#   - the general 192.168.0.0/16 private range used as illustrative example
#     addresses in docs and test fixtures throughout the tree; those are not
#     real hosts, only the two addresses named in the go-public plan's
#     history-scrub list are treated as real and checked for here.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

self="scripts/lint/check-no-internal-refs.sh"

pattern='example|justinlindh|host|/home/user|internal-planning/secrets|192\.168\.100\.[0-9]+|192\.168\.1\.199\b|192\.168\.1\.180\b'

hits="$(git grep -n -iE "$pattern" -- . ":!${self}" || true)"

if [[ -n "$hits" ]]; then
  printf 'check-no-internal-refs: internal infra references found (must not ship in a public repo):\n%s\n' "$hits" >&2
  exit 1
fi

echo "check-no-internal-refs: clean"
