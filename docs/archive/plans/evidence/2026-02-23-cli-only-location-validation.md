# CLI-only Location + Monitor Validation Evidence

Date: 2026-02-23
Branch: feature/location-sharing-impl-2026-02-23
Firmware: 0.4.0-dev
Protocol: 0.4.0

## Nodes used
- T-Deck Plus (serial `/dev/ttyACM0`, addr `D4813079`)
- Heltec V3 local (serial `/dev/ttyUSB0`, addr `6CBF8FE3`)
- Heltec V3 GPU (ws `ws://192.168.1.64/ws`, addr `0D941BEA`)

## 1) CLI-only config parity (set/get)

Command:
```bash
cd ~/src/bramble-cli
./bin/bramble -p /dev/ttyACM0 location set-config --enabled --default-tier full --interval-s 30 --source gps \
  --contact-rules '[{"address":"6CBF8FE3","enabled":true,"tier":"full","interval_s":30},{"address":"0D941BEA","enabled":true,"tier":"full","interval_s":30}]'
./bin/bramble -p /dev/ttyACM0 location get-config --json
```

Observed:
- `Location config updated.`
- Canonical fields returned (`enabled`, `default_tier`, `interval_s`, `source`, `contact_rules`).

## 2) Enabled => location should transmit

Command pattern:
```bash
./bin/bramble -p /dev/ttyUSB0 location status --json
./bin/bramble -t ws://192.168.1.64/ws location status --json
```
(repeated polling over ~90s)

Observed:
- Both Heltecs report T-Deck peer entry with valid GPS position.
- `lastUpdatedMs` advanced over time on both receivers.
- Example entry shape:
```json
{
  "addr": "D4813079",
  "tier": "full",
  "position": { "lat": 35.9328..., "lon": -115.1267..., "alt": ..., "accuracy": ..., "heading": ... },
  "online": true,
  "lastUpdatedMs": <increasing>
}
```

## 3) Disabled => location should stop transmitting

Command:
```bash
./bin/bramble -p /dev/ttyACM0 location set-config --enabled=false --default-tier full --interval-s 30 --source gps \
  --contact-rules '[{"address":"6CBF8FE3","enabled":true,"tier":"full","interval_s":30},{"address":"0D941BEA","enabled":true,"tier":"full","interval_s":30}]'
./bin/bramble -p /dev/ttyACM0 location get-config --json
```

Then repeated receiver polling over ~75s:
```bash
./bin/bramble -p /dev/ttyUSB0 location status --json
./bin/bramble -t ws://192.168.1.64/ws location status --json
```

Observed:
- `enabled:false` confirmed on T-Deck config.
- Receiver `lastUpdatedMs` remained unchanged across poll window.
- No fresh location propagation observed.

## 4) Monitor topics (`wifi,gps,location`) emit protocol-native events

Command:
```bash
./bin/bramble -t ws://192.168.1.112/ws monitor --topic wifi,gps,location --json
```

Observed events include:
- `topic: "gps"` with payload event `fix_acquired` (lat/lon/alt/valid fields)
- `topic: "location"` with payload event `sent` and summary count

## 5) Additional stability note (T-Deck AP)

- T-Deck AP auth instability was resolved for current validation profile by WiFi-first default (BLE disabled by default in profile).
- BLE component build remains correctly gated by config; non-BLE profile path uses stub.

## Outcome

✅ CLI-only operational validation passed for:
- canonical location set/get config,
- send when enabled,
- stop when disabled,
- protocol-native monitor topic emission.
