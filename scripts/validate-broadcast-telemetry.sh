#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  scripts/validate-broadcast-telemetry.sh \
    --send-output <path> \
    --telemetry-log <path> \
    --webapp-evidence <path>

Checks:
  1) sendBroadcast result includes broadcast_id
  2) Broadcast delivery telemetry events observed in logs
  3) Webapp recipient delivery panel evidence present

Exit code is non-zero when any check fails.
EOF
}

SEND_OUTPUT=""
TELEMETRY_LOG=""
WEBAPP_EVIDENCE=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --send-output)
      SEND_OUTPUT="${2:-}"
      shift 2
      ;;
    --telemetry-log)
      TELEMETRY_LOG="${2:-}"
      shift 2
      ;;
    --webapp-evidence)
      WEBAPP_EVIDENCE="${2:-}"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ -z "$SEND_OUTPUT" || -z "$TELEMETRY_LOG" || -z "$WEBAPP_EVIDENCE" ]]; then
  echo "ERROR: Missing required arguments." >&2
  usage >&2
  exit 2
fi

check_send="FAIL"
check_telemetry="FAIL"
check_webapp="FAIL"

if [[ -f "$SEND_OUTPUT" ]] && grep -Eq '"broadcast_id"[[:space:]]*:[[:space:]]*"[^"]+"' "$SEND_OUTPUT"; then
  check_send="PASS"
fi

if [[ -f "$TELEMETRY_LOG" ]] && grep -Eiq 'bramble\.onBroadcastDelivery|broadcast delivery|delivery telemetry|recipient.*(pending|delivered|failed)' "$TELEMETRY_LOG"; then
  check_telemetry="PASS"
fi

if [[ -f "$WEBAPP_EVIDENCE" ]]; then
  case "${WEBAPP_EVIDENCE##*.}" in
    txt|log|md|json)
      if grep -Eiq 'recipient|delivery panel|delivered|pending|failed|node[_ -]id' "$WEBAPP_EVIDENCE"; then
        check_webapp="PASS"
      fi
      ;;
    png|jpg|jpeg|webp)
      # For image-based evidence we can only verify artifact presence here.
      check_webapp="PASS"
      ;;
    *)
      # Unknown type; keep as FAIL unless it has obvious text markers.
      if grep -Eiq 'recipient|delivery panel|delivered|pending|failed' "$WEBAPP_EVIDENCE" 2>/dev/null; then
        check_webapp="PASS"
      fi
      ;;
  esac
fi

echo "check_sendBroadcast_returns_broadcast_id=$check_send"
echo "check_delivery_telemetry_observed=$check_telemetry"
echo "check_webapp_recipient_delivery_panel=$check_webapp"

if [[ "$check_send" == "PASS" && "$check_telemetry" == "PASS" && "$check_webapp" == "PASS" ]]; then
  exit 0
fi

exit 1
