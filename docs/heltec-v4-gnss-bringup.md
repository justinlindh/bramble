# Heltec WiFi LoRa 32 V4 + L76K GNSS Bring-up Playbook

## Status (current implementation)

- Heltec V4 build target exists (`sdkconfig.defaults.heltec_v4`).
- Board profile reports `short_name = heltec_v4` and `BOARD_CAP_GPS`.
- GNSS UART pins are intentionally unset in `main/boards/heltec_v4.h` (`tx=-1`, `rx=-1`) pending final schematic/net validation.
- Result: firmware should boot and mesh normally on V4, but GPS init is currently expected to fail with `GPS pins not configured` until GNSS UART mapping is finalized.

This document is an operator checklist for reproducible V4 firmware bring-up and GNSS validation when hardware mapping is ready.

---

## 1) Build the Heltec V4 firmware

```bash
cd /home/user/src/bramble

# One-time per shell session (adjust IDF_PATH for your machine)
export IDF_PATH=~/src/esp-idf
source "$IDF_PATH/export.sh"

# Clean board config and build using V4 defaults
rm -f sdkconfig
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.heltec_v4" build
```

Expected:
- Build succeeds
- `sdkconfig` contains `CONFIG_BRAMBLE_BOARD_HELTEC_V4=y`

Quick check:

```bash
rg "CONFIG_BRAMBLE_BOARD_HELTEC_V4=y" sdkconfig
```

---

## 2) Flash V4 and open serial monitor

```bash
# Replace ttyUSB0 with your board port
idf.py -p /dev/ttyUSB0 flash
idf.py -p /dev/ttyUSB0 monitor
```

Expected boot evidence:
- Board boots without panic/reset loop
- Radio init completes
- Main loop starts

Current expected GNSS behavior (until UART pins are mapped):
- `GPS init failed or not available` (from `main/main.c`)
- `GPS pins not configured` (from `components/gps/gps.c`)

---

## 3) Mesh/radio validation (2-node minimum)

Use two Bramble nodes (V4 + any known-good Bramble node).

### Option A: full RPC e2e smoke

```bash
# Example transport mix; replace endpoints for your setup
python3 scripts/e2e-test.py ws://192.0.2.0/ws ws://192.0.2.0/ws
```

### Option B: basic status probe (manual)

```bash
echo '{"jsonrpc":"2.0","id":1,"method":"bramble.getStatus","params":{}}' \
  | websocat ws://192.0.2.0/ws
```

Acceptance:
- 2-node send/receive path works without firmware panic
- `bramble.getStatus` returns `"hardware":"heltec_v4"` on the V4 node

---

## 4) GNSS validation steps (when V4 GNSS UART mapping is enabled)

> Run this section only after `main/boards/heltec_v4.h` has non-negative GNSS `tx/rx` pin assignments validated against the official V4 docs.

### 4.1 Confirm GPS stack initializes

Monitor output should include lines similar to:
- `GPS UART initialized (TX=..., RX=..., baud=...)`
- `GPS initialized successfully (cold start may take 30-90s)`

### 4.2 Verify fix acquisition outdoors

- Place node outdoors with clear sky view.
- Allow up to 90s for first cold-start fix.
- Query position repeatedly:

```bash
echo '{"jsonrpc":"2.0","id":2,"method":"bramble.getGpsPosition","params":{}}' \
  | websocat ws://192.0.2.0/ws
```

Acceptance:
- `bramble.getGpsPosition` returns valid coordinates (non-zero lat/lon) once fix is acquired
- First-fix timing is within expected cold-start window for your deployment environment

---

## 5) Power notes

Current firmware behavior:
- GPS component remains active once initialized (no V4-specific GNSS duty-cycling policy documented yet).
- Expect increased draw when GNSS is enabled and tracking.

Operational guidance:
- For battery-sensitive field tests, record runtime with GNSS disabled vs enabled.
- Treat production power budget for V4+L76K as **pending hardware validation**.

---

## 6) Known failure signatures and likely causes

- `GPS pins not configured`
  - Cause: V4 GNSS UART mapping not yet set in board profile.
  - Action: finalize schematic-backed TX/RX mapping and rebuild.

- `GPS init failed or not available`
  - Cause: upstream GPS init failure (pins unset, UART config issue, or module absent).
  - Action: verify board config, wiring/module seating, and UART lines.

- `gps not supported on this board` (RPC error)
  - Cause: board capability does not include `BOARD_CAP_GPS` for selected target.
  - Action: confirm V4 target/defaults are active and firmware was rebuilt/flashed.

---

## 7) Field acceptance criteria checklist

Mark each run with date, location, and firmware commit.

- [ ] V4 cold boot reaches mesh loop without panic/reboot loop
- [ ] `bramble.getStatus` reports `"hardware":"heltec_v4"`
- [ ] 2-node message send/receive succeeds
- [ ] GNSS init succeeds on mapped-pin hardware build
- [ ] Outdoor fix acquired within expected window
- [ ] `bramble.getGpsPosition` returns valid coordinates after fix

### Validation state right now

- ✅ Build/flash/monitor procedure documented and reproducible.
- ✅ Mesh/radio acceptance criteria defined.
- ⚠️ GNSS hardware acceptance remains **pending** until V4 GNSS UART mapping + hardware run is completed.
