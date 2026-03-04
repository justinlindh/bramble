#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WEBAPP_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

PORT="${PORT:-8085}"
BASE_URL="${BASE_URL:-http://127.0.0.1:${PORT}}"
SMOKE_START_RUNTIME="${SMOKE_START_RUNTIME:-1}"
SMOKE_MAX_WAIT_SECONDS="${SMOKE_MAX_WAIT_SECONDS:-60}"
SMOKE_SERVER_LOG="${SMOKE_SERVER_LOG:-${WEBAPP_DIR}/e2e-smoke-server.log}"
SMOKE_SERVER_PID_FILE="${SMOKE_SERVER_PID_FILE:-${WEBAPP_DIR}/e2e-smoke-server.pid}"

if [[ $# -ge 1 ]]; then
  BASE_URL="$1"
fi

SMOKE_HOST="$(node -e 'const u=new URL(process.argv[1]);process.stdout.write(u.hostname);' "${BASE_URL}")"
SMOKE_PORT="$(node -e 'const u=new URL(process.argv[1]);process.stdout.write(String(u.port || (u.protocol === "https:" ? 443 : 80)));' "${BASE_URL}")"

SERVER_PID=""
LAST_HEALTH_HTTP_STATUS=""
LAST_HEALTH_BODY=""

fail() {
  echo "[smoke] ERROR: $*" >&2
  if [[ -n "${SERVER_PID}" && -f "${SMOKE_SERVER_LOG}" ]]; then
    echo "[smoke] ---- server log tail ----" >&2
    tail -n 120 "${SMOKE_SERVER_LOG}" >&2 || true
    echo "[smoke] -------------------------" >&2
  fi
  exit 1
}

cleanup() {
  if [[ -n "${SERVER_PID}" ]]; then
    kill "${SERVER_PID}" >/dev/null 2>&1 || true
    wait "${SERVER_PID}" 2>/dev/null || true
  fi
  rm -f "${SMOKE_SERVER_PID_FILE}"
}
trap cleanup EXIT

assert_port_free_for_managed_start() {
  if [[ "${SMOKE_START_RUNTIME}" != "1" ]]; then
    return
  fi

  if node -e 'const net=require("net");const host=process.argv[1];const port=Number(process.argv[2]);const socket=net.createConnection({host,port});socket.setTimeout(1000);socket.on("connect",()=>process.exit(0));socket.on("timeout",()=>process.exit(1));socket.on("error",()=>process.exit(1));' "${SMOKE_HOST}" "${SMOKE_PORT}" >/dev/null 2>&1; then
    fail "port ${SMOKE_PORT} already accepts TCP connections at ${BASE_URL}; refusing managed start to avoid false-positive smoke pass"
  fi
}

start_runtime_if_requested() {
  if [[ "${SMOKE_START_RUNTIME}" != "1" ]]; then
    echo "[smoke] SMOKE_START_RUNTIME=${SMOKE_START_RUNTIME}; expecting runtime to already be running at ${BASE_URL}"
    return
  fi

  assert_port_free_for_managed_start

  echo "[smoke] Starting unified runtime"
  rm -f "${SMOKE_SERVER_LOG}" "${SMOKE_SERVER_PID_FILE}"
  (
    cd "${WEBAPP_DIR}"
    exec node server/unified-server.mjs >"${SMOKE_SERVER_LOG}" 2>&1
  ) &
  SERVER_PID="$!"
  echo "${SERVER_PID}" > "${SMOKE_SERVER_PID_FILE}"
}

wait_for_health() {
  echo "[smoke] Waiting for health endpoint: ${BASE_URL}/api/healthz"

  for _ in $(seq 1 "${SMOKE_MAX_WAIT_SECONDS}"); do
    if [[ -n "${SERVER_PID}" ]] && ! kill -0 "${SERVER_PID}" >/dev/null 2>&1; then
      fail "runtime process exited before becoming healthy"
    fi

    set +e
    response="$(curl -sS --connect-timeout 2 --max-time 5 -w $'\n__HTTP_STATUS__:%{http_code}' "${BASE_URL}/api/healthz")"
    curl_status=$?
    set -e

    if [[ ${curl_status} -eq 0 ]]; then
      http_status="${response##*__HTTP_STATUS__:}"
      body="${response%$'\n'__HTTP_STATUS__:*}"
      LAST_HEALTH_HTTP_STATUS="${http_status}"
      LAST_HEALTH_BODY="${body}"
      if [[ "${http_status}" == "200" ]] && printf '%s' "${body}" | node -e 'let d="";process.stdin.on("data",c=>d+=c);process.stdin.on("end",()=>{const j=JSON.parse(d);if(j.ok===true) process.exit(0);process.exit(1);});' 2>/dev/null; then
        echo "[smoke] Runtime healthy"
        return
      fi
    fi

    sleep 1
  done

  if [[ -n "${LAST_HEALTH_HTTP_STATUS}" ]]; then
    echo "[smoke] last health status=${LAST_HEALTH_HTTP_STATUS}" >&2
    echo "[smoke] last health body=${LAST_HEALTH_BODY}" >&2
  fi
  fail "runtime did not become healthy within ${SMOKE_MAX_WAIT_SECONDS}s"
}

http_get_and_assert() {
  local path="$1"
  local expected_status="$2"
  local body_check_script="${3:-}"

  local url="${BASE_URL}${path}"
  echo "[smoke] GET ${url}"

  set +e
  local response
  response="$(curl -sS --connect-timeout 2 --max-time 10 -w $'\n__HTTP_STATUS__:%{http_code}' "${url}")"
  local curl_status=$?
  set -e

  if [[ ${curl_status} -ne 0 ]]; then
    fail "curl failed for ${url} (exit=${curl_status})"
  fi

  local http_status="${response##*__HTTP_STATUS__:}"
  local body="${response%$'\n'__HTTP_STATUS__:*}"

  if [[ "${http_status}" != "${expected_status}" ]]; then
    echo "[smoke] response body: ${body}" >&2
    fail "unexpected status for ${url}: expected ${expected_status}, got ${http_status}"
  fi

  if [[ -n "${body_check_script}" ]]; then
    if ! printf '%s' "${body}" | node -e "${body_check_script}"; then
      echo "[smoke] response body: ${body}" >&2
      fail "response validation failed for ${url}"
    fi
  fi

  echo "[smoke] OK ${path} (${http_status})"
}

start_runtime_if_requested
wait_for_health

http_get_and_assert "/" "200" 'let d="";process.stdin.on("data",c=>d+=c);process.stdin.on("end",()=>{if(/<html|<!doctype html/i.test(d))process.exit(0);process.exit(1);});'
http_get_and_assert "/api/healthz" "200" 'let d="";process.stdin.on("data",c=>d+=c);process.stdin.on("end",()=>{const j=JSON.parse(d);if(j.ok===true)process.exit(0);process.exit(1);});'
http_get_and_assert "/api/mode" "200" 'let d="";process.stdin.on("data",c=>d+=c);process.stdin.on("end",()=>{const j=JSON.parse(d);if(j.mode==="hosted"||j.mode==="local")process.exit(0);process.exit(1);});'

echo "[smoke] PASS: unified runtime smoke checks succeeded"
