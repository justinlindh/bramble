# Delivery Event Persistence E2E Validation (Task 6)

Last updated: 2026-02-24
Repo: `/home/user/src/bramble`

## Scope

Task 6 validation matrix from `docs/archive/plans/2026-02-24-delivery-event-persistence-implementation-plan.md`:
1. Refresh
2. Reboot
3. Reconnect gap
4. Broadcast fanout
5. Dedupe/idempotency
6. Retention rollover

Also tune retention defaults and document rollout/fallback behavior.

---

## Retention defaults (applied)

### Firmware ring cap
- **Changed default**: `DELIVERY_EVENT_RING_CAPACITY` from `1024` → **`2048`**
- File: `main/delivery_event_ring.h`

### Web TTL
- **Default remains**: **30 days**
- Source: `webapp/src/store/actions.ts`
  - `DEFAULT_DELIVERY_EVENT_RETENTION_DAYS = 30`

---

## Commands executed + output (real run)

### Web delivery persistence tests
```bash
cd /home/user/src/bramble/webapp
npm test -- --run test/store/deliveryEventStore.test.ts test/store/deliveryPersistenceIntegration.test.ts
```

Output (excerpt):
```text
✓ test/store/deliveryEventStore.test.ts (3 tests)
✓ test/store/deliveryPersistenceIntegration.test.ts (3 tests)

Test Files  2 passed (2)
Tests       6 passed (6)
```

### Firmware/unit delivery tests
```bash
cd /home/user/src/bramble/test/build
./test_delivery_event_ring && ./test_broadcast_delivery_receipt && ./test_mesh_broadcast_delivery
```

Output (excerpt):
```text
test_delivery_event_ring: 3 Tests 0 Failures 0 Ignored OK
test_broadcast_delivery_receipt: 4 Tests 0 Failures 0 Ignored OK
test_mesh_broadcast_delivery: 3 Tests 0 Failures 0 Ignored OK
```

### Replay capability/reconnect API presence check
```bash
rg -n "rpc_register\(\"bramble\.get(Version|DeliveryEvents)\"|supportsDeliveryEventSync|supports_delivery_event_sync" main/rpc_methods.c webapp/src/store/actions.ts
```

Output:
```text
main/rpc_methods.c:1735:    rpc_register("bramble.getVersion",   handle_get_version);
webapp/src/store/actions.ts:494:  let supportsDeliveryEventSync = false;
webapp/src/store/actions.ts:497:    supportsDeliveryEventSync = Boolean(
webapp/src/store/actions.ts:498:      version.supportsDeliveryEventSync ?? version.supports_delivery_event_sync,
webapp/src/store/actions.ts:503:  if (!supportsDeliveryEventSync) return;
```

Interpretation: web client has guarded replay logic, but firmware RPC registration in this tree does not expose `bramble.getDeliveryEvents` and `getVersion` does not advertise `supportsDeliveryEventSync`.

---

## Validation matrix results

| Scenario | Evidence source | Result | Notes |
|---|---|---|---|
| Refresh persistence | `deliveryPersistenceIntegration.test.ts` hydrate case | ✅ PASS | Cached message delivery metadata restored from IndexedDB after store init. |
| Reboot persistence | `test_delivery_event_ring` serialize/deserialize roundtrip | ✅ PASS (ring module) | Ring state survives serialize/deserialize and seq monotonicity continues. Full firmware reboot path wiring not covered by this unit alone. |
| Reconnect gap replay | API presence check + web sync guard | ⚠️ BLOCKED / NOT IMPLEMENTED IN THIS TREE | No registered `bramble.getDeliveryEvents` RPC and no capability in `getVersion`; web exits replay path when capability absent. |
| Broadcast fanout persistence | `deliveryPersistenceIntegration.test.ts` + firmware broadcast tests | ✅ PASS | Broadcast recipient delivery events are persisted/merged and firmware emits broadcast delivery notifications. |
| Dedupe/idempotency | `deliveryEventStore.test.ts` dedupe by `eventId` | ✅ PASS | Upsert behavior prevents duplicate event rows and updates in place. |
| Retention rollover | web prune test + ring wrap tests | ✅ PASS | Web prunes >30d events; ring overwrite-oldest behavior verified at capacity boundary. |

---

## Rollout + fallback behavior

- Web client is safe to run with/without replay-capable firmware:
  - It first checks `supportsDeliveryEventSync` from `bramble.getVersion`.
  - If absent/false, replay is skipped (no hard failure).
- Current tree behavior therefore supports:
  - Refresh-safe persistence via IndexedDB,
  - Local ring-buffer durability semantics in unit tests,
  - But **not** full reconnect gap catch-up over RPC until replay endpoint/capability are added.

---

## Task 6 completion status for this source tree

- ✅ Executed real validation runs against available implementation.
- ✅ Tuned retention default in code (firmware cap to 2048; web TTL already 30 days).
- ✅ Updated runbook with real command outputs and scenario results.
- ⚠️ Remaining gap: firmware replay API/capability (`bramble.getDeliveryEvents`, `supportsDeliveryEventSync`) not present in this tree, so reconnect-gap E2E remains blocked at integration layer.
