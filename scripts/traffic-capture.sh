#!/bin/bash
# Traffic capture utility for Bramble traffic debug telemetry
# Captures via WebSocket stream + RPC backfill and saves to JSONL

set -e

DEVICE="${1:-192.168.1.64}"
DURATION="${2:-300}"  # default 5 minutes
OUTPUT="${3:-traffic-capture-$(date +%Y%m%d-%H%M%S).jsonl}"

echo "=== Bramble Traffic Capture ==="
echo "Device: $DEVICE"
echo "Duration: ${DURATION}s"
echo "Output: $OUTPUT"
echo ""

# Launch Python capture script
python3 - "$DEVICE" "$DURATION" "$OUTPUT" <<'PYTHON_SCRIPT'
#!/usr/bin/env python3
import asyncio
import json
import sys
import time
import signal
from datetime import datetime

try:
    import websockets
except ImportError:
    print("ERROR: websockets module not found. Install: pip3 install websockets")
    sys.exit(1)

device = sys.argv[1]
duration = int(sys.argv[2])
output = sys.argv[3]

url = f"ws://{device}/ws"
rpc_id = 0
stop_event = asyncio.Event()
events_captured = []
last_seq = 0

def signal_handler(sig, frame):
    print("\n[!] Interrupt received, stopping capture...")
    stop_event.set()

signal.signal(signal.SIGINT, signal_handler)

async def rpc_call(ws, method, params=None, timeout=5.0):
    global rpc_id
    rpc_id += 1
    req = {"jsonrpc": "2.0", "method": method, "id": rpc_id, "params": params or {}}
    await ws.send(json.dumps(req))
    
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        remaining = deadline - time.monotonic()
        try:
            raw = await asyncio.wait_for(ws.recv(), timeout=remaining)
            msg = json.loads(raw)
            if msg.get("id") == rpc_id:
                return msg
        except asyncio.TimeoutError:
            raise TimeoutError(f"RPC timeout: {method}")
    raise TimeoutError(f"RPC timeout: {method}")

async def capture():
    global last_seq
    
    print(f"[+] Connecting to {url}")
    async with websockets.connect(url, open_timeout=10, ping_interval=None) as ws:
        print("[+] Connected")
        
        # Enable traffic debug
        print("[+] Enabling traffic debug...")
        resp = await rpc_call(ws, "bramble.setTrafficDebug", {
            "enabled": True,
            "include_tx": True,
            "include_rx": True,
            "sample_rate": 100
        })
        if resp.get("result", {}).get("enabled"):
            print("    Traffic debug enabled")
        else:
            print("    WARNING: Failed to enable traffic debug")
            print(f"    Response: {resp}")
        
        # Get initial status
        status = await rpc_call(ws, "bramble.getTrafficDebug")
        print(f"[+] Buffer status: {status.get('result', {})}")
        
        # Start capture
        print(f"[+] Capturing for {duration} seconds...")
        start_time = time.monotonic()
        
        with open(output, 'w') as f:
            async def receive_events():
                global last_seq
                while not stop_event.is_set():
                    try:
                        raw = await asyncio.wait_for(ws.recv(), timeout=1.0)
                        msg = json.loads(raw)
                        
                        # Handle traffic event notifications
                        if msg.get("method") == "bramble.onTrafficEvent":
                            event = msg.get("params", {})
                            seq = event.get("seq", 0)
                            if seq > last_seq:
                                last_seq = seq
                            events_captured.append(event)
                            f.write(json.dumps(event) + "\n")
                            f.flush()
                            
                            # Progress indicator
                            if len(events_captured) % 10 == 0:
                                elapsed = time.monotonic() - start_time
                                print(f"    Events: {len(events_captured)}, Seq: {seq}, Elapsed: {elapsed:.1f}s", end='\r')
                    
                    except asyncio.TimeoutError:
                        # Periodic backfill check
                        if time.monotonic() - start_time > duration:
                            stop_event.set()
                            break
                        continue
                    except Exception as e:
                        print(f"\n[!] Error receiving: {e}")
                        break
            
            await receive_events()
        
        # Final backfill
        print(f"\n[+] Performing final backfill...")
        resp = await rpc_call(ws, "bramble.getTrafficEvents", {
            "since_seq": last_seq,
            "limit": 1000
        })
        backfill_events = resp.get("result", {}).get("events", [])
        if backfill_events:
            print(f"    Backfilled {len(backfill_events)} events")
            with open(output, 'a') as f:
                for event in backfill_events:
                    seq = event.get("seq", 0)
                    if seq > last_seq:
                        events_captured.append(event)
                        f.write(json.dumps(event) + "\n")
                        last_seq = seq
        
        # Disable traffic debug
        print("[+] Disabling traffic debug...")
        await rpc_call(ws, "bramble.setTrafficDebug", {"enabled": False})
        
        print(f"\n[+] Capture complete!")
        print(f"    Total events: {len(events_captured)}")
        print(f"    Last seq: {last_seq}")
        print(f"    Output: {output}")

try:
    asyncio.run(capture())
except Exception as e:
    print(f"[!] Capture failed: {e}")
    sys.exit(1)

PYTHON_SCRIPT

echo ""
echo "Capture saved to: $OUTPUT"
