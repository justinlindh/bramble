#!/bin/bash
# Run a Bramble simulator scenario headlessly and report stats.
# Usage: ./run-scenario.sh <scenario-name|path> [--json] [--verbose]
#
# Examples:
#   ./run-scenario.sh ideal-10-node
#   ./run-scenario.sh ideal-massive --json
#   ./run-scenario.sh all              # run all scenarios
#   ./run-scenario.sh ../scenarios/custom.json
#
# Works both locally (if bramble-gosim is on PATH or built nearby)
# and via Docker (falls back to container execution).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SIMULATOR_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SCENARIO_DIR="$SIMULATOR_DIR/scenarios"
CONTAINER="simulator-bramble-sim-1"

JSON_MODE=false
VERBOSE=false
SCENARIOS=()

for arg in "$@"; do
  case "$arg" in
    --json) JSON_MODE=true ;;
    --verbose) VERBOSE=true ;;
    *) SCENARIOS+=("$arg") ;;
  esac
done

# Detect execution mode: local binary or Docker container
GOSIM_BIN=""
USE_DOCKER=false

if command -v bramble-gosim &>/dev/null; then
  GOSIM_BIN="bramble-gosim"
elif [ -x "$SIMULATOR_DIR/gosim/bramble-gosim" ]; then
  GOSIM_BIN="$SIMULATOR_DIR/gosim/bramble-gosim"
elif docker exec "$CONTAINER" true &>/dev/null; then
  USE_DOCKER=true
  SCENARIO_DIR="/scenarios"
else
  echo "ERROR: bramble-gosim not found locally and Docker container '$CONTAINER' is not running."
  echo "Either build locally (cd simulator/gosim && go build) or start Docker (docker compose up -d)."
  exit 1
fi

run_cmd() {
  if $USE_DOCKER; then
    docker exec "$CONTAINER" "$@"
  else
    "$@"
  fi
}

if [ ${#SCENARIOS[@]} -eq 0 ]; then
  echo "Usage: $0 <scenario|all> [--json] [--verbose]"
  echo ""
  echo "Available scenarios:"
  if $USE_DOCKER; then
    docker exec "$CONTAINER" ls "$SCENARIO_DIR" 2>/dev/null | sed 's/\.json$//' | sed 's/^/  /'
  else
    ls "$SCENARIO_DIR" 2>/dev/null | sed 's/\.json$//' | sed 's/^/  /'
  fi
  exit 1
fi

# Expand "all"
if [ "${SCENARIOS[0]}" = "all" ]; then
  if $USE_DOCKER; then
    SCENARIOS=($(docker exec "$CONTAINER" ls "$SCENARIO_DIR" 2>/dev/null | sed 's/\.json$//'))
  else
    SCENARIOS=($(ls "$SCENARIO_DIR" 2>/dev/null | sed 's/\.json$//'))
  fi
fi

run_scenario() {
  local name="$1"
  local path="$SCENARIO_DIR/$name.json"

  # Run headless simulation
  local output
  if $USE_DOCKER; then
    output=$(docker exec "$CONTAINER" timeout 300 bramble-gosim --headless --scenario "$path" 2>&1)
  else
    output=$(timeout 300 $GOSIM_BIN --headless --scenario "$path" 2>&1)
  fi

  # Extract final metrics line (last JSON with type=metrics)
  local final_metrics
  final_metrics=$(echo "$output" | grep '"type":"final_metrics"' | tail -1)
  if [ -z "$final_metrics" ]; then
    # Fallback to periodic metrics if final_metrics not found
    final_metrics=$(echo "$output" | grep '"type":"metrics"' | tail -1)
  fi

  if [ -z "$final_metrics" ]; then
    echo "ERROR: No metrics output for $name"
    if $VERBOSE; then
      echo "  Raw output:"
      echo "$output" | head -20 | sed 's/^/    /'
    fi
    return 1
  fi

  # Parse metrics with lightweight tools (no node dependency)
  local stats
  stats=$(echo "$final_metrics" | python3 -c "
import sys, json
m = json.load(sys.stdin)
rate = (m.get('delivered',0) / m['messages_sent'] * 100) if m.get('messages_sent',0) > 0 else 0.0
overhead = round(m.get('total_packets',0) / m['messages_sent'], 1) if m.get('messages_sent',0) > 0 else 0
result = {
    'scenario': '$name',
    'messages_sent': m.get('messages_sent', 0),
    'delivered': m.get('delivered', 0),
    'delivery_pct': round(rate, 1),
    'dropped': m.get('dropped', 0),
    'total_packets': m.get('total_packets', 0),
    'avg_latency_ms': m.get('avg_latency_ms', 0),
    'active_nodes': m.get('active_nodes', '?'),
    'duration_s': round(m.get('timestamp_us', 0) / 1e6, 2) if m.get('timestamp_us') else '?',
    'airtime_deferred': m.get('airtime_deferred', 0),
    'fragments_sent': m.get('fragments_sent', 0),
    'crypto_encrypted': m.get('crypto_encrypted', 0),
    'overhead_ratio': overhead
}
print(json.dumps(result))
" 2>/dev/null)

  if [ -z "$stats" ]; then
    echo "ERROR: Failed to parse metrics for $name"
    return 1
  fi

  if $JSON_MODE; then
    echo "$stats"
  else
    echo "$stats" | python3 -c "
import sys, json
m = json.load(sys.stdin)
print('━' * 50)
print('  Scenario:      ' + m['scenario'])
print('  Nodes:         ' + str(m['active_nodes']))
print('  Duration:      ' + str(m['duration_s']) + 's')
print('  Messages:      ' + str(m['delivered']) + '/' + str(m['messages_sent']) + ' delivered (' + str(m['delivery_pct']) + '%)')
print('  Dropped:       ' + str(m['dropped']))
print('  Avg latency:   ' + str(round(m['avg_latency_ms'], 1)) + ' ms')
print('  Total packets: ' + str(m['total_packets']))
print('  Overhead:      ' + str(m['overhead_ratio']) + 'x (packets per message)')
if m.get('airtime_deferred', 0) > 0:
    print('  Airtime defer: ' + str(m['airtime_deferred']))
if m.get('fragments_sent', 0) > 0:
    print('  Fragments:     ' + str(m['fragments_sent']))
if m.get('crypto_encrypted', 0) > 0:
    print('  Encrypted:     ' + str(m['crypto_encrypted']))
"
  fi

  if $VERBOSE; then
    echo ""
    echo "  Route events:"
    echo "$output" | grep -c '"type":"route_' | xargs -I{} echo "    {} route changes" || echo "    0 route changes"
    echo "  Anomalies:"
    local anomalies
    anomalies=$(echo "$output" | grep -c '"type":"anomaly"' || true)
    if [ "$anomalies" -gt 0 ]; then
      echo "    $anomalies anomalies detected"
    else
      echo "    none"
    fi
  fi
}

if ! $JSON_MODE; then
  echo ""
  echo "╔══════════════════════════════════════════════════╗"
  echo "║         Bramble Mesh Simulator Results           ║"
  echo "╚══════════════════════════════════════════════════╝"
fi

for scenario in "${SCENARIOS[@]}"; do
  run_scenario "$scenario"
done

if ! $JSON_MODE; then
  echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
fi
