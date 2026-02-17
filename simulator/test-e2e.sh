#!/bin/bash
set -e
echo "=== Bramble Simulator E2E Test ==="

echo "1. Building C engine..."
cd "$(dirname "$0")/engine" && make clean && make && cd ..

echo "2. Running test scenario..."
timeout 5s engine/bramble-sim scenarios/test-2-node.json > /tmp/sim-output.json 2>/tmp/sim-stderr.log || true

echo "3. Validating output..."
if grep -q '"type":"node_moved"' /tmp/sim-output.json; then
  echo "✓ Found node_moved event"
else
  echo "✗ Missing node_moved event"
  cat /tmp/sim-output.json
  exit 1
fi

if grep -q '"type":"metrics"' /tmp/sim-output.json; then
  echo "✓ Found metrics event"
else
  echo "✗ Missing metrics event"
  exit 1
fi

echo "=== All tests passed ==="
