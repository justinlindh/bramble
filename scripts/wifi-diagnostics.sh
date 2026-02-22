#!/usr/bin/env bash
# WiFi Diagnostics Script for Bramble T-Deck
# Tests network reachability, websocket connectivity, and RPC functionality

set -euo pipefail

DEVICE_IP="${1:-192.0.2.0}"
SERIAL_PORT="${2:-/dev/ttyACM0}"

echo "╔══════════════════════════════════════════════════════════╗"
echo "║  Bramble T-Deck WiFi Diagnostics                         ║"
echo "╚══════════════════════════════════════════════════════════╝"
echo ""
echo "Device IP:    $DEVICE_IP"
echo "Serial Port:  $SERIAL_PORT"
echo ""

# Test 1: Ping
echo "[1/5] Testing ICMP ping..."
if ping -c 3 -W 2 "$DEVICE_IP" > /dev/null 2>&1; then
    echo "  ✓ Ping successful"
    ping -c 3 "$DEVICE_IP" | grep "rtt min/avg/max"
else
    echo "  ✗ Ping failed - device unreachable"
    echo ""
    echo "DIAGNOSIS: Device is not reachable on the network."
    echo "Possible causes:"
    echo "  1. Device is in AP mode (check for 192.168.4.1)"
    echo "  2. Device is on a different network"
    echo "  3. WiFi disconnected (check serial logs)"
    echo "  4. Routing issue on host"
    exit 1
fi
echo ""

# Test 2: HTTP port
echo "[2/5] Testing HTTP port 80..."
if nc -zv -w 2 "$DEVICE_IP" 80 2>&1 | grep -q "open"; then
    echo "  ✓ Port 80 open"
else
    echo "  ✗ Port 80 not responding"
    echo ""
    echo "DIAGNOSIS: Port 80 is closed or filtered."
    echo "Possible causes:"
    echo "  1. WebSocket server not started"
    echo "  2. WiFi connected but server initialization failed"
    echo "  3. Firewall blocking connection"
    exit 1
fi
echo ""

# Test 3: WebSocket handshake
echo "[3/5] Testing WebSocket handshake..."
python3 -c "
import asyncio
import websockets
import sys

async def test():
    try:
        ws = await asyncio.wait_for(
            websockets.connect('ws://$DEVICE_IP/ws'), 
            timeout=10
        )
        await ws.close()
        print('  ✓ WebSocket handshake successful')
        return 0
    except Exception as e:
        print(f'  ✗ WebSocket handshake failed: {e}')
        return 1

sys.exit(asyncio.run(test()))
"
if [ $? -ne 0 ]; then
    echo ""
    echo "DIAGNOSIS: WebSocket handshake failed."
    echo "Possible causes:"
    echo "  1. Server accepting HTTP but not upgrading to WebSocket"
    echo "  2. Firmware version mismatch"
    exit 1
fi
echo ""

# Test 4: RPC via WebSocket
echo "[4/5] Testing bramble.getStatus RPC..."
python3 -c "
import asyncio
import websockets
import json
import sys

async def test():
    try:
        ws = await asyncio.wait_for(
            websockets.connect('ws://$DEVICE_IP/ws'), 
            timeout=10
        )
        req = {'jsonrpc': '2.0', 'id': 1, 'method': 'bramble.getStatus', 'params': {}}
        await ws.send(json.dumps(req))
        resp = await asyncio.wait_for(ws.recv(), timeout=5)
        data = json.loads(resp)
        if 'result' in data:
            print('  ✓ bramble.getStatus succeeded')
            print(f'    - address: {data[\"result\"][\"address\"]}')
            print(f'    - uptime: {data[\"result\"][\"uptime_s\"]}s')
            print(f'    - peers: {data[\"result\"][\"peers\"]}')
            await ws.close()
            return 0
        else:
            print('  ✗ No result in RPC response')
            return 1
    except Exception as e:
        print(f'  ✗ RPC test failed: {e}')
        return 1

sys.exit(asyncio.run(test()))
"
if [ $? -eq 0 ]; then
    echo ""
else
    echo ""
    echo "DIAGNOSIS: RPC call failed."
    echo "Possible causes:"
    echo "  1. RPC dispatcher not initialized"
    echo "  2. Firmware crash/restart loop"
    echo "  3. Memory exhaustion"
    exit 1
fi

# Test 5: Serial console check
echo "[5/5] Checking serial console..."
if [ -e "$SERIAL_PORT" ]; then
    echo "  ✓ Serial port exists: $SERIAL_PORT"
    python3 -c "
import serial
import time

try:
    ser = serial.Serial('$SERIAL_PORT', 115200, timeout=2)
    time.sleep(0.5)
    ser.reset_input_buffer()
    ser.write(b'wifi status\n')
    ser.flush()
    time.sleep(1)
    response = ser.read(4096).decode('utf-8', errors='ignore')
    ser.close()
    
    print('  Serial WiFi status:')
    for line in response.split('\n'):
        if 'Mode:' in line or 'SSID:' in line or 'IP:' in line:
            print(f'    {line.strip()}')
except Exception as e:
    print(f'  ✗ Serial check failed: {e}')
"
else
    echo "  ⚠ Serial port not found (device may be USB-disconnected)"
fi
echo ""

echo "╔══════════════════════════════════════════════════════════╗"
echo "║  ALL DIAGNOSTICS PASSED ✓                                ║"
echo "║  Device is fully operational on WiFi                     ║"
echo "╚══════════════════════════════════════════════════════════╝"
