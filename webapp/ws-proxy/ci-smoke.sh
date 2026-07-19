#!/usr/bin/env bash
# CI smoke check for the ws-proxy relay (issue: webapp/ws-proxy is another
# standalone package with its own package.json/package-lock.json that
# Dependabot updates, and nothing installs or starts it either).
#
# Run this from webapp/ws-proxy AFTER `npm ci` in this directory, so it
# resolves ws-proxy/node_modules per its own lockfile, not the webapp root's
# hoisted copy. Starts the real server on a fixed non-default port, waits for
# its own "listening" log line (deterministic: the process either logs it
# and stays up, or it doesn't and the loop times out; no fixed sleep, no
# retried assertions), then hits GET /health once before shutting it down.
set -u

PORT=39006
LOG="$(mktemp)"

PORT="$PORT" node server.mjs >"$LOG" 2>&1 &
PROXY_PID=$!

cleanup() {
  kill "$PROXY_PID" >/dev/null 2>&1 || true
}
trap cleanup EXIT

started=0
for _ in $(seq 1 100); do
  if grep -q "listening on :${PORT}" "$LOG"; then
    started=1
    break
  fi
  sleep 0.1
done

if [ "$started" -ne 1 ]; then
  echo "ws-proxy smoke check FAILED: server never logged 'listening on :${PORT}'"
  cat "$LOG"
  exit 1
fi

if ! curl -sf "http://localhost:${PORT}/health" >/dev/null; then
  echo "ws-proxy smoke check FAILED: GET /health did not return 200"
  cat "$LOG"
  exit 1
fi

echo "ws-proxy smoke check ok: server.mjs installed under its own lockfile, started, and answered /health"
