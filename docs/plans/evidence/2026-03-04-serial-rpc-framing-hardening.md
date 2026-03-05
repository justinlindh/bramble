# Evidence: Serial RPC framing hardening (Task 5)

Date: 2026-03-04 (PST)
Repo: `~/src/bramble`
Branch state: local working tree with Tasks 2-4 already applied

## Scope completed

Validated serial RPC initialization behavior on **2 locally USB-connected nodes**:
- `/dev/ttyACM1` (Espressif USB/JTAG-style ACM device)
- `/dev/ttyUSB0` (CP210x device)

Confirmed init-critical RPC methods respond on both nodes:
- `bramble.getConfig`
- `bramble.getStatus`
- `bramble.getAirtime`
- `bramble.getNeighbors`
- `bramble.getRoutes`
- `bramble.getPeerLocations`
- `bramble.getMessages`

Also captured test/build verification from current branch (`npm test`, `npm run build`).

---

## Hardware validation evidence

### Node A (ACM)
- Port: `/dev/ttyACM1`
- Address from `bramble.getIdentity`: `E3CF8D24`

RPC results (all first attempt):
- `bramble.getConfig` ✅ ~41ms (`channels=1`)
- `bramble.getStatus` ✅ ~29ms (`radio_ok=true`, `peers=4`, `uptime_s=2935`)
- `bramble.getAirtime` ✅ ~24ms (`remaining_ms=36000`)
- `bramble.getNeighbors` ✅ ~39ms (`neighbors=4`)
- `bramble.getRoutes` ✅ ~12ms (`routes=0`)
- `bramble.getPeerLocations` ✅ ~18ms (`peerLocations=0`)
- `bramble.getMessages` ✅ ~38ms (`messages=3`)

Observed mixed stream sample on same device:
- `bramble> {"jsonrpc": "2.0", "id": 2, "method": "bramble.getConfig"}`

### Node B (USB)
- Port: `/dev/ttyUSB0`
- Address from `bramble.getIdentity`: `6CBF8FE3`

RPC results (all first attempt):
- `bramble.getConfig` ✅ ~58ms (`channels=1`)
- `bramble.getStatus` ✅ ~42ms (`radio_ok=true`, `peers=4`, `uptime_s=2926`)
- `bramble.getAirtime` ✅ ~34ms (`remaining_ms=36000`)
- `bramble.getNeighbors` ✅ ~59ms (`neighbors=4`)
- `bramble.getRoutes` ✅ ~17ms (`routes=0`)
- `bramble.getPeerLocations` ✅ ~44ms (`peerLocations=1`)
- `bramble.getMessages` ✅ ~63ms (`messages=4`)

Observed mixed stream sample on same device:
- `bramble> {"jsonrpc": "2.0", "id": 2, "method": "bramble.getConfig"}`

---

## Before/after-style observations tied to current behavior

### Before hardening (problem mode from plan/tests)
- Serial stream commonly includes CLI prompt/log noise (`bramble> ` and firmware logs) adjacent to JSON.
- Prior line-oriented parsing behavior was fragile when JSON was not cleanly isolated at line start.

### After Tasks 2-4 (current branch behavior)
- Parser robustness evidence: unit tests now explicitly cover noisy-prefix and split-chunk JSON parsing and pass:
  - `SerialTransport` regression tests pass, including noisy-prefix and split-across-chunks cases.
- Init flow resilience evidence: serial readiness gate tests pass:
  - retries + handshake failure path works
  - successful handshake gates and then proceeds with init methods
- On real hardware (ACM + USB), all init-critical methods returned successfully on first attempt despite mixed prompt+JSON stream samples.

---

## Test/build verification (current branch)

Executed in `~/src/bramble/webapp`:

### `npm test`
- **PASS**
- Summary: `42 passed (42)` test files, `160 passed (160)` tests.
- Includes passing serial hardening tests:
  - `src/transport/__tests__/SerialTransport.test.ts`
  - `src/store/__tests__/actions.serial-init.test.ts`

### `npm run build`
- **FAIL** (known blocker)
- Fails during typecheck with:
  - `src/store/__tests__/actions.serial-init.test.ts(11,107): error TS2554: Expected 1 arguments, but got 3.`
- Build did not proceed beyond typecheck.

---

## Browser E2E honesty note

Full browser E2E (real Web Serial picker-driven connect path) was **not executed** here due automation/picker constraints in this validation pass. This evidence is therefore host-side + hardware-serial RPC level, plus unit/integration test coverage in webapp.

---

## Residual risk

1. Browser picker/UI-layer regressions could still exist despite host-side serial RPC stability.
2. Build/typecheck is currently blocked by TS error in `actions.serial-init.test.ts`, so branch is not build-clean yet.
3. Extremely noisy/fragmented serial streams beyond observed conditions may still expose edge cases; no sustained soak run was done in this task.
