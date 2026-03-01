# OTA Rollout Guide (Single Node)

Last verified: 2026-03-01

Use this runbook to deploy a `bramble.bin` build to one WiFi-connected node without USB flashing.

## Prerequisites

- Node reachable over JSON-RPC WebSocket (example: `ws://192.0.2.0/ws`)
- Host machine IP reachable by the node (example: `203.0.113.34`)
- `bramble-cli` with `ota` command available
- Choose target board: `heltec-v3`, `heltec-v4`, or `tdeck-plus`

## 1) Build artifact

Use the board-aware build wrapper:

```bash
cd ~/src/bramble
bash scripts/flash.sh local heltec-v4 build
```

Expected artifact: `~/src/bramble/build-heltec-v4/bramble.bin`

> For other boards, switch the board name and build directory accordingly (for example `heltec-v3` -> `build-heltec-v3`, `tdeck-plus` -> `build-tdeck-plus`).

## 2) Host artifact over HTTP

```bash
cd ~/src/bramble/build-heltec-v4
python3 -m http.server 8088
```

Firmware URL used below:

```text
http://203.0.113.34:8088/bramble.bin
```

## 3) Trigger OTA

```bash
cd ~/src/bramble-cli
./bramble --transport ws://192.0.2.0/ws ota --url http://203.0.113.34:8088/bramble.bin
```

Expected: OTA RPC acknowledgment (`ok=true`), then device reboot/disconnect.

## 4) Verify reconnect + version/health

After reboot, verify node reconnects and responds:

```bash
cd ~/src/bramble-cli
./bramble --transport ws://192.0.2.0/ws ping
./bramble --transport ws://192.0.2.0/ws status --json
```

Optional post-checks:

```bash
./bramble --transport ws://192.0.2.0/ws monitor --topic gps --follow --since 2m
```

## 5) Rollback guidance

If the node becomes unhealthy after OTA:

1. Re-run OTA with a known-good `bramble.bin` URL:
   ```bash
   ./bramble --transport ws://192.0.2.0/ws ota --url http://203.0.113.34:8088/bramble.bin
   ```
2. If the node is unreachable over WiFi, recover via USB flash:
   ```bash
   cd ~/src/bramble
   bash scripts/flash.sh local heltec-v4 flash /dev/ttyACM0
   ```
3. Re-verify with `ping` and `status --json` after recovery.
