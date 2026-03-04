# Heltec V4 OTA Battery Fix Rollout Evidence (Task 4)

Date: 2026-02-23 (PST)
Target node: `192.168.1.179`
Transport: `ws://192.168.1.179/ws`

## 1) Baseline (pre-OTA)

Command:
```bash
cd /home/justin/src/bramble-cli
./bramble --transport ws://192.168.1.179/ws status --json
```
Result:
```json
{
  "address": "824C0ADE",
  "firmware_version": "0.4.0-dev",
  "protocol_version": "0.4.0",
  "hardware": "heltec_v4",
  "radio_ok": true,
  "peers": 3,
  "beacon_tx": 25,
  "beacon_rx": 75,
  "packets_tx": 70,
  "packets_rx": 240,
  "uptime_s": 1520
}
```

Battery baseline via direct RPC (`bramble.getStatus`) to capture `battery_mv/battery_pct` fields:
```bash
python3 - <<'PY'
import asyncio, json
import websockets

async def main():
    uri='ws://192.168.1.179/ws'
    async with websockets.connect(uri) as ws:
        req={"jsonrpc":"2.0","id":1,"method":"bramble.getStatus","params":{}}
        await ws.send(json.dumps(req))
        print(await ws.recv())

asyncio.run(main())
PY
```
Result excerpt:
```json
"battery_mv":0,
"battery_pct":0
```

## 2) Build Heltec V4 firmware artifact

Command:
```bash
cd /home/justin/src/bramble
source /home/justin/src/esp-idf/export.sh >/tmp/esp_export.log && \
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.heltec_v4" build
```
Result: build succeeded.

Key output:
- `bramble.bin binary size 0x121040 bytes`
- `Project build complete.`

## 3) Host firmware locally

Command:
```bash
cd /home/justin/src/bramble/build
python3 -m http.server 8088
```
Host IP discovered:
```bash
hostname -I | tr ' ' '\n' | rg '^192\.168\.'
# 192.168.6.34
```
Serving URL used for OTA: `http://192.168.6.34:8088/bramble.bin`

## 4) Trigger OTA via CLI

Command:
```bash
cd /home/justin/src/bramble-cli
./bramble --transport ws://192.168.1.179/ws ota --url http://192.168.6.34:8088/bramble.bin
```
Result:
```text
OTA update: ok=true
Note: OTA starting — device will reboot on success
Partition: app0
```

## 5) Post-OTA verification checks

### Ping
```bash
./bramble --transport ws://192.168.1.179/ws ping
```
Result:
```text
Pong from 824C0ADE (protocol: 0.4.0)
```

### Status
```bash
./bramble --transport ws://192.168.1.179/ws status --json
```
Result:
```json
{
  "address": "824C0ADE",
  "firmware_version": "0.4.0-dev",
  "protocol_version": "0.4.0",
  "hardware": "heltec_v4",
  "radio_ok": true,
  "peers": 3,
  "beacon_tx": 26,
  "beacon_rx": 77,
  "packets_tx": 72,
  "packets_rx": 247,
  "uptime_s": 1598
}
```

### GPS monitor
```bash
./bramble --transport ws://192.168.1.179/ws monitor --topic wifi,gps,mesh --since 10m --follow=false
```
Result:
```text
Monitoring node events... (Ctrl+C to stop)
[23:05:57] GPS event=fix_acquired valid=true
Stopping monitor.
```

### Reboot expectation check (OTA success criterion)
```bash
for i in 1 2 3 4 5; do ./bramble --transport ws://192.168.1.179/ws status --json | jq '.uptime_s'; sleep 5; done
```
Result:
```text
1622
1627
1633
1638
1643
```
Observation: uptime increased monotonically; no reboot observed after OTA trigger.

### Battery post-check
Direct RPC still reported battery pinned at 0:
```json
"battery_mv":0,
"battery_pct":0
```

## Conclusion

- Firmware build and OTA RPC trigger were executed successfully.
- Node remained reachable/healthy (`ping`, `status`, GPS fix event observed).
- OTA success condition (device reboot/apply) was **not observed** in this run.
- Battery behavior did **not** show improvement in observable telemetry; remained pinned at `0`.

This evidence indicates rollout attempt was performed, but effective OTA application could not be confirmed from node behavior in this execution.
