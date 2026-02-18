#!/usr/bin/env python3
"""
Bramble End-to-End Device Test Suite

Exercises all 23 JSON-RPC methods against real Bramble hardware.
Requires 2 nodes for messaging/neighbor tests.

Usage:
  # WebSocket (default)
  python3 scripts/e2e-test.py ws://192.168.1.64/ws ws://192.168.1.21/ws

  # BLE (by MAC address)
  python3 scripts/e2e-test.py ble:F0:9E:9E:75:5A:9E ble:10:51:DB:57:9A:12

  # Serial
  python3 scripts/e2e-test.py serial:/dev/ttyUSB0 serial:/dev/ttyUSB1

  # Mixed
  python3 scripts/e2e-test.py ws://192.168.1.64/ws ble:F0:9E:9E:75:5A:9E
"""

import asyncio
import json
import sys
import time
import argparse
from dataclasses import dataclass, field
from typing import Any, Optional

# ── Transport Abstraction ────────────────────────────────────────────

class Transport:
    async def connect(self): ...
    async def rpc(self, method: str, params: dict = None, timeout: float = 5.0) -> dict: ...
    async def close(self): ...
    def label(self) -> str: ...


class WSTransport(Transport):
    def __init__(self, url: str):
        self.url = url
        self.ws = None
        self._id = 0

    async def connect(self):
        import websockets
        self.ws = await websockets.connect(self.url, open_timeout=5)

    async def rpc(self, method, params=None, timeout=5.0):
        self._id += 1
        req = {"jsonrpc": "2.0", "method": method, "id": self._id}
        if params:
            req["params"] = params
        await self.ws.send(json.dumps(req))
        # Read responses, skip notifications
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            remaining = deadline - time.monotonic()
            raw = await asyncio.wait_for(self.ws.recv(), timeout=remaining)
            msg = json.loads(raw)
            if msg.get("id") == self._id:
                return msg
        raise TimeoutError(f"RPC timeout: {method}")

    async def close(self):
        if self.ws:
            await self.ws.close()

    def label(self):
        return self.url


class BLETransport(Transport):
    NUS_TX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
    NUS_RX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

    def __init__(self, address: str):
        self.address = address
        self.client = None
        self._id = 0
        self._buf = bytearray()
        self._event = asyncio.Event()

    async def connect(self):
        from bleak import BleakClient
        self.client = BleakClient(self.address, timeout=15)
        await self.client.connect()
        await self.client.start_notify(self.NUS_RX, self._on_rx)
        await asyncio.sleep(0.3)

    def _on_rx(self, sender, data):
        self._buf.extend(data)
        if b"\n" in self._buf:
            self._event.set()

    async def rpc(self, method, params=None, timeout=5.0):
        self._id += 1
        self._buf = bytearray()
        self._event = asyncio.Event()
        req = {"jsonrpc": "2.0", "method": method, "id": self._id}
        if params:
            req["params"] = params
        payload = json.dumps(req).encode() + b"\n"
        for i in range(0, len(payload), 20):
            await self.client.write_gatt_char(self.NUS_TX, payload[i:i+20], response=False)
            await asyncio.sleep(0.03)
        await asyncio.wait_for(self._event.wait(), timeout=timeout)
        line = self._buf.decode().split("\n")[0]
        return json.loads(line)

    async def close(self):
        if self.client and self.client.is_connected:
            await self.client.disconnect()

    def label(self):
        return f"ble:{self.address}"


class SerialTransport(Transport):
    def __init__(self, port: str):
        self.port = port
        self.ser = None
        self._id = 0

    async def connect(self):
        import serial as pyserial
        self.ser = pyserial.Serial(self.port, 115200, timeout=1)
        self.ser.reset_input_buffer()

    async def rpc(self, method, params=None, timeout=5.0):
        self._id += 1
        req = {"jsonrpc": "2.0", "method": method, "id": self._id}
        if params:
            req["params"] = params
        payload = json.dumps(req) + "\n"
        self.ser.write(payload.encode())
        self.ser.flush()

        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            line = self.ser.readline().decode("utf-8", errors="replace").strip()
            if not line:
                continue
            if line.startswith("{"):
                try:
                    msg = json.loads(line)
                    if msg.get("id") == self._id:
                        return msg
                except json.JSONDecodeError:
                    continue
        raise TimeoutError(f"RPC timeout: {method}")

    async def close(self):
        if self.ser:
            self.ser.close()

    def label(self):
        return f"serial:{self.port}"


def make_transport(spec: str) -> Transport:
    if spec.startswith("ws://") or spec.startswith("wss://"):
        return WSTransport(spec)
    elif spec.startswith("ble:"):
        return BLETransport(spec[4:])
    elif spec.startswith("serial:"):
        return SerialTransport(spec[7:])
    else:
        # Assume WebSocket URL if it looks like an IP
        return WSTransport(spec)


# ── Test Framework ───────────────────────────────────────────────────

@dataclass
class TestResult:
    name: str
    passed: bool
    duration_ms: float
    detail: str = ""
    error: str = ""

@dataclass
class TestContext:
    node1: Transport
    node2: Transport
    node1_addr: str = ""
    node2_addr: str = ""
    results: list = field(default_factory=list)
    original_radio: dict = field(default_factory=dict)
    original_name: str = ""

    def record(self, name: str, passed: bool, duration_ms: float, detail="", error=""):
        r = TestResult(name, passed, duration_ms, detail, error)
        self.results.append(r)
        icon = "✅" if passed else "❌"
        print(f"  {icon} {name} ({duration_ms:.0f}ms){f' — {detail}' if detail else ''}{f' ERROR: {error}' if error else ''}")


async def timed_rpc(transport, method, params=None, timeout=5.0):
    t0 = time.monotonic()
    result = await transport.rpc(method, params, timeout)
    elapsed = (time.monotonic() - t0) * 1000
    return result, elapsed


# ── Individual Tests ─────────────────────────────────────────────────

async def test_ping(ctx: TestContext):
    """Basic connectivity check"""
    try:
        r, ms = await timed_rpc(ctx.node1, "bramble.ping")
        ok = r.get("result", {}).get("pong") is True
        ctx.record("ping", ok, ms, f"pong={r.get('result', {}).get('pong')}")
    except Exception as e:
        ctx.record("ping", False, 0, error=str(e))

async def test_get_version(ctx: TestContext):
    try:
        r, ms = await timed_rpc(ctx.node1, "bramble.getVersion")
        res = r.get("result", {})
        ok = "firmware_version" in res and "protocol_version" in res
        ctx.record("getVersion", ok, ms, f"fw={res.get('firmware_version')} proto={res.get('protocol_version')}")
    except Exception as e:
        ctx.record("getVersion", False, 0, error=str(e))

async def test_get_identity(ctx: TestContext):
    try:
        r, ms = await timed_rpc(ctx.node1, "bramble.getIdentity")
        res = r.get("result", {})
        addr = res.get("address", "")
        ok = len(addr) == 8
        ctx.node1_addr = addr
        ctx.record("getIdentity", ok, ms, f"addr={addr}")
    except Exception as e:
        ctx.record("getIdentity", False, 0, error=str(e))

async def test_get_identity_node2(ctx: TestContext):
    try:
        r, ms = await timed_rpc(ctx.node2, "bramble.getIdentity")
        res = r.get("result", {})
        addr = res.get("address", "")
        ok = len(addr) == 8 and addr != ctx.node1_addr
        ctx.node2_addr = addr
        ctx.record("getIdentity (node2)", ok, ms, f"addr={addr}")
    except Exception as e:
        ctx.record("getIdentity (node2)", False, 0, error=str(e))

async def test_get_status(ctx: TestContext):
    try:
        r, ms = await timed_rpc(ctx.node1, "bramble.getStatus")
        res = r.get("result", {})
        ok = res.get("radio_ok") is True and "uptime_s" in res
        ctx.record("getStatus", ok, ms, f"radio={res.get('radio_ok')} uptime={res.get('uptime_s')}s peers={res.get('peers')}")
    except Exception as e:
        ctx.record("getStatus", False, 0, error=str(e))

async def test_get_config(ctx: TestContext):
    try:
        r, ms = await timed_rpc(ctx.node1, "bramble.getConfig")
        res = r.get("result", {})
        ok = "radio" in res and "channels" in res
        radio = res.get("radio", {})
        ctx.original_radio = radio
        ctx.original_name = res.get("node_name", "")
        ctx.record("getConfig", ok, ms, f"sf={radio.get('sf')} freq={radio.get('frequency_mhz')} channels={len(res.get('channels', []))}")
    except Exception as e:
        ctx.record("getConfig", False, 0, error=str(e))

async def test_get_airtime(ctx: TestContext):
    try:
        r, ms = await timed_rpc(ctx.node1, "bramble.getAirtime")
        res = r.get("result", {})
        ok = "error" not in r
        ctx.record("getAirtime", ok, ms, f"keys={list(res.keys())[:5]}")
    except Exception as e:
        ctx.record("getAirtime", False, 0, error=str(e))

async def test_get_neighbors(ctx: TestContext):
    try:
        r, ms = await timed_rpc(ctx.node1, "bramble.getNeighbors")
        res = r.get("result", {})
        neighbors = res.get("neighbors", [])
        ok = isinstance(neighbors, list)
        ctx.record("getNeighbors", ok, ms, f"count={len(neighbors)}")
    except Exception as e:
        ctx.record("getNeighbors", False, 0, error=str(e))

async def test_get_routes(ctx: TestContext):
    try:
        r, ms = await timed_rpc(ctx.node1, "bramble.getRoutes")
        res = r.get("result", {})
        routes = res.get("routes", [])
        ok = isinstance(routes, list)
        ctx.record("getRoutes", ok, ms, f"count={len(routes)}")
    except Exception as e:
        ctx.record("getRoutes", False, 0, error=str(e))

async def test_get_messages(ctx: TestContext):
    try:
        r, ms = await timed_rpc(ctx.node1, "bramble.getMessages")
        res = r.get("result", {})
        msgs = res.get("messages", [])
        ok = isinstance(msgs, list)
        ctx.record("getMessages", ok, ms, f"count={len(msgs)}")
    except Exception as e:
        ctx.record("getMessages", False, 0, error=str(e))

async def test_get_peer_locations(ctx: TestContext):
    try:
        r, ms = await timed_rpc(ctx.node1, "bramble.getPeerLocations")
        res = r.get("result", {})
        ok = "peers" in res or "locations" in res
        ctx.record("getPeerLocations", ok, ms)
    except Exception as e:
        ctx.record("getPeerLocations", False, 0, error=str(e))

async def test_set_node_name(ctx: TestContext):
    """Set name, verify via getConfig, then restore"""
    try:
        test_name = "E2E-Test-Node"
        r, ms = await timed_rpc(ctx.node1, "bramble.setNodeName", {"name": test_name})
        ok = r.get("result", {}).get("ok") is True
        # Verify
        r2, _ = await timed_rpc(ctx.node1, "bramble.getConfig")
        actual = r2.get("result", {}).get("node_name", "")
        ok = ok and actual == test_name
        # Restore
        restore = ctx.original_name if ctx.original_name else "(unnamed)"
        await timed_rpc(ctx.node1, "bramble.setNodeName", {"name": restore})
        ctx.record("setNodeName", ok, ms, f"set='{test_name}' verified={actual == test_name}")
    except Exception as e:
        ctx.record("setNodeName", False, 0, error=str(e))

async def test_set_radio(ctx: TestContext):
    """Change SF, verify, restore"""
    try:
        orig_sf = ctx.original_radio.get("sf", 9)
        test_sf = 10 if orig_sf != 10 else 11
        r, ms = await timed_rpc(ctx.node1, "bramble.setRadio", {"sf": test_sf})
        ok = r.get("result", {}).get("ok") is True
        returned_sf = r.get("result", {}).get("sf")
        ok = ok and returned_sf == test_sf
        # Restore original
        await timed_rpc(ctx.node1, "bramble.setRadio", {"sf": orig_sf})
        ctx.record("setRadio", ok, ms, f"sf {orig_sf}→{test_sf} (restored)")
    except Exception as e:
        ctx.record("setRadio", False, 0, error=str(e))

async def test_add_remove_channel(ctx: TestContext):
    """Add a channel, verify in config, remove it"""
    try:
        r, ms = await timed_rpc(ctx.node1, "bramble.addChannel", {"name": "e2e-test", "psk": "testkey123"})
        ok = r.get("result", {}).get("ok") is True
        ch_idx = r.get("result", {}).get("index", -1)

        # Verify via getConfig
        r2, _ = await timed_rpc(ctx.node1, "bramble.getConfig")
        channels = r2.get("result", {}).get("channels", [])
        found = any(c.get("name") == "e2e-test" for c in channels)
        ok = ok and found

        # Remove
        r3, _ = await timed_rpc(ctx.node1, "bramble.removeChannel", {"index": ch_idx})
        removed = r3.get("result", {}).get("ok") is True
        ok = ok and removed

        ctx.record("addChannel + removeChannel", ok, ms, f"idx={ch_idx} found={found} removed={removed}")
    except Exception as e:
        ctx.record("addChannel + removeChannel", False, 0, error=str(e))

async def test_set_default_channel(ctx: TestContext):
    """Add a channel, set as default, restore, remove"""
    try:
        # Add temp channel
        r, _ = await timed_rpc(ctx.node1, "bramble.addChannel", {"name": "tmp-default", "psk": "tmpkey"})
        idx = r.get("result", {}).get("index", -1)

        # Set as default
        r2, ms = await timed_rpc(ctx.node1, "bramble.setDefaultChannel", {"index": idx})
        ok = r2.get("result", {}).get("ok") is True

        # Restore public as default (now at idx since they swapped)
        await timed_rpc(ctx.node1, "bramble.setDefaultChannel", {"index": idx})

        # Remove temp channel
        await timed_rpc(ctx.node1, "bramble.removeChannel", {"index": idx})

        ctx.record("setDefaultChannel", ok, ms, f"set idx={idx}")
    except Exception as e:
        ctx.record("setDefaultChannel", False, 0, error=str(e))

async def test_set_mailbox(ctx: TestContext):
    try:
        r, ms = await timed_rpc(ctx.node1, "bramble.setMailbox", {"enabled": True})
        ok = r.get("result", {}).get("ok") is True
        # Disable
        await timed_rpc(ctx.node1, "bramble.setMailbox", {"enabled": False})
        ctx.record("setMailbox", ok, ms, f"enabled→disabled")
    except Exception as e:
        ctx.record("setMailbox", False, 0, error=str(e))

async def test_set_location_config(ctx: TestContext):
    try:
        r, ms = await timed_rpc(ctx.node1, "bramble.setLocationConfig", {
            "enabled": True, "lat": 36.0133, "lon": -115.0364, "default_tier": "zone"
        })
        ok = r.get("result", {}).get("ok") is True
        # Disable
        await timed_rpc(ctx.node1, "bramble.setLocationConfig", {"enabled": False})
        ctx.record("setLocationConfig", ok, ms)
    except Exception as e:
        ctx.record("setLocationConfig", False, 0, error=str(e))

async def test_set_location_contact(ctx: TestContext):
    try:
        addr = ctx.node2_addr or "DEADBEEF"
        r, ms = await timed_rpc(ctx.node1, "bramble.setLocationContact", {"address": addr, "tier": "zone"})
        ok = r.get("result", {}).get("ok") is True
        # Remove
        r2, _ = await timed_rpc(ctx.node1, "bramble.removeLocationContact", {"address": addr})
        removed = r2.get("result", {}).get("ok") is True
        ctx.record("setLocationContact + remove", ok and removed, ms, f"addr={addr}")
    except Exception as e:
        ctx.record("setLocationContact + remove", False, 0, error=str(e))

async def test_share_location_once(ctx: TestContext):
    try:
        # Enable location first
        await timed_rpc(ctx.node1, "bramble.setLocationConfig", {"enabled": True, "lat": 36.0, "lon": -115.0})
        addr = ctx.node2_addr or "DEADBEEF"
        r, ms = await timed_rpc(ctx.node1, "bramble.shareLocationOnce", {"address": addr})
        res = r.get("result", {})
        ok = res.get("ok") is True or "note" in res  # ok or has note about no GPS
        # Disable
        await timed_rpc(ctx.node1, "bramble.setLocationConfig", {"enabled": False})
        ctx.record("shareLocationOnce", ok, ms, f"note={res.get('note', 'none')}")
    except Exception as e:
        ctx.record("shareLocationOnce", False, 0, error=str(e))

async def test_send_probe(ctx: TestContext):
    try:
        r, ms = await timed_rpc(ctx.node1, "bramble.sendProbe")
        ok = r.get("result", {}).get("ok") is True
        pid = r.get("result", {}).get("probe_id", "")
        ctx.record("sendProbe", ok, ms, f"probe_id={pid}")
    except Exception as e:
        ctx.record("sendProbe", False, 0, error=str(e))

async def test_send_broadcast(ctx: TestContext):
    """Broadcast from node1, check node2 receives it"""
    try:
        msg = f"e2e-test-{int(time.time())}"
        r, ms = await timed_rpc(ctx.node1, "bramble.sendBroadcast", {"text": msg})
        res = r.get("result", {})
        ok = res.get("ok") is True or res.get("status") == "sent"

        # Wait for LoRa delivery, then check node2 messages
        await asyncio.sleep(3)
        r2, _ = await timed_rpc(ctx.node2, "bramble.getMessages")
        msgs = r2.get("result", {}).get("messages", [])
        received = any(m.get("text") == msg for m in msgs)

        ctx.record("sendBroadcast", ok, ms, f"sent='{msg}' received_by_node2={received}")
    except Exception as e:
        ctx.record("sendBroadcast", False, 0, error=str(e))

async def test_send_message(ctx: TestContext):
    """DM from node1 to node2"""
    try:
        msg = f"dm-test-{int(time.time())}"
        r, ms = await timed_rpc(ctx.node1, "bramble.sendMessage", {"dest": ctx.node2_addr, "text": msg})
        res = r.get("result", {})
        ok = res.get("ok") is True or res.get("status") == "sent"

        # Wait and check
        await asyncio.sleep(2)
        r2, _ = await timed_rpc(ctx.node2, "bramble.getMessages")
        msgs = r2.get("result", {}).get("messages", [])
        received = any(m.get("text") == msg for m in msgs)

        ctx.record("sendMessage (DM)", ok, ms, f"to={ctx.node2_addr} received={received}")
    except Exception as e:
        ctx.record("sendMessage (DM)", False, 0, error=str(e))

# Note: reboot is intentionally excluded — it would disrupt the test run


# ── Test Runner ──────────────────────────────────────────────────────

ALL_TESTS = [
    # Phase 1: Basic connectivity (single node)
    ("ping", test_ping),
    ("getVersion", test_get_version),
    ("getIdentity", test_get_identity),
    ("getIdentity (node2)", test_get_identity_node2),
    ("getStatus", test_get_status),
    ("getConfig", test_get_config),
    ("getAirtime", test_get_airtime),
    ("getNeighbors", test_get_neighbors),
    ("getRoutes", test_get_routes),
    ("getMessages", test_get_messages),
    ("getPeerLocations", test_get_peer_locations),

    # Phase 2: Configuration mutations (single node, restore after each)
    ("setNodeName", test_set_node_name),
    ("setRadio", test_set_radio),
    ("addChannel + removeChannel", test_add_remove_channel),
    ("setDefaultChannel", test_set_default_channel),
    ("setMailbox", test_set_mailbox),
    ("setLocationConfig", test_set_location_config),
    ("setLocationContact + remove", test_set_location_contact),
    ("shareLocationOnce", test_share_location_once),
    ("sendProbe", test_send_probe),

    # Phase 3: Two-node messaging
    ("sendBroadcast", test_send_broadcast),
    ("sendMessage (DM)", test_send_message),
]


async def run_tests(node1_spec: str, node2_spec: str):
    node1 = make_transport(node1_spec)
    node2 = make_transport(node2_spec)

    print(f"\n{'='*60}")
    print(f"Bramble E2E Test Suite")
    print(f"{'='*60}")
    print(f"  Node 1: {node1.label()}")
    print(f"  Node 2: {node2.label()}")
    print(f"{'='*60}\n")

    # Connect
    print("Connecting to nodes...")
    try:
        await node1.connect()
        print(f"  ✅ Node 1 connected")
    except Exception as e:
        print(f"  ❌ Node 1 connect failed: {e}")
        return 1

    try:
        await node2.connect()
        print(f"  ✅ Node 2 connected")
    except Exception as e:
        print(f"  ❌ Node 2 connect failed: {e}")
        await node1.close()
        return 1

    ctx = TestContext(node1=node1, node2=node2)

    print(f"\nRunning {len(ALL_TESTS)} tests...\n")

    t0 = time.monotonic()
    for name, test_fn in ALL_TESTS:
        try:
            await test_fn(ctx)
        except Exception as e:
            ctx.record(name, False, 0, error=f"Unhandled: {e}")

    total_ms = (time.monotonic() - t0) * 1000

    # Cleanup
    await node1.close()
    await node2.close()

    # Summary
    passed = sum(1 for r in ctx.results if r.passed)
    failed = sum(1 for r in ctx.results if not r.passed)
    total = len(ctx.results)

    print(f"\n{'='*60}")
    print(f"Results: {passed}/{total} passed, {failed} failed ({total_ms:.0f}ms total)")
    print(f"{'='*60}")

    if failed > 0:
        print("\nFailed tests:")
        for r in ctx.results:
            if not r.passed:
                print(f"  ❌ {r.name}: {r.error or r.detail}")

    return 0 if failed == 0 else 1


def main():
    parser = argparse.ArgumentParser(
        description="Bramble E2E device test suite",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Transport formats:
  ws://IP/ws           WebSocket
  ble:MAC_ADDRESS      Bluetooth Low Energy
  serial:/dev/ttyUSBN  Serial/UART

Examples:
  %(prog)s ws://192.168.1.64/ws ws://192.168.1.21/ws
  %(prog)s ble:F0:9E:9E:75:5A:9E ble:10:51:DB:57:9A:12
  %(prog)s serial:/dev/ttyUSB0 serial:/dev/ttyUSB1
        """,
    )
    parser.add_argument("node1", help="Node 1 transport (e.g. ws://IP/ws)")
    parser.add_argument("node2", help="Node 2 transport (e.g. ws://IP2/ws)")
    args = parser.parse_args()

    rc = asyncio.run(run_tests(args.node1, args.node2))
    sys.exit(rc)


if __name__ == "__main__":
    main()
