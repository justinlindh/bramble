# Network Reach Redesign Implementation Plan

> **For Agent:** REQUIRED SUB-SKILL: Use executing-plans to implement this plan task-by-task.

**Goal:** Redesign Network Reach to produce reliable, de-duplicated, self-excluding reachability results with lower airtime cost and predictable behavior.

**Architecture:** Move from ad-hoc probe/ack fanout to a deterministic probe session model: one probe request, bounded response window, one logical result per responder. Keep reliability by using controlled retry/backoff and strict receiver-side dedupe keyed by `(probe_id, responder_addr)`. Add explicit origin/self guards and structured completion signaling (`onProbeComplete`) so UI state is driven by firmware truth, not timers.

**Tech Stack:** ESP-IDF C firmware, RPC notifications, TypeScript webapp (Zustand store/actions), Vitest.

---

### Task 1: Define protocol invariants + test vectors

**Files:**
- Create: `docs/protocol/network-reach-v2.md`
- Test: `main/tests/probe_v2_spec_test.c` (or existing test harness equivalent)

**Step 1: Write the failing test/spec assertions**
- Add assertions for invariants:
  - Never report self as responder.
  - At most one final result row per responder per probe.
  - Duplicate ACKs update quality fields (best RSSI/SNR) instead of adding rows.
  - Probe session closes by explicit completion event.

**Step 2: Run test to verify it fails**
- Run: firmware unit/spec test command used in repo.
- Expected: FAIL for missing v2 semantics.

**Step 3: Write minimal spec document**
- Document packet semantics, state machine, and dedupe keys:
  - Probe key: `probe_id + origin_addr`
  - Response key: `probe_id + responder_addr`

**Step 4: Re-run test/spec checks**
- Expected: parser/spec tests PASS.

**Step 5: Commit**
- `git add docs/protocol/network-reach-v2.md main/tests/probe_v2_spec_test.c`
- `git commit -m "docs+test: define network reach v2 invariants"`

### Task 2: Firmware collector rewrite (single logical responder entry)

**Files:**
- Modify: `main/mesh_task.c` (probe send/ack receive paths)
- Modify: `main/mesh_task.h` (if structures are exported)
- Test: firmware test for probe aggregation behavior

**Step 1: Write failing aggregation tests**
- Repeated ACK from same responder should not create multiple entries.
- Higher-quality later ACK updates existing responder record.

**Step 2: Run test to verify fail**
- Expected: FAIL with current append-only behavior.

**Step 3: Implement minimal aggregator**
- Replace append-only response list with upsert by `responder_addr`.
- Keep counters for `raw_ack_count` and `unique_responder_count`.

**Step 4: Run tests**
- Expected: PASS.

**Step 5: Commit**
- `git add main/mesh_task.c main/mesh_task.h <tests>`
- `git commit -m "feat(probe): aggregate results by responder with upsert"`

### Task 3: Self/loop safeguards + forwarding policy tightening

**Files:**
- Modify: `main/mesh_task.c`
- Test: probe forwarding/self-loop tests

**Step 1: Write failing tests**
- Probe loopback should never generate self-ACK.
- Forwarded ACK should never be re-forwarded indefinitely.
- ACK forwarding only when useful (dest mismatch + hop_limit > 1 + not seen).

**Step 2: Run tests (fail expected)**

**Step 3: Implement minimal guards**
- Keep self-originated probe ignore.
- Add bounded forwarding with dedupe for forwarded ACK path.
- Enforce no self-reporting in notification emitter.

**Step 4: Run tests (pass expected)**

**Step 5: Commit**
- `git add main/mesh_task.c <tests>`
- `git commit -m "fix(probe): enforce self/loop safety and bounded forwarding"`

### Task 4: Reliability without airtime explosion (retry/jitter policy)

**Files:**
- Modify: `main/mesh_task.c` (ACK jitter/retry)
- Create: `docs/protocol/network-reach-airtime.md`
- Test: deterministic timing policy test (where possible)

**Step 1: Write failing policy tests**
- Ensure retry count and spacing remain within configured caps.
- Ensure jitter distribution avoids deterministic slot collisions.

**Step 2: Run tests (fail expected)**

**Step 3: Implement policy**
- Use configurable constants:
  - `PROBE_ACK_RETRIES` (default 2 or 3)
  - `PROBE_ACK_MIN_JITTER_MS / MAX_JITTER_MS`
- Prefer low default airtime, not brute-force reliability.
- Keep random jitter but bounded and documented.

**Step 4: Run tests**
- Expected: PASS.

**Step 5: Commit**
- `git add main/mesh_task.c docs/protocol/network-reach-airtime.md <tests>`
- `git commit -m "feat(probe): configurable low-airtime retry and jitter policy"`

### Task 5: Explicit probe completion signaling

**Files:**
- Modify: `main/mesh_task.c`
- Modify: `main/rpc_methods.c` (if needed)
- Test: firmware notify tests

**Step 1: Write failing tests**
- `bramble.onProbeComplete` emitted exactly once per probe session.
- Includes `probe_id`, `unique_count`, and end timestamp.

**Step 2: Run tests (fail expected)**

**Step 3: Implement completion notifier**
- End probe collection at firmware-defined timeout.
- Emit complete notification once.

**Step 4: Run tests (pass expected)**

**Step 5: Commit**
- `git add main/mesh_task.c main/rpc_methods.c <tests>`
- `git commit -m "feat(probe): emit explicit onProbeComplete event"`

### Task 6: Webapp result normalization (dedupe + self filter)

**Files:**
- Modify: `webapp/src/store/actions.ts`
- Modify: `webapp/src/pages/Stats/NetworkReach.tsx`
- Test: `webapp/test/store/probeResultsNormalization.test.ts`

**Step 1: Write failing tests**
- Duplicate notifications from same responder collapse into one row.
- Self address is excluded.
- Completion state uses firmware `onProbeComplete` when present.

**Step 2: Run tests (fail expected)**

**Step 3: Implement minimal normalization**
- Store responses in map keyed by responder address per probe.
- Render sorted stable array from map.
- Preserve best RSSI/SNR and latest latency.

**Step 4: Run tests (pass expected)**

**Step 5: Commit**
- `git add webapp/src/store/actions.ts webapp/src/pages/Stats/NetworkReach.tsx webapp/test/store/probeResultsNormalization.test.ts`
- `git commit -m "fix(webapp): normalize probe results and exclude self"`

### Task 7: Backward-compat behavior for mixed firmware fleet

**Files:**
- Modify: `webapp/src/store/actions.ts`
- Test: `webapp/test/store/probeCompatibility.test.ts`

**Step 1: Write failing tests**
- If `onProbeComplete` absent, fallback timer still works.
- Mixed old/new notification payloads parse correctly.

**Step 2: Run tests (fail expected)**

**Step 3: Implement compatibility shim**
- Feature-detect completion event.
- Keep fallback timer with bounded timeout.

**Step 4: Run tests (pass expected)**

**Step 5: Commit**
- `git add webapp/src/store/actions.ts webapp/test/store/probeCompatibility.test.ts`
- `git commit -m "feat(webapp): support mixed probe protocol versions"`

### Task 8: End-to-end verification on 3-node mesh

**Files:**
- Create: `docs/testing/network-reach-e2e-checklist.md`

**Step 1: Add e2e checklist**
- Test matrix:
  - strong links
  - weak asymmetric link
  - duplicate ACK stress
  - probe from each node
- Expected: no self rows, no duplicate rows, stable unique counts.

**Step 2: Run verification commands**
- Firmware probe loop scripts + webapp checks.
- Record pass/fail and sample logs.

**Step 3: Final test run**
- `npm test` in `webapp`
- firmware build/tests per repo standard

**Step 4: Commit docs/results**
- `git add docs/testing/network-reach-e2e-checklist.md`
- `git commit -m "test: add network reach e2e validation checklist"`

---

## Design choices to discuss before implementation

1. **Airtime budget target:** prioritize low airtime (2 retries) vs aggressive reliability (3+ retries).
2. **Completion authority:** firmware-only complete event vs hybrid fallback.
3. **Result semantics:** best-signal snapshot vs first-heard snapshot.
4. **Forwarding policy:** allow ACK forwarding always vs only under weak-link heuristics.
5. **Protocol versioning:** explicit `probe_protocol_version` field now vs implicit compatibility.
