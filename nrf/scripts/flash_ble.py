#!/usr/bin/env python3
"""Zero-touch reflash for UF2-bootloader Bramble nRF boards (T1000-E).

Connects over BLE (NUS), authenticates, calls bramble.enterDfu, waits for
the UF2 mass-storage volume, copies the image, and watches the device come
back up. Requires the device to be advertising (disconnect other clients
first: the link is single-connection).

Usage:
  uv run --with bleak python nrf/scripts/flash_ble.py NAME UF2 --token HEX
  (NAME e.g. Bramble-AA36; add --skip-dfu if the device is already in DFU)

The auth token is the device's RPC token; pass it on the command line or via
BRAMBLE_TOKEN in the environment. Never commit token values.
"""

import argparse
import asyncio
import json
import os
import shutil
import subprocess
import sys
import time

NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # write
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # notify


def find_uf2_mount(timeout_s: int) -> str | None:
    """Waits for the T1000-E UF2 volume and returns its mountpoint."""
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        out = subprocess.run(
            ["lsblk", "-o", "NAME,LABEL", "-nr"], capture_output=True, text=True
        ).stdout
        for line in out.splitlines():
            parts = line.split()
            if len(parts) == 2 and parts[1] == "T1000-E":
                dev = f"/dev/{parts[0]}"
                mnt = subprocess.run(
                    ["findmnt", "-n", "-o", "TARGET", "--source", dev],
                    capture_output=True,
                    text=True,
                ).stdout.strip()
                if not mnt:
                    subprocess.run(
                        ["udisksctl", "mount", "-b", dev],
                        capture_output=True,
                        text=True,
                    )
                    mnt = subprocess.run(
                        ["findmnt", "-n", "-o", "TARGET", "--source", dev],
                        capture_output=True,
                        text=True,
                    ).stdout.strip()
                if mnt:
                    return mnt
        time.sleep(1)
    return None


async def rpc_enter_dfu(name: str, token: str) -> bool:
    from bleak import BleakClient, BleakScanner

    dev = await BleakScanner.find_device_by_name(name, timeout=20)
    if dev is None:
        print(f"{name} not advertising (is another client connected?)")
        return False
    responses: asyncio.Queue = asyncio.Queue()
    buf = bytearray()

    def on_notify(_h, data: bytearray):
        buf.extend(data)
        while b"\n" in buf:
            line, _, rest = bytes(buf).partition(b"\n")
            buf.clear()
            buf.extend(rest)
            responses.put_nowait(line)

    async with BleakClient(dev, timeout=30) as client:
        # The NUS characteristics are WRITE_ENC/NOTIFY on an encrypted link
        # only (LE Secure Connections, Just Works): an unpaired write makes
        # the ATT server demand encryption and BlueZ drops the connection if
        # it cannot elevate. Pair explicitly first; a no-op when bonded.
        try:
            await client.pair()
        except Exception as e:
            print(f"pairing: {e} (continuing; may already be bonded)")
        await client.start_notify(NUS_TX, on_notify)

        async def call(payload: dict) -> dict:
            data = (json.dumps(payload) + "\n").encode()
            for i in range(0, len(data), 200):
                await client.write_gatt_char(NUS_RX, data[i : i + 200], response=True)
            raw = await asyncio.wait_for(responses.get(), timeout=15)
            return json.loads(raw)

        auth = await call({"jsonrpc": "2.0", "method": "auth", "params": {"token": token}, "id": 1})
        if not (auth.get("result") or {}).get("ok"):
            print(f"auth failed: {auth}")
            return False
        print("authenticated")
        resp = await call({"jsonrpc": "2.0", "method": "bramble.enterDfu", "params": {}, "id": 2})
        result = resp.get("result") or {}
        if result.get("error"):
            print(f"enterDfu rejected: {result['error']}")
            return False
        print("enterDfu accepted; device resets in ~500ms")
        return True


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("name", help="BLE device name, e.g. Bramble-AA36")
    ap.add_argument("uf2", help="path to the padded UF2 image")
    ap.add_argument("--token", default=os.environ.get("BRAMBLE_TOKEN", ""))
    ap.add_argument("--skip-dfu", action="store_true", help="device is already in DFU")
    args = ap.parse_args()

    if not args.skip_dfu:
        if not args.token:
            print("no token (pass --token or set BRAMBLE_TOKEN)")
            return 1
        if not asyncio.run(rpc_enter_dfu(args.name, args.token)):
            return 1

    print("waiting for the UF2 volume...")
    mnt = find_uf2_mount(60)
    if mnt is None:
        print("UF2 volume never appeared")
        return 1
    print(f"mounted at {mnt}; copying image")
    shutil.copy(args.uf2, os.path.join(mnt, "FLASH.UF2"))
    subprocess.run(["sync"])
    print("copy complete; waiting for the app to boot")
    deadline = time.time() + 30
    while time.time() < deadline:
        if subprocess.run(["lsusb", "-d", "2886:0057"], capture_output=True).returncode != 0:
            print("bootloader gone: app is booting")
            return 0
        time.sleep(1)
    print("device stayed in DFU; check the trace page")
    return 1


if __name__ == "__main__":
    sys.exit(main())
