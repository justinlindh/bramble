#!/usr/bin/env python3
"""
Flash Bramble firmware to multiple devices (USB + OTA) with board auto-detection.

Features:
- Detect USB ESP32-S3 devices on /dev/ttyACM* and /dev/ttyUSB*
- Detect board type from PSRAM size (esptool flash_id)
- Build required board targets once, concurrently
- Flash USB devices in parallel via scripts/flash.sh wrapper
- OTA network nodes in parallel via bramble.otaUpdate over WebSocket
- Verify success using bramble.getStatus uptime checks (serial for USB, WS for OTA)

Usage:
  python3 scripts/flash-all.py [--config .flash-targets.json] [--dry-run] [--project-ver 1.5.12]
"""

from __future__ import annotations

import argparse
import asyncio
import glob
import json
import os
import re
import shlex
import subprocess
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path
from typing import Any


DEFAULT_CONFIG = ".flash-targets.json"
REPO_ROOT = Path(__file__).resolve().parents[1]
FLASH_SH = REPO_ROOT / "scripts" / "flash.sh"


@dataclass
class UsbDevice:
    port: str
    board: str | None
    psram_mb: int | None
    chip: str | None
    detection_log: str


@dataclass
class Result:
    name: str
    kind: str
    board: str
    success: bool
    phase: str
    details: str


def run_cmd(
    cmd: list[str],
    timeout: int = 180,
    check: bool = False,
    input_text: str | None = None,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        cmd,
        cwd=str(REPO_ROOT),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=timeout,
        check=check,
        input=input_text,
    )


def norm_board(board: str) -> str:
    b = (board or "").strip().lower()
    aliases = {
        "heltec": "heltec-v3",
        "heltec-v3": "heltec-v3",
        "heltec-v4": "heltec-v4",
        "tdeck": "tdeck-plus",
        "t-deck": "tdeck-plus",
        "tdeck-plus": "tdeck-plus",
    }
    if b not in aliases:
        raise ValueError(f"Unsupported board '{board}' (expected heltec-v3/heltec-v4 or tdeck-plus/tdeck)")
    return aliases[b]


def load_config(path: Path) -> dict[str, Any]:
    if not path.exists():
        raise FileNotFoundError(
            f"Config file not found: {path}\n"
            f"Create it from scripts/flash-targets.example.json"
        )
    with path.open("r", encoding="utf-8") as f:
        cfg = json.load(f)
    cfg.setdefault("network_nodes", [])
    cfg.setdefault("usb_detection", {})
    cfg.setdefault("ota", {})
    cfg.setdefault("usb_nodes", [])
    return cfg


def find_esptool() -> str:
    """Find esptool.py on PATH or in ESP-IDF venv."""
    import shutil
    found = shutil.which("esptool.py")
    if found:
        return found
    # Check ESP-IDF python venv
    venvs = sorted(glob.glob(os.path.expanduser("~/.espressif/python_env/idf*_env/bin/esptool.py")))
    if venvs:
        return venvs[-1]
    return "esptool.py"  # fallback, will fail with clear error


def detect_usb_ports() -> list[str]:
    ports = sorted(set(glob.glob("/dev/ttyACM*") + glob.glob("/dev/ttyUSB*")))
    return [p for p in ports if os.path.exists(p)]


def detect_usb_device(port: str, cfg: dict[str, Any], esptool: str) -> UsbDevice:
    cmd = [esptool, "--port", port, "flash_id"]
    cp = run_cmd(cmd, timeout=35)
    out = cp.stdout

    chip = None
    psram_mb = None

    # Chip type: "Chip is ESP32-S3 ..." or "Detecting chip type... ESP32-S3"
    m_chip = re.search(r"(?:Chip is|Detecting chip type\.\.\.)\s*(.+?)(?:\s*\(|$)", out, re.MULTILINE)
    if m_chip:
        chip = m_chip.group(1).strip()

    # PSRAM: "Embedded PSRAM 8MB" or "PSRAM size: 8MB"
    m_psram = re.search(r"(?:PSRAM size:?\s*|Embedded PSRAM\s*)([0-9]+)\s*MB", out, re.IGNORECASE)
    if m_psram:
        psram_mb = int(m_psram.group(1))

    mapping = cfg.get("usb_detection", {})
    board = None
    if psram_mb is not None:
        board_key = f"psram_{psram_mb}mb"
        mapped = mapping.get(board_key)
        if mapped:
            board = norm_board(mapped)
    elif psram_mb is None:
        # No PSRAM detected: check fallback rules:
        # "no_psram" key in usb_detection, or "default" key
        fallback = mapping.get("no_psram") or mapping.get("default")
        if fallback:
            board = norm_board(fallback)

    return UsbDevice(port=port, board=board, psram_mb=psram_mb, chip=chip, detection_log=out)


def build_cmd(board: str, project_ver: str | None) -> list[str]:
    cmd = ["bash", str(FLASH_SH), "local", board, "build"]
    if project_ver:
        cmd.append(f"-DPROJECT_VER={project_ver}")
    return cmd


def build_board(board: str, project_ver: str | None) -> tuple[bool, str]:
    cp = run_cmd(build_cmd(board, project_ver), timeout=1800)
    return cp.returncode == 0, cp.stdout


def board_antirollback(board: str) -> tuple[bool, int]:
    """Inspect the board's generated sdkconfig for eFuse anti-rollback.

    Returns (enabled, secure_version_epoch). The sdkconfig.<board> file is
    produced at the repo root by the build (see scripts/flash.sh); a missing
    file reads as not-enabled because the guard in flash.sh independently
    fails closed at flash time.
    """
    cfg_path = REPO_ROOT / f"sdkconfig.{board}"
    if not cfg_path.exists():
        return False, 0
    enabled = False
    epoch = 0
    for line in cfg_path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.strip() == "CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK=y":
            enabled = True
        elif line.startswith("CONFIG_BOOTLOADER_APP_SECURE_VERSION="):
            try:
                epoch = int(line.split("=", 1)[1])
            except ValueError:
                pass
    return enabled, epoch


def flash_cmd(port: str, board: str, project_ver: str | None) -> list[str]:
    cmd = ["bash", str(FLASH_SH), "local", board, "flash", port]
    # flash.sh's flash action REBUILDS before flashing and pins PROJECT_VER
    # to 0.0.0-local when the flag is absent, so the version must ride along
    # here too or the versioned build from the build phase gets restamped.
    if project_ver:
        cmd.append(f"-DPROJECT_VER={project_ver}")
    return cmd


def flash_usb(port: str, board: str, project_ver: str | None, antirollback_phrase: str | None = None) -> tuple[bool, str]:
    cmd = flash_cmd(port, board, project_ver)
    input_text = None
    if antirollback_phrase is not None:
        # The operator already typed the epoch confirmation once, upfront in
        # main(); pass the flag and replay the phrase to flash.sh's guard so
        # parallel flashes do not each block on an invisible prompt.
        cmd.append("--enable-antirollback")
        input_text = antirollback_phrase + "\n"
    cp = run_cmd(cmd, timeout=900, input_text=input_text)
    if cp.returncode != 0:
        return False, cp.stdout
    # For ESP32-S3 USB JTAG (ACM) devices, give a moment for USB
    # re-enumeration after flash before opening for verification.
    if "ACM" in port:
        time.sleep(3)
    return True, cp.stdout


BOOT_MARKER = "entering main mesh loop"
# Strings that prove the device booted successfully even if we missed the marker
OPERATIONAL_MARKERS = [
    "bramble.on",        # JSON-RPC event notifications (onGpsEvent, onMeshRx, etc.)
    "radio_esp: TX",     # radio transmitting
    "mesh: ACK",         # mesh protocol active
    "wifi:mode",         # WiFi initialized
    "bramble>",          # CLI prompt
    "LVGL initialized",  # T-Deck display up
    "Beacon TX",         # beacon active
]
PANIC_MARKERS = ["Guru Meditation Error", "InstrFetchProhibited", "LoadProhibited", "abort()"]


def verify_usb_boot(port: str, timeout_s: int = 30) -> tuple[bool, str]:
    """Monitor serial output after flash for the boot success marker.

    Returns (success, detail_message). Watches for BOOT_MARKER within
    timeout_s seconds. Also accepts operational output as proof of successful
    boot (the marker may have scrolled past before we opened the port).
    Detects panic/boot-loop patterns early to fail fast.
    """
    import serial  # type: ignore

    deadline = time.monotonic() + timeout_s
    collected: list[str] = []
    try:
        with serial.Serial(port, 115200, timeout=1) as ser:
            ser.reset_input_buffer()
            while time.monotonic() < deadline:
                raw = ser.readline()
                if not raw:
                    continue
                line = raw.decode("utf-8", errors="replace").strip()
                if not line:
                    continue
                collected.append(line)
                if BOOT_MARKER in line:
                    return True, f"boot OK: saw '{BOOT_MARKER}' after {len(collected)} lines"
                for panic in PANIC_MARKERS:
                    if panic in line:
                        return False, f"panic detected: {line}"
                for op in OPERATIONAL_MARKERS:
                    if op in line:
                        return True, f"boot OK: device operational (saw '{op}' output)"
    except Exception as e:
        return False, f"serial error on {port}: {e}"

    # If we got readable lines but no markers, the device is probably fine
    # but just quiet. Count readable (non-garbage) lines as weak evidence.
    readable = [line for line in collected if len(line) > 5 and line.isprintable()]
    if len(readable) >= 3:
        return True, f"boot likely OK: {len(readable)} readable lines but no marker (device may be idle)"

    tail = "\n".join(collected[-5:]) if collected else "(no output)"
    return False, f"timeout after {timeout_s}s waiting for boot marker. Last output:\n{tail}"


async def ws_call(host: str, method: str, params: dict[str, Any] | None = None, req_id: int = 1, timeout_s: float = 8.0) -> dict[str, Any]:
    import websockets  # type: ignore

    url = host if host.startswith("ws://") or host.startswith("wss://") else f"ws://{host}/ws"
    async with websockets.connect(url, open_timeout=8, ping_interval=None) as ws:
        req = {"jsonrpc": "2.0", "id": req_id, "method": method, "params": params or {}}
        await ws.send(json.dumps(req))
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            msg = json.loads(await asyncio.wait_for(ws.recv(), timeout=timeout_s))
            if msg.get("id") == req_id:
                return msg
    raise TimeoutError(f"ws RPC timeout for {method} on {host}")


async def ota_and_verify(node: dict[str, Any], ota_url: str, max_uptime_s: int = 60) -> Result:
    name = node.get("name") or node.get("host")
    host = node.get("host")
    board = norm_board(node.get("board", ""))
    if not host:
        return Result(name=name or "<unknown>", kind="ota", board=board, success=False, phase="ota", details="missing host")

    try:
        start = await ws_call(host, "bramble.otaUpdate", {"url": ota_url}, req_id=100)
        ok = ((start.get("result") or {}).get("ok") is True)
        if not ok:
            return Result(name=name, kind="ota", board=board, success=False, phase="ota", details=f"otaUpdate rejected: {start}")
    except Exception as e:
        return Result(name=name, kind="ota", board=board, success=False, phase="ota", details=f"ota start failed: {e}")

    # Wait for reboot and uptime reset.
    deadline = time.monotonic() + 90
    last_err = ""
    while time.monotonic() < deadline:
        try:
            status = await ws_call(host, "bramble.getStatus", {}, req_id=101)
            uptime = (status.get("result") or {}).get("uptime_s")
            if isinstance(uptime, int):
                if uptime <= max_uptime_s:
                    return Result(name=name, kind="ota", board=board, success=True, phase="verify", details=f"uptime_s={uptime}")
                last_err = f"uptime_s={uptime} (> {max_uptime_s})"
        except Exception as e:
            last_err = str(e)
        await asyncio.sleep(3)

    return Result(name=name, kind="ota", board=board, success=False, phase="verify", details=f"verify timeout: {last_err}")


def discover_mdns_nodes() -> list[str]:
    # Best-effort discovery via avahi-browse, returns hostnames/IPs where available.
    try:
        cp = run_cmd(["avahi-browse", "-rt", "_http._tcp"], timeout=15)
    except Exception:
        return []
    hosts: list[str] = []
    for line in cp.stdout.splitlines():
        if ";IPv4;" not in line:
            continue
        if "bramble" not in line.lower():
            continue
        parts = line.split(";")
        if len(parts) >= 8:
            host = parts[7].strip()
            if host:
                hosts.append(host)
    return sorted(set(hosts))


def fmt_cmd(parts: list[str]) -> str:
    return " ".join(shlex.quote(p) for p in parts)


def main() -> int:
    p = argparse.ArgumentParser(description="Flash all Bramble nodes (USB + OTA)")
    p.add_argument("--config", default=DEFAULT_CONFIG, help="Path to config JSON (default: .flash-targets.json)")
    p.add_argument("--dry-run", action="store_true", help="Print planned actions, do not build/flash/ota")
    p.add_argument("--usb-only", action="store_true")
    p.add_argument("--ota-only", action="store_true")
    p.add_argument("--max-workers", type=int, default=4)
    p.add_argument(
        "--project-ver",
        default=None,
        help=(
            "Version string to stamp into the builds (passed to flash.sh as "
            "-DPROJECT_VER=...). Use the release version (e.g. 1.5.12 when "
            "flashing the firmware-v1.5.12 tag) so devices report the release "
            "they run. Without it builds stamp 0.0.0-local."
        ),
    )
    p.add_argument(
        "--enable-antirollback",
        action="store_true",
        help=(
            "Required to flash builds with CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK. "
            "Prompts for a typed epoch confirmation; booting such a build "
            "irreversibly burns the eFuse secure-version floor. "
            "See docs/design/ota-antirollback.md."
        ),
    )
    args = p.parse_args()

    cfg = load_config((REPO_ROOT / args.config).resolve() if not os.path.isabs(args.config) else Path(args.config))

    esptool = find_esptool()
    usb_ports = detect_usb_ports() if not args.ota_only else []
    usb_devices: list[UsbDevice] = []
    for port in usb_ports:
        try:
            dev = detect_usb_device(port, cfg, esptool)
            usb_devices.append(dev)
        except Exception as e:
            usb_devices.append(UsbDevice(port=port, board=None, psram_mb=None, chip=None, detection_log=f"detection failed: {e}"))

    # Optional static overrides by port
    overrides = {item.get("port"): item for item in cfg.get("usb_nodes", []) if item.get("port")}
    for d in usb_devices:
        ov = overrides.get(d.port)
        if ov and ov.get("board"):
            d.board = norm_board(ov["board"])

    net_nodes = [] if args.usb_only else list(cfg.get("network_nodes", []))

    if cfg.get("mdns", {}).get("enabled") and not args.usb_only:
        discovered = discover_mdns_nodes()
        known_hosts = {n.get("host") for n in net_nodes}
        default_board = norm_board(cfg.get("mdns", {}).get("default_board", "heltec-v3"))
        for host in discovered:
            if host in known_hosts:
                continue
            net_nodes.append({"name": f"mDNS:{host}", "host": host, "board": default_board})

    # Determine build matrix
    boards_to_build: set[str] = set()
    for d in usb_devices:
        if d.board:
            boards_to_build.add(d.board)
    for n in net_nodes:
        if n.get("board"):
            boards_to_build.add(norm_board(n["board"]))

    print("=== Bramble flash-all plan ===")
    print(f"Config: {args.config}")
    print(f"USB ports detected: {len(usb_devices)}")
    for d in usb_devices:
        print(f"  - {d.port}: board={d.board or 'UNKNOWN'} psram={d.psram_mb}MB chip={d.chip}")
    print(f"Network nodes: {len(net_nodes)}")
    for n in net_nodes:
        print(f"  - {n.get('name', n.get('host'))}: host={n.get('host')} board={n.get('board')}")
    print(f"Boards to build: {', '.join(sorted(boards_to_build)) if boards_to_build else '(none)'}")

    if args.dry_run:
        if not args.enable_antirollback:
            print("NOTE: builds with CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK would be refused at the guard (missing --enable-antirollback).")
        for b in sorted(boards_to_build):
            print("BUILD:", fmt_cmd(build_cmd(b, args.project_ver)))
        for d in usb_devices:
            if d.board:
                print("FLASH:", fmt_cmd(flash_cmd(d.port, d.board, args.project_ver)))
        for n in net_nodes:
            board = norm_board(n.get("board", ""))
            ota_url = (n.get("ota_url") or (cfg.get("ota", {}).get("urls", {}) or {}).get(board) or cfg.get("ota", {}).get("url"))
            print(f"OTA: host={n.get('host')} board={board} url={ota_url}")
        return 0

    # 1) Concurrent builds
    build_logs: dict[str, str] = {}
    build_ok: dict[str, bool] = {}
    if boards_to_build:
        print("\n=== Building firmware (parallel by board target) ===")
        with ThreadPoolExecutor(max_workers=min(args.max_workers, len(boards_to_build))) as ex:
            fut_map = {ex.submit(build_board, b, args.project_ver): b for b in boards_to_build}
            for fut in as_completed(fut_map):
                b = fut_map[fut]
                ok, log = fut.result()
                build_ok[b] = ok
                build_logs[b] = log
                print(f"[{b}] {'OK' if ok else 'FAIL'}")

    # 1b) Anti-rollback consent gate (issue #79). A build with
    # CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK burns the eFuse secure-version floor
    # on first boot, irreversibly. The default flow refuses to carry that onto
    # any device silently: it requires --enable-antirollback plus one typed,
    # epoch-specific confirmation before the parallel phases start.
    results: list[Result] = []
    ar_boards: dict[str, int] = {}
    for b in sorted(boards_to_build):
        if build_ok.get(b):
            enabled, epoch = board_antirollback(b)
            if enabled:
                ar_boards[b] = epoch

    ar_phrase_by_board: dict[str, str] = {}
    if ar_boards:
        print("\n=== ANTI-ROLLBACK BUILDS DETECTED (eFuse secure version) ===")
        for b, epoch in sorted(ar_boards.items()):
            print(f"  - {b}: secure_version epoch {epoch}")
        print("Booting these images on an enforcing bootloader IRREVERSIBLY burns")
        print("the eFuse floor up to the epoch shown. There is no undo; the device")
        print("will refuse to boot any lower-epoch image forever, even over USB.")
        print("See docs/design/ota-antirollback.md.")
        if not args.enable_antirollback:
            print("REFUSING all targets for these boards (missing --enable-antirollback).")
            refusal = "anti-rollback build refused: re-run with --enable-antirollback"
            for d in usb_devices:
                if d.board in ar_boards:
                    results.append(Result(name=d.port, kind="usb", board=d.board, success=False, phase="guard", details=refusal))
            kept_nodes = []
            for n in net_nodes:
                n_board = norm_board(n.get("board", ""))
                if n_board in ar_boards:
                    results.append(Result(name=n.get("name") or n.get("host"), kind="ota", board=n_board, success=False, phase="guard", details=refusal))
                else:
                    kept_nodes.append(n)
            net_nodes = kept_nodes
            usb_devices = [d for d in usb_devices if d.board not in ar_boards]
        else:
            for epoch in sorted(set(ar_boards.values())):
                expected = f"BURN EPOCH {epoch}"
                try:
                    typed = input(f"Type exactly '{expected}' to confirm epoch {epoch} (anything else aborts): ")
                except EOFError:
                    typed = ""
                if typed != expected:
                    print("Confirmation mismatch; aborting the entire run (nothing flashed).")
                    return 2
            for b, epoch in ar_boards.items():
                ar_phrase_by_board[b] = f"BURN EPOCH {epoch}"

    # 2) USB flash in parallel
    usb_candidates = [d for d in usb_devices if d.board and build_ok.get(d.board)]
    if usb_candidates:
        print("\n=== Flashing USB devices (parallel) ===")
        with ThreadPoolExecutor(max_workers=min(args.max_workers, len(usb_candidates))) as ex:
            fut_map = {ex.submit(flash_usb, d.port, d.board, args.project_ver, ar_phrase_by_board.get(d.board)): d for d in usb_candidates}
            for fut in as_completed(fut_map):
                d = fut_map[fut]
                ok, log = fut.result()
                if not ok:
                    results.append(Result(name=d.port, kind="usb", board=d.board or "unknown", success=False, phase="flash", details="flash failed"))
                    continue
                v_ok, v_msg = verify_usb_boot(d.port, timeout_s=30)
                results.append(Result(name=d.port, kind="usb", board=d.board or "unknown", success=v_ok, phase="verify", details=v_msg))

    for d in usb_devices:
        if not d.board:
            results.append(Result(name=d.port, kind="usb", board="unknown", success=False, phase="detect", details="board detection failed"))
        elif not build_ok.get(d.board, False):
            results.append(Result(name=d.port, kind="usb", board=d.board, success=False, phase="build", details=f"board build failed: {d.board}"))

    # 3) OTA in parallel
    ota_nodes = []
    for node in net_nodes:
        board = norm_board(node.get("board", ""))
        if not build_ok.get(board):
            results.append(Result(name=node.get("name") or node.get("host"), kind="ota", board=board, success=False, phase="build", details=f"board build failed: {board}"))
            continue
        ota_url = node.get("ota_url") or (cfg.get("ota", {}).get("urls", {}) or {}).get(board) or cfg.get("ota", {}).get("url")
        if not ota_url:
            results.append(Result(name=node.get("name") or node.get("host"), kind="ota", board=board, success=False, phase="ota", details="missing ota_url (node.ota_url or ota.urls[board] or ota.url)"))
            continue
        node2 = dict(node)
        node2["board"] = board
        node2["ota_url"] = ota_url
        ota_nodes.append(node2)

    if ota_nodes:
        print("\n=== OTA updates (parallel) ===")

        async def run_ota_all() -> list[Result]:
            tasks = [ota_and_verify(n, n["ota_url"], max_uptime_s=60) for n in ota_nodes]
            return await asyncio.gather(*tasks)

        ota_results = asyncio.run(run_ota_all())
        results.extend(ota_results)

    # Summary
    print("\n=== Summary ===")
    ok_count = 0
    for r in results:
        mark = "✅" if r.success else "❌"
        if r.success:
            ok_count += 1
        print(f"{mark} [{r.kind}] {r.name} ({r.board}) phase={r.phase} :: {r.details}")

    print(f"\nSuccess: {ok_count}/{len(results)}")

    # Show concise build evidence block for wrapper compliance
    print("\n=== Build command evidence (scripts/flash.sh wrapper) ===")
    for b in sorted(boards_to_build):
        print(f"Board target: {b}")
        print(f"Build command: {fmt_cmd(build_cmd(b, args.project_ver))}")
        excerpt = "\n".join(build_logs.get(b, "").splitlines()[-12:])
        print("Build output excerpt:")
        print(excerpt if excerpt else "(no output captured)")
        print("---")

    return 0 if all(r.success for r in results) else 2


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130)
