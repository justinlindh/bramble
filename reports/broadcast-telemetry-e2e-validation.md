# Broadcast Delivery Telemetry E2E Validation (Task 10)

Date: 2026-02-22 (PST)
Branch: `feature/broadcast-delivery-telemetry-2026-02-22`
Repo: `/home/user/src/bramble`

## Scope
Validate firmware-to-UX broadcast delivery telemetry path:
1. `bramble.sendBroadcast` returns `broadcast_id`
2. `bramble.onBroadcastDelivery` telemetry events are observed
3. Webapp recipient delivery panel entries are rendered

## Artifacts
- Validation script: `scripts/validate-broadcast-telemetry.sh`
- Baseline run inputs:
  - `tmp-e2e/e2e-required/send-before-flash.json` (missing, expected baseline failure)
  - `tmp-e2e/e2e-required/run.log`
  - `tmp-e2e/e2e-required/01-loaded.png`
- Post-flash run inputs:
  - `tmp-e2e/e2e-required/send-after-flash.json`
  - `tmp-e2e/e2e-required/telemetry-after-flash.log`
  - `tmp-e2e/broadcast-delivery-ui/expanded-panel.png`

## 1) Baseline (before flash) — expected failure

Command:

```bash
scripts/validate-broadcast-telemetry.sh \
  --send-output tmp-e2e/e2e-required/send-before-flash.json \
  --telemetry-log tmp-e2e/e2e-required/run.log \
  --webapp-evidence tmp-e2e/e2e-required/01-loaded.png
```

Output:

```text
check_sendBroadcast_returns_broadcast_id=FAIL
check_delivery_telemetry_observed=FAIL
check_webapp_recipient_delivery_panel=PASS
```

Exit code: `1`

## 2) Firmware flash

Command:

```bash
bash scripts/flash.sh local heltec-v3 flash /dev/ttyUSB0
```

Result: **SUCCESS**

Evidence snippet:

```text
Chip is ESP32-S3 (QFN56) (revision v0.2)
...
Wrote 1334080 bytes (829405 compressed) at 0x00010000
...
Done
==> Done!
```

## 3) Post-flash real validation scenario

### 3.1 Capture `sendBroadcast` response + telemetry stream

Command executed (Python serial RPC capture on `/dev/ttyUSB0` and `/dev/ttyACM0`) generated:
- `tmp-e2e/e2e-required/send-after-flash.json`
- `tmp-e2e/e2e-required/telemetry-after-flash.log`

`send-after-flash.json` snippet:

```json
{
  "jsonrpc": "2.0",
  "result": {
    "broadcast_id": "22090521",
    "status": "sent",
    "broadcast": true,
    "channel": -1,
    "fragmented": false,
    "max_bytes": 616,
    "actual_bytes": 22
  },
  "id": 301
}
```

`telemetry-after-flash.log` snippet:

```text
{"jsonrpc":"2.0","method":"bramble.onNeighborChange"}
...
{"jsonrpc":"2.0","method":"bramble.onNeighborChange"}
```

No `bramble.onBroadcastDelivery` event observed in captured window.

### 3.2 Run validation script after flash

Command:

```bash
scripts/validate-broadcast-telemetry.sh \
  --send-output tmp-e2e/e2e-required/send-after-flash.json \
  --telemetry-log tmp-e2e/e2e-required/telemetry-after-flash.log \
  --webapp-evidence tmp-e2e/broadcast-delivery-ui/expanded-panel.png
```

Output:

```text
check_sendBroadcast_returns_broadcast_id=PASS
check_delivery_telemetry_observed=FAIL
check_webapp_recipient_delivery_panel=PASS
```

Exit code: `1`

## Pass/Fail Matrix

| Check | Baseline (pre-flash) | Post-flash |
|---|---:|---:|
| `sendBroadcast` returns `broadcast_id` | FAIL | PASS |
| broadcast delivery telemetry observed (`bramble.onBroadcastDelivery`) | FAIL | FAIL |
| webapp recipient delivery panel entries evidence | PASS | PASS |

## Blocker

`bramble.onBroadcastDelivery` notifications were not emitted during live post-flash capture on the tested serial setup, so full end-to-end telemetry confirmation remains blocked.

## Runnable follow-up procedure

When telemetry emission is available, rerun:

```bash
# 1) Flash latest firmware (if needed)
bash scripts/flash.sh local heltec-v3 flash /dev/ttyUSB0

# 2) Capture a fresh send response and telemetry logs on active sender/receiver serial ports
#    (same Python capture flow used in this report)

# 3) Re-run validator
scripts/validate-broadcast-telemetry.sh \
  --send-output <fresh-send-output.json> \
  --telemetry-log <fresh-telemetry.log> \
  --webapp-evidence <panel-screenshot-or-log>
```

Success criterion: all three checks return `PASS` with exit code `0`.
