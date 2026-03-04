# Delivery Event Persistence Implementation Plan

## Status (2026-02-24)
- ✅ Plan complete (Tasks 1–6 delivered).
- Reconnect replay sync is now wired end-to-end with firmware RPC support (`bramble.getDeliveryEvents`) and capability signaling (`supports_delivery_event_sync`).
- Protocol version bumped to `0.5.0` and reflected in firmware (`bramble`), SDK (`bramble-go`), and CLI surface (`bramble-cli`).

### Completion commits (high signal)
- `6ca2d42`: web IndexedDB delivery_events store + tests
- `8ada61f`: hydration + live merge + retention integration
- `ff504b9`: web reconnect replay sync glue + tests
- `4c9a126`: firmware replay API wiring + capability + protocol bump
- `8d2e189`: SDK replay types/client method + protocol range update
- `411950c`: CLI status output reflects replay-sync capability
- `a281e4f`: E2E validation runbook + retention tuning docs

> **For Agent:** REQUIRED SUB-SKILL: Use executing-plans to implement this plan task-by-task.

**Goal:** Make delivery metadata (route hops, delivery outcomes, broadcast recipient notes) durable across browser refresh, reconnect, and device reboot.

**Architecture:** Implement dual-layer persistence: IndexedDB persistence in web client for immediate refresh-safe UX, then bounded firmware ring-buffer persistence with sequence-based replay (`sinceEventSeq`) for reconnect/reboot catch-up.

**Tech Stack:** Bramble web app (TypeScript/IndexedDB), firmware C modules + RPC methods, existing sync patterns, dashboard task orchestration.

---

### Task 1: Web IndexedDB delivery_events store + repository

**Files:**
- Modify: `webapp/src/lib/*` (existing storage modules)
- Create: `webapp/src/lib/deliveryEventStore.ts`
- Test: `webapp/src/lib/deliveryEventStore.test.ts`

**Steps:**
1. Add failing tests for put/get/upsert and dedupe behavior keyed by `eventId`.
2. Create `delivery_events` object store schema with indexes (`messageId`, `conversationKey`, `ts`, `nodeAddr`).
3. Implement repository helpers (`upsertDeliveryEvent`, `listByMessage`, `pruneOldEvents`).
4. Run web tests/build and commit.

### Task 2: Web hydration + live merge integration

**Files:**
- Modify: `webapp/src/store/*` + message rendering components
- Test: relevant web state tests

**Steps:**
1. Write failing test: delivery notes survive refresh/hydration.
2. Merge persisted delivery events during cached message hydrate.
3. Persist incoming live delivery events before UI update.
4. Add retention prune on startup (configurable days) and verify.
5. Run tests/build and commit.

### Task 3: Firmware delivery-event ring buffer persistence

**Files:**
- Create/Modify: firmware delivery modules under `main/` or `components/`
- Test: unit tests under `test/`

**Steps:**
1. Define compact record struct with `eventSeq` and required fields.
2. Implement bounded ring persistence with overwrite-oldest behavior.
3. Add failing unit tests for wrap-around, ordering, and seq monotonicity.
4. Implement until tests pass.
5. Commit.

### Task 4: Firmware replay API (`sinceEventSeq`) + capability flag

**Files:**
- Modify: RPC methods + API schema docs (`api/openapi.yaml` if needed)
- Test: firmware RPC tests

**Steps:**
1. Add failing tests for incremental sync and idempotent replay windows.
2. Implement endpoint/method returning `{events, latestEventSeq}`.
3. Add capability surface (`supportsDeliveryEventSync`).
4. Verify backward-compatible fallback behavior.
5. Commit.

### Task 5: End-to-end sync glue (web reconnect flow)

**Files:**
- Modify: web transport/session sync layer
- Test: integration tests/sim where available

**Steps:**
1. On connect, request replay using stored last seq per node.
2. Upsert replayed events, then continue live stream.
3. Add dedupe guard to prevent duplicate UI entries on replay overlap.
4. Verify reconnect gap scenario.
5. Commit.

### Task 6: Validation, retention tuning, and docs/runbook

**Files:**
- Modify: docs/runbook and persistence strategy docs
- Test: manual validation checklist artifacts

**Steps:**
1. Execute six scenario checks: refresh, reboot, reconnect gap, broadcast fanout, dedupe, retention rollover.
2. Tune retention defaults (web days + firmware cap).
3. Document rollout + fallback behavior.
4. Commit final validation evidence.

---

## Task ordering / dependencies
1. Task 1
2. Task 2 (blocked by Task 1)
3. Task 3 (blocked by Task 2)
4. Task 4 (blocked by Task 3)
5. Task 5 (blocked by Task 4)
6. Task 6 (blocked by Task 5)

## Completion criteria
- Delivery notes survive refresh without server re-fetch.
- Reconnect/reboot restores delivery history via replay sync.
- Duplicate replay batches do not duplicate UI metadata.
- Broadcast recipient outcomes persist and render correctly.
- Retention behavior is bounded and documented.
