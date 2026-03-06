# Serial RPC Framing Hardening Implementation Plan

> **For Agent:** REQUIRED SUB-SKILL: Use executing-plans to implement this plan task-by-task.

**Goal:** Make USB/Serial connection initialization reliable in the webapp even when firmware logs and CLI output share the same serial stream.

**Architecture:** Keep the current JSON-RPC-over-serial contract, but harden the web transport parser so it can extract JSON objects from mixed log/text lines instead of requiring perfectly clean newline-delimited JSON. Add an explicit serial handshake/readiness phase before bulk init RPC calls, and reduce init burst fragility by staging calls after handshake success.

**Tech Stack:** TypeScript, React/Zustand, Vitest, Web Serial API, ESP-IDF firmware serial CLI/RPC stream.

---

### Task 1: Reproduce and lock the failure mode with tests

**Files:**
- Create: `~/src/bramble/webapp/src/transport/__tests__/SerialTransport.test.ts`
- Modify: `~/src/bramble/webapp/src/transport/SerialTransport.ts` (testability hooks only if required)

**Step 1: Write the failing test (mixed line noise before JSON response)**

Add a test that simulates incoming serial data chunks where a valid JSON-RPC response is preceded by log text on the same line, e.g.:

```text
I (10327) layo{"jsonrpc":"2.0","id":1,"result":{"ok":true}}
```

Assert: `sendRPC("bramble.getStatus")` resolves (it currently times out or is dropped).

**Step 2: Write the failing test (split JSON across multiple chunks)**

Simulate chunking like:
- chunk A: `{"jsonrpc":"2.0","id":2,`
- chunk B: `"result":{"ok":true}}\n`

Assert: parser reconstructs and resolves pending RPC.

**Step 3: Run tests to verify they fail**

Run:
```bash
cd ~/src/bramble/webapp
npm run test:unit -- src/transport/__tests__/SerialTransport.test.ts
```

Expected: FAIL showing parser/timeout behavior for noisy/mixed lines.

**Step 4: Commit failing tests**

```bash
cd ~/src/bramble
git add webapp/src/transport/__tests__/SerialTransport.test.ts
git commit -m "test(webapp): reproduce serial rpc parse failures on mixed log stream"
```

---

### Task 2: Harden SerialTransport parsing to extract JSON from mixed serial stream

**Files:**
- Modify: `~/src/bramble/webapp/src/transport/SerialTransport.ts`
- Test: `~/src/bramble/webapp/src/transport/__tests__/SerialTransport.test.ts`

**Step 1: Implement robust JSON extraction logic**

Replace the current line-only parser behavior with buffered extraction that can:
- ignore non-JSON prefix/suffix text,
- find JSON object boundaries (`{...}`),
- safely parse complete objects only,
- keep incomplete trailing fragments in buffer.

Implementation should remain minimal and deterministic (no regex-only parser hacks).

**Step 2: Ensure notification and RPC response routing is unchanged**

Preserve existing semantics:
- response objects with numeric `id` match pending requests,
- notifications route via `method` with no `id`.

**Step 3: Run focused tests**

```bash
cd ~/src/bramble/webapp
npm run test:unit -- src/transport/__tests__/SerialTransport.test.ts
```

Expected: PASS for new parser robustness tests.

**Step 4: Run full webapp unit tests**

```bash
cd ~/src/bramble/webapp
npm test
```

Expected: all tests pass.

**Step 5: Commit transport hardening**

```bash
cd ~/src/bramble
git add webapp/src/transport/SerialTransport.ts webapp/src/transport/__tests__/SerialTransport.test.ts
git commit -m "fix(webapp): parse json-rpc from mixed serial log stream"
```

---

### Task 3: Add explicit serial readiness handshake before init fan-out

**Files:**
- Modify: `~/src/bramble/webapp/src/store/actions.ts`
- Test: `~/src/bramble/webapp/src/store/__tests__/actions.serial-init.test.ts` (new)

**Step 1: Write failing test for init handshake policy**

Add a test around `connect("serial")` flow asserting:
- a readiness probe RPC (`bramble.ping` or `bramble.getStatus`) is attempted first,
- if readiness fails, bulk init RPC batch is not launched,
- if readiness succeeds, init batch proceeds.

**Step 2: Implement minimal readiness gate**

In `connect()` path for serial transport:
- perform short retry loop for readiness (e.g., 2–3 attempts with small delay),
- on failure, surface a clear transport error (`Serial connected but RPC not ready`),
- only then run `loadConfig` + init loaders.

**Step 3: Stage init calls to reduce burst sensitivity**

Keep `loadConfig` first, then run the rest in a controlled sequence (or small batches) to avoid all critical reads timing out simultaneously on noisy startup.

**Step 4: Run tests**

```bash
cd ~/src/bramble/webapp
npm run test:unit -- src/store/__tests__/actions.serial-init.test.ts
npm test
```

Expected: PASS.

**Step 5: Commit init hardening**

```bash
cd ~/src/bramble
git add webapp/src/store/actions.ts webapp/src/store/__tests__/actions.serial-init.test.ts
git commit -m "fix(webapp): gate serial init on rpc readiness and stage startup calls"
```

---

### Task 4: Improve diagnostics for future triage

**Files:**
- Modify: `~/src/bramble/webapp/src/transport/SerialTransport.ts`
- Modify: `~/src/bramble/webapp/src/store/actions.ts`

**Step 1: Add structured debug logs (dev-only)**

Add bounded, low-noise debug counters/logs for:
- parsed JSON objects count,
- dropped malformed fragments count,
- handshake attempts and failure reason.

Guard logs under dev mode flag to avoid production noise.

**Step 2: Add user-facing error mapping for serial handshake failure**

Map readiness failures to actionable text in `friendlyError()`.

**Step 3: Run tests + build**

```bash
cd ~/src/bramble/webapp
npm test
npm run build
```

Expected: PASS.

**Step 4: Commit diagnostics update**

```bash
cd ~/src/bramble
git add webapp/src/transport/SerialTransport.ts webapp/src/store/actions.ts
git commit -m "chore(webapp): add serial rpc diagnostic telemetry and clearer init errors"
```

---

### Task 5: Verification on real hardware

**Files:**
- Create: `~/src/bramble/docs/plans/evidence/2026-03-04-serial-rpc-framing-hardening.md`

**Step 1: Validate on at least 2 USB-connected nodes**

Run webapp against:
- one Espressif USB-JTAG/serial node (`/dev/ttyACM*`),
- one CP210x-backed node (`/dev/ttyUSB0`).

Verify:
- connection succeeds,
- no init timeout storm,
- `getConfig/getStatus/getAirtime/getNeighbors/getRoutes/getPeerLocations/getMessages` all return,
- chat send/receive works.

**Step 2: Capture evidence**

Record:
- console snippets before/after,
- test output,
- node addresses used,
- any residual intermittent failure rate.

**Step 3: Commit evidence doc**

```bash
cd ~/src/bramble
git add docs/plans/evidence/2026-03-04-serial-rpc-framing-hardening.md
git commit -m "docs: add hardware validation evidence for serial rpc hardening"
```

---

## Risks / Escalation Trigger

- If failures persist after parser + readiness fixes, escalate to firmware-side transport separation (dedicated RPC framing channel or log suppression during RPC window).
- Escalate immediately if parser robustness introduces regression in BLE/WebSocket transports.

## Done Criteria

- New serial parser tests pass.
- No startup timeout burst in typical connect flow.
- Real hardware verification shows stable init across at least two board types.
- Full `npm test` and `npm run build` pass.
