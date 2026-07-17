#!/usr/bin/env python3
"""Bench driver for the T-Deck LVGL UI: screenshot + input injection over serial.

The two firmware RPCs this wraps (bramble.screenshot, bramble.injectInput) feed
the SAME code paths a human does, so focus, groups and textarea editing behave
identically to physical input. Zero third-party deps: PNG encoding is inline
(zlib+struct), transport is scripts/bramble-rpc via subprocess.

Import (the usual way, from a REPL or driving script):

    import sys; sys.path.insert(0, "scripts")
    import importlib
    tdeck = importlib.import_module("tdeck-bench")
    tdeck.inject(type="trackball", dir="down")
    tdeck.shot("/tmp/step1.png")

CLI (one-shot checks):

    python3 scripts/tdeck-bench.py shot /tmp/screen.png
    python3 scripts/tdeck-bench.py inject '{"type":"trackball","dir":"select"}'
    python3 scripts/tdeck-bench.py rpc bramble.getStatus

Port selection: TDECK_PORT env var wins; otherwise every /dev/ttyACM* is probed
with getIdentity. If TDECK_ADDR is set, the node whose address matches it is
used; otherwise the first node whose hardware looks like a T-Deck is used.
Ports RENUMBER whenever anything replugs, so never hardcode one; set
TDECK_PORT or TDECK_ADDR for your own bench instead.

The injectInput contract (a wrong field name silently no-ops -- this burned a
whole verification round once):
    {"type":"trackball","dir":"up"|"down"|"left"|"right"|"select"}
    {"type":"key","char":"a"}          <- "char", NOT "key"
    {"type":"text","text":"hello","enter":true|false}
"""

import base64
import glob
import json
import os
import struct
import subprocess
import sys
import time
import zlib

# Interpreter used to run bramble-rpc: it just needs pyserial installed. Set
# TDECK_RPC_PYTHON if your default python3 doesn't have it, e.g. it lives in
# a pipx-managed virtualenv on your machine.
RPC_PY = os.environ.get("TDECK_RPC_PYTHON", "python3")
RPC_SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "bramble-rpc")
TDECK_ADDR = os.environ.get("TDECK_ADDR")

_port = None


def _rpc_on(port, method, params=None, timeout=60):
    cmd = [RPC_PY, RPC_SCRIPT, port, method] + ([params] if params else [])
    out = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout).stdout
    try:
        return json.loads(out)
    except (ValueError, TypeError):
        return {}


def port():
    """The T-Deck's serial port, found by ADDRESS (ports renumber on replug).

    Resolution order: TDECK_PORT env var wins outright. Otherwise every
    /dev/ttyACM* is probed with getIdentity; if TDECK_ADDR is set, the node
    whose address matches it is used, else the first node reporting
    hardware "tdeck_plus" is used.
    """
    global _port
    if _port:
        return _port
    env = os.environ.get("TDECK_PORT")
    if env:
        _port = env
        return _port
    for p in sorted(glob.glob("/dev/ttyACM*")):
        ident = _rpc_on(p, "bramble.getIdentity", timeout=25)
        if TDECK_ADDR:
            if ident.get("address") == TDECK_ADDR:
                _port = p
                return _port
        elif ident.get("hardware") == "tdeck_plus":
            _port = p
            return _port
    if TDECK_ADDR:
        raise RuntimeError(
            f"no /dev/ttyACM* answered getIdentity with address {TDECK_ADDR}; "
            "is the T-Deck plugged in? (set TDECK_PORT to override)"
        )
    raise RuntimeError(
        "no /dev/ttyACM* answered getIdentity as a T-Deck (hardware tdeck_plus); "
        "set TDECK_PORT to a specific port or TDECK_ADDR to a specific node address"
    )


def rpc(method, params=None):
    return _rpc_on(port(), method, params)


def inject(**kw):
    """inject(type='trackball', dir='down') / inject(type='text', text='hi', enter=True)"""
    return rpc("bramble.injectInput", json.dumps(kw))


def uptime():
    """Cheap liveness + unexpected-reboot detector: sample before and after a
    UI sequence; a reset means the sequence crashed the firmware."""
    return rpc("bramble.getStatus").get("uptime_s")


# The serial CLI's 16 KB PSRAM response buffer lifted the old 2048-byte
# silent-drop ceiling; the screenshot RPC clamps its own chunks at 6144, so a
# full 320x240 RGB565 frame (153600 bytes) takes 25 round trips.
CHUNK = 6144


def shot(path, retries=3):
    """Capture the live framebuffer to a PNG at path. Retries short reads:
    a chunk occasionally arrives truncated and one retry always heals it."""
    last_err = None
    for _ in range(retries):
        try:
            return _shot_once(path)
        except (struct.error, KeyError) as e:  # truncated transfer
            last_err = e
            time.sleep(1.5)
    raise RuntimeError(f"screenshot failed after {retries} attempts: {last_err}")


def _shot_once(path):
    r = rpc("bramble.screenshot", '{"capture":true,"offset":0,"max_len":%d}' % CHUNK)
    total, w, h = r["total"], r["width"], r["height"]
    buf = bytearray(base64.b64decode(r["data"]))
    while len(buf) < total:
        r2 = rpc(
            "bramble.screenshot",
            '{"capture":false,"offset":%d,"max_len":%d}' % (len(buf), CHUNK),
        )
        d = base64.b64decode(r2.get("data", "") or "")
        if not d:
            break
        buf += d
    rows = []
    for y in range(h):
        row = b"\x00"
        for x in range(w):
            v = struct.unpack_from("<H", buf, 2 * (y * w + x))[0]
            row += bytes(
                ((v >> 11 & 31) * 255 // 31, (v >> 5 & 63) * 255 // 63, (v & 31) * 255 // 31)
            )
        rows.append(row)
    raw = b"".join(rows)

    def ch(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    png = (
        b"\x89PNG\r\n\x1a\n"
        + ch(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
        + ch(b"IDAT", zlib.compress(raw))
        + ch(b"IEND", b"")
    )
    with open(path, "wb") as f:
        f.write(png)
    return path


def _main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    cmd = argv[1]
    if cmd == "shot":
        print(shot(argv[2] if len(argv) > 2 else "/tmp/tdeck.png"))
    elif cmd == "inject":
        print(json.dumps(rpc("bramble.injectInput", argv[2])))
    elif cmd == "rpc":
        print(json.dumps(rpc(argv[2], argv[3] if len(argv) > 3 else None), indent=1))
    elif cmd == "port":
        print(port())
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(_main(sys.argv))
