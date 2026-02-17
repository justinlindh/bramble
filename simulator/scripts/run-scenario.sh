#!/bin/bash
# Run a Bramble simulator scenario headlessly and report stats.
# Usage: ./run-scenario.sh <scenario-name|path> [--json] [--verbose]
#
# Examples:
#   ./run-scenario.sh ideal-10-node
#   ./run-scenario.sh ideal-massive --json
#   ./run-scenario.sh all              # run all scenarios
#   ./run-scenario.sh ../scenarios/custom.json

set -euo pipefail

CONTAINER="simulator-bramble-sim-1"
SCENARIO_DIR="/app/scenarios"
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

if [ ${#SCENARIOS[@]} -eq 0 ]; then
  echo "Usage: $0 <scenario|all> [--json] [--verbose]"
  echo ""
  echo "Available scenarios:"
  docker exec "$CONTAINER" ls "$SCENARIO_DIR" 2>/dev/null | sed 's/\.json$//' | sed 's/^/  /'
  exit 1
fi

# Expand "all"
if [ "${SCENARIOS[0]}" = "all" ]; then
  SCENARIOS=($(docker exec "$CONTAINER" ls "$SCENARIO_DIR" 2>/dev/null | sed 's/\.json$//'))
fi

run_scenario() {
  local name="$1"
  local path="$SCENARIO_DIR/$name.json"

  # Capture all output
  local output
  output=$(docker exec "$CONTAINER" timeout 300 /app/engine/bramble-sim "$path" 2>&1)

  # Extract final metrics line (last JSON with type=metrics)
  local final_metrics
  final_metrics=$(echo "$output" | grep '"type":"metrics"' | tail -1)

  if [ -z "$final_metrics" ]; then
    echo "ERROR: No metrics output for $name"
    return 1
  fi

  # Parse with node (available in container)
  local stats
  stats=$(echo "$final_metrics" | docker exec -i "$CONTAINER" node -e "
    const fs = require('fs');
    const m = JSON.parse(fs.readFileSync('/dev/stdin','utf8'));
    const rate = m.messages_sent > 0 ? (m.delivered / m.messages_sent * 100).toFixed(1) : '0.0';
    const result = {
      scenario: '$name',
      messages_sent: m.messages_sent,
      delivered: m.delivered,
      delivery_pct: parseFloat(rate),
      dropped: m.dropped,
      total_packets: m.total_packets,
      avg_latency_ms: m.avg_latency_ms,
      active_nodes: m.active_nodes,
      duration_s: m.timestamp_us / 1e6,
      overhead_ratio: m.messages_sent > 0 ? parseFloat((m.total_packets / m.messages_sent).toFixed(1)) : 0
    };
    console.log(JSON.stringify(result));
  ")

  if $JSON_MODE; then
    echo "$stats"
  else
    echo "$stats" | docker exec -i "$CONTAINER" node -e "
      const m = JSON.parse(require('fs').readFileSync('/dev/stdin','utf8'));
      console.log('━'.repeat(50));
      console.log('  Scenario:      ' + m.scenario);
      console.log('  Nodes:         ' + m.active_nodes);
      console.log('  Duration:      ' + m.duration_s + 's');
      console.log('  Messages:      ' + m.delivered + '/' + m.messages_sent + ' delivered (' + m.delivery_pct + '%)');
      console.log('  Dropped:       ' + m.dropped);
      console.log('  Avg latency:   ' + m.avg_latency_ms.toFixed(1) + ' ms');
      console.log('  Total packets: ' + m.total_packets);
      console.log('  Overhead:      ' + m.overhead_ratio + 'x (packets per message)');
    "
  fi

  if $VERBOSE; then
    # Show route and anomaly events
    echo ""
    echo "  Route events:"
    echo "$output" | grep '"type":"route_' | wc -l | xargs -I{} echo "    {} route changes"
    echo "  Anomalies:"
    local anomalies
    anomalies=$(echo "$output" | grep '"type":"anomaly"' | wc -l)
    if [ "$anomalies" -gt 0 ]; then
      echo "    $anomalies anomalies detected"
      echo "$output" | grep '"type":"anomaly"' | docker exec -i "$CONTAINER" node -e "
        const lines = require('fs').readFileSync('/dev/stdin','utf8').trim().split('\n');
        const counts = {};
        lines.forEach(l => { const a = JSON.parse(l); counts[a.anomaly_type] = (counts[a.anomaly_type]||0)+1; });
        Object.entries(counts).forEach(([k,v]) => console.log('      ' + k + ': ' + v));
      "
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
