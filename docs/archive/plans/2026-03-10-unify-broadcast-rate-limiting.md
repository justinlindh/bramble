# Unify Broadcast Rate Limiting Implementation Plan

> **For Agent:** REQUIRED SUB-SKILL: Use executing-plans to implement this plan task-by-task.

**Goal:** Replace the hardcoded public channel TX token bucket with the existing airtime budget system, so broadcast rate limiting adapts to mesh size automatically.

**Architecture:** Remove `public_channel_can_send` as the broadcast gate in `mesh_send_broadcast`. Replace it with `airtime_budget_can_transmit` using `AIRTIME_TIER_BROADCAST`. The airtime budget already scales by peer count (400% for ≤8 peers, down to 60% for 40+). Also fix the existing bug where `mesh_send_broadcast` never debits the airtime budget.

**Tech Stack:** C (ESP-IDF), Unity test framework, host-side tests (`test/`)

---

## Background

Two independent rate limiting systems exist for broadcast TX:

1. **`public_channel_can_send`** — dumb token bucket (3 burst, 30s refill). No mesh-size awareness. Located in `components/channel/public_channel.c`.
2. **`airtime_budget`** — sophisticated per-tier token bucket with mesh-size-adaptive profiles. Located in `components/airtime/airtime_budget.c`.

Currently `mesh_send_broadcast` only checks (1), never checks or debits (2). Beacons debit (2) but don't check (1). The systems are disconnected.

After this change:
- `mesh_send_broadcast` checks and debits `airtime_budget` (BROADCAST tier)
- `public_channel_can_send` is removed from the broadcast TX path (kept for RX rate limiting)
- The TX token bucket state and `public_channel_can_send` function remain available but are no longer called from `mesh_send_broadcast`
- Broadcast budget automatically adapts: micro mesh (≤8 peers) gets 72s/hr airtime, large mesh (40+) gets ~10.8s/hr

## Key Files

- `main/mesh_task.c` — `mesh_send_broadcast()` (line ~2707), airtime budget instance `s_airtime` (line 158)
- `components/channel/public_channel.c` — TX token bucket implementation
- `components/channel/include/public_channel.h` — TX rate limit constants
- `components/airtime/airtime_budget.c` — adaptive airtime budget
- `components/airtime/include/airtime_budget.h` — budget API
- `components/radio/radio_airtime.c` — `bramble_calculate_airtime_us()`
- `main/rpc_methods.c` — RPC handler returns `-2` as rate limit (line ~470)
- `test/test_public_channel.c` — existing TX rate limit test
- `test/test_airtime_budget.c` — existing airtime budget tests

---

### Task 1: Add test for airtime-budget-gated broadcast

**Files:**
- Create: `test/test_broadcast_airtime_gate.c`
- Modify: `test/CMakeLists.txt` (add new test to build)

**Step 1: Write the failing test**

Create `test/test_broadcast_airtime_gate.c`:

```c
#include "unity.h"
#include "airtime_budget.h"

/* We test the policy logic, not mesh_send_broadcast directly (that requires
   the full mesh_task runtime).  The contract is:
     - broadcast is allowed when airtime_budget_can_transmit(BROADCAST, est) is true
     - broadcast is denied when the budget is exhausted
     - budget adapts to mesh size                                              */

void setUp(void) {}
void tearDown(void) {}

/* A broadcast of ~50 bytes at SF10/125kHz ≈ 250ms airtime.
   Use a round number for test clarity. */
#define BROADCAST_AIRTIME_MS 250u

void test_broadcast_allowed_when_budget_available(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 0);
    /* Fresh budget should allow a broadcast */
    TEST_ASSERT_TRUE(airtime_budget_can_transmit(&ab, AIRTIME_TIER_BROADCAST, BROADCAST_AIRTIME_MS));
}

void test_broadcast_denied_when_budget_exhausted(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 0);
    /* Drain the entire broadcast budget */
    uint32_t remaining = airtime_budget_remaining(&ab, AIRTIME_TIER_BROADCAST);
    airtime_budget_debit(&ab, AIRTIME_TIER_BROADCAST, remaining);
    TEST_ASSERT_EQUAL_UINT32(0u, airtime_budget_remaining(&ab, AIRTIME_TIER_BROADCAST));
    /* Now a broadcast should be denied */
    TEST_ASSERT_FALSE(airtime_budget_can_transmit(&ab, AIRTIME_TIER_BROADCAST, BROADCAST_AIRTIME_MS));
}

void test_broadcast_budget_refills_over_time(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 0);
    uint32_t remaining = airtime_budget_remaining(&ab, AIRTIME_TIER_BROADCAST);
    airtime_budget_debit(&ab, AIRTIME_TIER_BROADCAST, remaining);
    /* After half the refill interval, some budget should be restored */
    airtime_budget_refill(&ab, AIRTIME_REFILL_INTERVAL_MS / 2u);
    TEST_ASSERT_TRUE(airtime_budget_remaining(&ab, AIRTIME_TIER_BROADCAST) > 0u);
    TEST_ASSERT_TRUE(airtime_budget_can_transmit(&ab, AIRTIME_TIER_BROADCAST, BROADCAST_AIRTIME_MS));
}

void test_micro_mesh_gets_more_broadcast_budget(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 0);
    /* Default (0 peers) uses micro profile */
    uint32_t micro_max = ab.max_ms[AIRTIME_IDX_BROADCAST];

    airtime_budget_t ab2;
    airtime_budget_init(&ab2, 0);
    airtime_budget_set_mesh_size(&ab2, 50);
    uint32_t large_max = ab2.max_ms[AIRTIME_IDX_BROADCAST];

    /* Micro mesh should have significantly more broadcast budget */
    TEST_ASSERT_TRUE(micro_max > large_max * 4u);
}

void test_rapid_broadcasts_exhaust_budget_not_arbitrary_limit(void) {
    airtime_budget_t ab;
    airtime_budget_init(&ab, 0);
    /* Micro mesh: budget is 400% of 18000ms = 72000ms.
       At 250ms per broadcast, we should get 72000/250 = 288 broadcasts
       before exhaustion — far more than the old 3-burst limit. */
    int count = 0;
    while (airtime_budget_can_transmit(&ab, AIRTIME_TIER_BROADCAST, BROADCAST_AIRTIME_MS)) {
        airtime_budget_debit(&ab, AIRTIME_TIER_BROADCAST, BROADCAST_AIRTIME_MS);
        count++;
        if (count > 500) break; /* safety valve */
    }
    /* Should allow many more than the old 3-burst limit */
    TEST_ASSERT_TRUE(count > 100);
    /* But should eventually exhaust */
    TEST_ASSERT_FALSE(airtime_budget_can_transmit(&ab, AIRTIME_TIER_BROADCAST, BROADCAST_AIRTIME_MS));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_broadcast_allowed_when_budget_available);
    RUN_TEST(test_broadcast_denied_when_budget_exhausted);
    RUN_TEST(test_broadcast_budget_refills_over_time);
    RUN_TEST(test_micro_mesh_gets_more_broadcast_budget);
    RUN_TEST(test_rapid_broadcasts_exhaust_budget_not_arbitrary_limit);
    return UNITY_END();
}
```

**Step 2: Add to CMakeLists.txt**

In `test/CMakeLists.txt`, add the new test executable following the existing pattern for `test_airtime_budget`:

```cmake
add_executable(test_broadcast_airtime_gate test_broadcast_airtime_gate.c)
target_include_directories(test_broadcast_airtime_gate PRIVATE
    ${CMAKE_SOURCE_DIR}/components/airtime/include
    ${CMAKE_SOURCE_DIR}/components/airtime
    ${UNITY_DIR}/src)
target_sources(test_broadcast_airtime_gate PRIVATE
    ${UNITY_DIR}/src/unity.c
    ${CMAKE_SOURCE_DIR}/components/airtime/airtime_budget.c)
add_test(NAME test_broadcast_airtime_gate COMMAND test_broadcast_airtime_gate)
```

**Step 3: Build and run to verify tests pass**

```bash
cd ~/src/bramble && bash scripts/bramble-test.sh test_broadcast_airtime_gate
```

Expected: All 5 tests PASS (these test the airtime budget API directly, which already works).

**Step 4: Commit**

```bash
git add test/test_broadcast_airtime_gate.c test/CMakeLists.txt
git commit -m "test: add broadcast airtime gate tests for rate limit unification"
```

---

### Task 2: Expose airtime budget to mesh_send_broadcast

Currently `s_airtime` is a file-scoped static in `mesh_task.c` and `mesh_send_broadcast` is also in `mesh_task.c`, so it already has access. The change is straightforward.

**Files:**
- Modify: `main/mesh_task.c` — `mesh_send_broadcast()` function (~line 2707)

**Step 1: Calculate broadcast airtime estimate**

The function needs to estimate airtime for the packet. Other paths in mesh_task.c use `30 + len * 4` as a rough ms estimate. We'll use the same pattern but import `bramble_calculate_airtime_us` for accuracy.

Check if `radio_airtime.c` is already linked in the main build:

```bash
grep -r "radio_airtime" main/CMakeLists.txt components/radio/CMakeLists.txt
```

**Step 2: Replace `public_channel_can_send` with airtime budget check**

In `main/mesh_task.c`, function `mesh_send_broadcast` (~line 2714), replace:

```c
    if (!public_channel_can_send(now_ms())) {
        ESP_LOGW(TAG, "Rate limited on public channel");
        return -2;
    }
```

With:

```c
    /* Estimate airtime for rate-limit check */
    uint32_t airtime_est = 30u + (uint32_t)(len * 4u);
    uint32_t t_now = now_ms();
    airtime_budget_set_mesh_size(&s_airtime, (uint8_t)neighbor_count(&s_neighbors));
    airtime_budget_refill(&s_airtime, t_now);
    if (!airtime_budget_can_transmit(&s_airtime, AIRTIME_TIER_BROADCAST, airtime_est)) {
        ESP_LOGW(TAG, "Broadcast rate limited by airtime budget (remaining=%"PRIu32"ms, need=%"PRIu32"ms)",
                 airtime_budget_remaining(&s_airtime, AIRTIME_TIER_BROADCAST), airtime_est);
        return -2;
    }
```

**Step 3: Add airtime debit after successful transmission**

After the successful `send_data_packet` call in both the fragmented and non-fragmented paths, add airtime debit. 

For the **non-fragmented path** (after `if (pkt_id != 0)` block, ~line 2810):

```c
        airtime_budget_debit(&s_airtime, AIRTIME_TIER_BROADCAST, airtime_est);
```

For the **fragmented path**, the airtime debit should happen per-fragment in the send loop. Add after each successful `send_data_packet` call inside the fragment loop:

```c
            uint32_t frag_airtime = 30u + (uint32_t)(frags[i].len * 4u);
            airtime_budget_debit(&s_airtime, AIRTIME_TIER_BROADCAST, frag_airtime);
```

**Step 4: Add include if needed**

Verify `airtime_budget.h` is already included in `mesh_task.c`:

```bash
grep "airtime_budget.h" main/mesh_task.c
```

(It should be — the file already uses `airtime_budget_*` functions.)

**Step 5: Build firmware**

```bash
cd ~/src/bramble && bash scripts/bramble-build.sh heltec-v3
```

Expected: Clean build, no warnings.

**Step 6: Run all tests**

```bash
cd ~/src/bramble && bash scripts/bramble-test.sh
```

Expected: All tests pass including the new `test_broadcast_airtime_gate`.

**Step 7: Commit**

```bash
git add main/mesh_task.c
git commit -m "feat: replace public_channel TX rate limit with airtime budget for broadcasts

mesh_send_broadcast now uses the airtime_budget system (BROADCAST tier)
instead of the hardcoded public_channel token bucket (3 burst / 30s refill).

This means broadcast rate limiting automatically adapts to mesh size:
- Micro mesh (≤8 peers): 72s/hr airtime (400% of baseline)
- Small mesh (9-15): 45s/hr (250%)
- Baseline (16-40): 18s/hr (100%)
- Large mesh (40+): 10.8s/hr (60%)

Also fixes a bug where mesh_send_broadcast never debited the airtime
budget, so broadcast airtime was untracked."
```

---

### Task 3: Update public_channel_can_send test expectations

The existing `test_public_channel_rate_limit` test verifies the old 3-burst behavior. This test should still pass (we didn't change `public_channel_can_send` itself), but we should add a comment noting it's now only used for non-broadcast paths.

**Files:**
- Modify: `test/test_public_channel.c`

**Step 1: Add comment to existing test**

Above `test_public_channel_rate_limit`, add:

```c
/* NOTE: public_channel_can_send is no longer used for broadcast TX gating.
   Broadcasts now go through airtime_budget (AIRTIME_TIER_BROADCAST).
   This test validates the token bucket still works for any future callers. */
```

**Step 2: Run tests**

```bash
cd ~/src/bramble && bash scripts/bramble-test.sh test_public_channel
```

Expected: All 4 tests PASS.

**Step 3: Commit**

```bash
git add test/test_public_channel.c
git commit -m "docs: annotate public_channel TX rate limit test re: broadcast unification"
```

---

### Task 4: Verify with mesh-test on hardware

This task is manual / interactive — not for subagent execution.

**Step 1: Flash all connected nodes with the new firmware**

```bash
cd ~/src/bramble
bash scripts/bramble-build.sh heltec-v3
bash scripts/bramble-build.sh heltec-v4
bash scripts/bramble-build.sh tdeck-plus
# Flash each device
```

**Step 2: Run mesh-test with same parameters as before**

```bash
cd ~/src/bramble-cli && ./bramble mesh-test --port /dev/ttyUSB0 --count 10 --spacing 10 --verbose
```

**Expected improvement:** Delivery rate should be significantly higher than 67%. The old 3-burst/30s-refill limit is gone. With a 4-peer micro mesh, the broadcast airtime budget is 72,000ms — enough for hundreds of broadcasts before exhaustion.

**Step 3: Run a stress test**

```bash
cd ~/src/bramble-cli && ./bramble mesh-test --port /dev/ttyUSB0 --count 30 --spacing 5 --verbose
```

This would have been impossible under the old rate limiter. With the airtime budget, it should work as long as total airtime stays within budget.

---

## Summary

| Before | After |
|--------|-------|
| 3 burst / 30s refill (hardcoded) | Airtime budget: 72s/hr for micro mesh, scales down with size |
| No mesh-size awareness | Automatically adapts via peer count profiles |
| Broadcast airtime untracked | Every broadcast debits AIRTIME_TIER_BROADCAST |
| ~10 broadcasts/5min max | ~288 broadcasts before budget exhaustion (micro mesh) |
| Rate limit errors at 10s spacing | 10s spacing well within budget |

## Future Work (not in scope)

- **Congestion sensing:** Use SX1262 CAD or RX packet rate to dynamically adjust airtime budget profiles based on observed channel utilization
- **Remove `public_channel_can_send` entirely** if no other callers remain after audit
- **Expose airtime budget remaining via RPC** so CLI can show budget status and mesh-test can adapt spacing
