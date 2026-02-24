# OTA Rollout Guide (Single Node)

Use this runbook to deploy a `bramble.bin` build to one WiFi-connected node without USB flashing.

## Prerequisites

- Node reachable over JSON-RPC WebSocket (example: `ws://192.0.2.0/ws`)
- Host machine IP reachable by the node (example: `203.0.113.34`)
- `bramble-cli` with `ota` command available

## 1) Build artifact

```bash
cd ~/src/bramble
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.heltec_v4" build
```

Expected artifact: `~/src/bramble/build/bramble.bin`

## 2) Host artifact over HTTP

```bash
cd ~/src/bramble/build
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
   idf.py -p <serial-port> flash monitor
   ```
3. Re-verify with `ping` and `status --json` after recovery.
