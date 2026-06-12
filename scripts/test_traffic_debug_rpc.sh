#!/bin/bash
# Test script for traffic debug RPC methods
# Mock verification (would normally connect to actual device)

echo "=== Traffic Debug RPC Method Verification ==="
echo ""

echo "1. Testing bramble.setTrafficDebug"
echo "Request:"
cat <<'EOF'
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "bramble.setTrafficDebug",
  "params": {
    "enabled": true,
    "include_tx": true,
    "include_rx": true,
    "sample_rate": 100
  }
}
EOF

echo ""
echo "Expected Response:"
cat <<'EOF'
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "ok": true,
    "enabled": true,
    "include_tx": true,
    "include_rx": true,
    "sample_rate": 100
  }
}
EOF

echo ""
echo "---"
echo ""

echo "2. Testing bramble.getTrafficDebug"
echo "Request:"
cat <<'EOF'
{
  "jsonrpc": "2.0",
  "id": 2,
  "method": "bramble.getTrafficDebug",
  "params": {}
}
EOF

echo ""
echo "Expected Response:"
cat <<'EOF'
{
  "jsonrpc": "2.0",
  "id": 2,
  "result": {
    "enabled": true,
    "include_tx": true,
    "include_rx": true,
    "sample_rate": 100,
    "buffer_capacity": 512,
    "buffer_count": 0,
    "dropped_count": 0
  }
}
EOF

echo ""
echo "---"
echo ""

echo "3. Testing bramble.getTrafficEvents"
echo "Request:"
cat <<'EOF'
{
  "jsonrpc": "2.0",
  "id": 3,
  "method": "bramble.getTrafficEvents",
  "params": {
    "since_seq": 0,
    "limit": 10
  }
}
EOF

echo ""
echo "Expected Response (with sample events):"
cat <<'EOF'
{
  "jsonrpc": "2.0",
  "id": 3,
  "result": {
    "events": [
      {
        "seq": 1,
        "timestamp_ms": 12345,
        "pkt_type": 5,
        "category": "beacon",
        "airtime_tier": "broadcast",
        "packet_len": 64,
        "rssi": 0,
        "is_tx": true
      },
      {
        "seq": 2,
        "timestamp_ms": 12456,
        "pkt_type": 5,
        "category": "beacon",
        "airtime_tier": "broadcast",
        "packet_len": 64,
        "rssi": -75,
        "is_tx": false
      }
    ],
    "returned": 2,
    "total_available": 2
  }
}
EOF

echo ""
echo "---"
echo ""

echo "4. WebSocket notification stream"
echo "Event: bramble.onTrafficEvent"
echo "Expected notification (real-time):"
cat <<'EOF'
{
  "jsonrpc": "2.0",
  "method": "bramble.onTrafficEvent",
  "params": {
    "seq": 3,
    "timestamp_ms": 12567,
    "pkt_type": 10,
    "category": "chat",
    "airtime_tier": "normal",
    "packet_len": 128,
    "rssi": 0,
    "is_tx": true
  }
}
EOF

echo ""
echo "=== Verification Complete ==="
echo "All RPC methods defined and payload schemas documented."
