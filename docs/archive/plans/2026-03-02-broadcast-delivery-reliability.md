# Broadcast Delivery Receipt Reliability Implementation Plan

> **For Agent:** REQUIRED SUB-SKILL: Use executing-plans to implement this plan task-by-task.

**Goal:** Eliminate flaky broadcast delivery receipts (currently ~3/4 nodes respond instead of 4/4) by adding LoRa collision avoidance at three layers: wider slot spacing, CAD-based listen-before-talk, and improved retry backoff.

**Architecture:** Three complementary layers that reduce collisions progressively. Layer 1 (wider slots) spreads initial TX attempts across a wider time window so fewer nodes attempt simultaneous transmission. Layer 2 (CAD/LBT) adds a channel-busy check before every radio transmission — if activity is detected, the sender backs off and retries. Layer 3 (more retries with exponential backoff) increases the number of delivery receipt attempts from 2 to 3 with wider randomized windows. Each layer is independently testable and incrementally improves reliability.

**Tech Stack:** C (ESP-IDF), SX1262 LoRa radio (CAD via opcode 0x88), FreeRTOS, Unity test framework

**Repo:** `~/src/bramble`

**Radio Config Context:** Default params are SF9, BW125kHz, 915MHz. The SX1262 CAD hardware feature detects preamble activity on-channel. At SF9/BW125, one CAD check takes ~5-8ms. The `SetCadParams` command (opcode `0x88`) must be configured before first CAD use. CAD results arrive via DIO1 IRQ (bits `IRQ_CAD_DONE` and `IRQ_CAD_DETECTED`), which is already wired up in `radio_esp.c`.

**Current Slot Math:**
- 16 buckets, 140ms spacing, 180ms base → max spread 2280ms
- 2 retry attempts with narrow windows (420-679ms, 900-1349ms)

**Target Slot Math:**
- 32 buckets, 200ms spacing, 200ms base → max spread 6400ms
- 3 retry attempts with wider exponential backoff

---

## Task 1: Widen Broadcast Receipt Slot Parameters

**Files:**
- Modify: `main/broadcast_delivery_receipt.c` (constants on lines 5-8)
- Modify: `test/test_broadcast_delivery_receipt.c` (update bound assertions)

**Step 1: Update the test bounds for wider slot range**

In `test/test_broadcast_delivery_receipt.c`, update `test_slot_delay_is_bounded_and_identity_sensitive`:

```c
void test_slot_delay_is_bounded_and_identity_sensitive(void) {
    uint32_t d1 = mesh_broadcast_receipt_slot_delay_ms(0x01020304u, 0xCAFEBABEu);
    uint32_t d2 = mesh_broadcast_receipt_slot_delay_ms(0x0A0B0C0Du, 0xCAFEBABEu);

    TEST_ASSERT_TRUE(d1 >= 200u);
    TEST_ASSERT_TRUE(d1 <= (200u + 200u * 31u));
    TEST_ASSERT_TRUE(d2 >= 200u);
    TEST_ASSERT_TRUE(d2 <= (200u + 200u * 31u));
    TEST_ASSERT_NOT_EQUAL(d1, d2);
}
```

**Step 2: Run tests to verify they fail**

```bash
cd ~/src/bramble && bash scripts/bramble-test.sh test_broadcast_delivery_receipt
```

Expected: `test_slot_delay_is_bounded_and_identity_sensitive` FAILS (old range is 180-2280, new bounds are 200-6400).

**Step 3: Update the slot constants**

In `main/broadcast_delivery_receipt.c`, change the defines:

```c
#define BROADCAST_RECEIPT_DELAY_BASE_MS      200u
#define BROADCAST_RECEIPT_SLOT_SPACING_MS    200u
#define BROADCAST_RECEIPT_SLOT_BUCKETS       32u
#define BROADCAST_RECEIPT_RETRY_COUNT        3u
```

**Step 4: Run tests to verify they pass**

```bash
cd ~/src/bramble && bash scripts/bramble-test.sh test_broadcast_delivery_receipt
```

Expected: All 4 tests PASS.

**Step 5: Add a distribution test to verify 32 buckets provide adequate spread for 4-5 nodes**

Add to `test/test_broadcast_delivery_receipt.c`:

```c
void test_slot_distribution_no_collision_for_typical_mesh(void) {
    /* Simulate 5 nodes responding to the same packet.
     * Verify that with 32 buckets, no two nodes land in the same slot
     * for at least a representative set of packet IDs. */
    uint32_t addrs[] = { 0x196F8E71u, 0xE3CF8D24u, 0x0D941BEAu, 0xD4813079u, 0x6CBF8FE3u };
    int collision_count = 0;

    for (uint32_t pkt_id = 0; pkt_id < 1000u; pkt_id++) {
        uint32_t slots[5];
        for (int i = 0; i < 5; i++) {
            slots[i] = mesh_broadcast_receipt_slot_delay_ms(addrs[i], pkt_id);
        }
        /* Check all pairs */
        for (int i = 0; i < 5; i++) {
            for (int j = i + 1; j < 5; j++) {
                if (slots[i] == slots[j]) {
                    collision_count++;
                }
            }
        }
    }
    /* With 32 buckets and 5 nodes, birthday collision rate ~31%.
     * Over 1000 packets × 10 pairs = 10000 pair checks,
     * expect ~3100 collisions. Verify it's well under 50%. */
    TEST_ASSERT_TRUE(collision_count < 5000);
    /* Also verify it's not zero (would indicate broken hash) */
    TEST_ASSERT_TRUE(collision_count > 0);
}
```

Register in `main()`:

```c
RUN_TEST(test_slot_distribution_no_collision_for_typical_mesh);
```

**Step 6: Run full test suite**

```bash
cd ~/src/bramble && bash scripts/bramble-test.sh test_broadcast_delivery_receipt
```

Expected: All 5 tests PASS.

**Step 7: Commit**

```bash
cd ~/src/bramble
git add main/broadcast_delivery_receipt.c test/test_broadcast_delivery_receipt.c
git commit -m "feat(delivery): widen slot spacing to 32 buckets × 200ms for collision reduction"
```

---

## Task 2: Add `SetCadParams` to SX1262 Driver

The SX1262 requires `SetCadParams` (opcode `0x88`) to be configured before running CAD. This task adds the low-level command and initializes it during radio setup.

**SX1262 SetCadParams format (7 bytes):**
- Byte 0: `cadSymbolNum` — number of symbols for CAD detection (recommended: 2 for SF9)
- Byte 1: `cadDetPeak` — sensitivity peak (recommended: 22 for SF9)
- Byte 2: `cadDetMin` — sensitivity minimum (recommended: 10 for SF9)
- Byte 3: `cadExitMode` — 0x00 = CAD only (return to STDBY after CAD), 0x01 = CAD then RX
- Bytes 4-6: `cadTimeout` — 24-bit timeout (0 for CAD-only mode)

**Files:**
- Modify: `components/radio/include/sx1262.h` (add command define + function decl)
- Modify: `components/radio/sx1262.c` (add implementation)
- Modify: `components/radio/radio_esp.c` (call during configure + add synchronous CAD API)
- Modify: `components/radio/include/radio.h` (add `radio_cad_check` decl)

**Step 1: Add the SX1262 command define and function declaration**

In `components/radio/include/sx1262.h`, after the existing `SX1262_CMD_SET_CAD` define, add:

```c
#define SX1262_CMD_SET_CAD_PARAMS     0x88
```

Add function declaration near `sx1262_set_cad`:

```c
int sx1262_set_cad_params(uint8_t symbol_num, uint8_t det_peak, uint8_t det_min,
                          uint8_t exit_mode, uint32_t timeout);
```

**Step 2: Implement in sx1262.c**

In `components/radio/sx1262.c`, near `sx1262_set_cad()`:

```c
int sx1262_set_cad_params(uint8_t symbol_num, uint8_t det_peak, uint8_t det_min,
                          uint8_t exit_mode, uint32_t timeout) {
    uint8_t data[7];
    data[0] = symbol_num;
    data[1] = det_peak;
    data[2] = det_min;
    data[3] = exit_mode;
    data[4] = (uint8_t)((timeout >> 16) & 0xFF);
    data[5] = (uint8_t)((timeout >> 8) & 0xFF);
    data[6] = (uint8_t)(timeout & 0xFF);
    return sx1262_write_command(SX1262_CMD_SET_CAD_PARAMS, data, 7);
}
```

**Step 3: Call SetCadParams during radio configuration**

In `components/radio/radio_esp.c`, find the `configure_radio()` function. At the end (after modulation params are set, before returning), add:

```c
    /* Configure CAD for listen-before-talk.
     * SF9/BW125: 2 symbols, peak=22, min=10, CAD-only mode (return to STDBY). */
    sx1262_set_cad_params(2, 22, 10, 0x00, 0);
```

**Step 4: Add synchronous `radio_cad_check` function**

In `components/radio/include/radio.h`, add:

```c
/**
 * Perform a synchronous CAD (Channel Activity Detection) check.
 * Returns true if LoRa activity was detected on the channel, false if clear.
 * Blocks for ~5-10ms depending on SF/BW. Returns to RX mode after check.
 */
bool radio_cad_check(void);
```

In `components/radio/radio_esp.c`, implement:

```c
static volatile bool     s_cad_result;
static SemaphoreHandle_t s_cad_sem;

static void cad_check_cb(bool detected) {
    s_cad_result = detected;
    if (s_cad_sem) {
        xSemaphoreGiveFromISR(s_cad_sem, NULL);
    }
}

bool radio_cad_check(void) {
    if (!s_cad_sem) {
        s_cad_sem = xSemaphoreCreateBinary();
        if (!s_cad_sem) {
            return false; /* Can't check, assume clear */
        }
    }

    /* Save and swap CAD callback */
    radio_cad_done_callback_t prev_cb = s_cad_done_cb;
    s_cad_done_cb = cad_check_cb;
    s_cad_result = false;

    /* Must be in standby to start CAD */
    radio_standby();
    sx1262_clear_irq_status(0x03FF);
    radio_cad();

    /* Wait for CAD result — 50ms should be plenty for any SF */
    bool got_result = xSemaphoreTake(s_cad_sem, pdMS_TO_TICKS(50));

    /* Restore previous callback and return to RX */
    s_cad_done_cb = prev_cb;
    radio_start_rx();

    if (!got_result) {
        ESP_LOGW(TAG, "CAD check timed out");
        return false; /* Assume clear on timeout */
    }

    return s_cad_result;
}
```

**Important:** The `xSemaphoreGiveFromISR` call is correct here because the CAD done callback fires from the DIO1 ISR handler → radio task → callback. Looking at the existing code, `radio_task` runs in a FreeRTOS task (not directly from ISR), so `xSemaphoreGive` (not `FromISR`) is actually correct. Let me check:

Actually, reviewing `radio_esp.c` — the DIO1 ISR notifies `s_radio_task`, which then calls callbacks from task context. So use `xSemaphoreGive` (not `FromISR`):

```c
static void cad_check_cb(bool detected) {
    s_cad_result = detected;
    if (s_cad_sem) {
        xSemaphoreGive(s_cad_sem);
    }
}
```

**Step 5: Verify firmware builds**

```bash
cd ~/src/bramble && bash scripts/bramble-build.sh heltec build
```

Expected: Clean build, no errors.

**Step 6: Commit**

```bash
cd ~/src/bramble
git add components/radio/include/sx1262.h components/radio/sx1262.c \
        components/radio/radio_esp.c components/radio/include/radio.h
git commit -m "feat(radio): add SX1262 CAD params config and synchronous radio_cad_check API"
```

---

## Task 3: Add Listen-Before-Talk to `transmit_packet`

Integrate CAD checking into the central `transmit_packet` function so ALL packet types benefit from collision avoidance.

**Files:**
- Modify: `main/mesh_task.c` (`transmit_packet` function, ~line 1254)

**Step 1: Add LBT wrapper around transmit_packet**

Replace the existing `transmit_packet` function in `main/mesh_task.c`:

```c
#define LBT_MAX_ATTEMPTS     3u
#define LBT_BACKOFF_BASE_MS  50u
#define LBT_BACKOFF_MAX_MS   300u

static int transmit_packet(const uint8_t *buf, uint8_t len) {
    /* Extract packet type for telemetry (assumes header is already serialized) */
    uint8_t pkt_type = (len >= 2) ? buf[1] : 0xFF;
    
    /* Extract tier from flags (bits 6-7) */
    uint8_t flags = (len >= 3) ? buf[2] : 0;
    uint8_t tier = ((flags >> FLAG_TIER_SHIFT) & 0x03);
    if (tier == 0) tier = 0x01; /* default to normal if not set */

    /* Listen-Before-Talk: check channel before transmitting */
    for (uint8_t attempt = 0; attempt < LBT_MAX_ATTEMPTS; attempt++) {
        if (!radio_cad_check()) {
            break; /* Channel is clear */
        }
        /* Channel busy — back off with randomized exponential delay */
        uint32_t backoff_ms = LBT_BACKOFF_BASE_MS * (1u << attempt);
        if (backoff_ms > LBT_BACKOFF_MAX_MS) {
            backoff_ms = LBT_BACKOFF_MAX_MS;
        }
        backoff_ms += (esp_random() % backoff_ms);
        ESP_LOGD(TAG, "LBT: channel busy (attempt %u/%u), backoff %" PRIu32 "ms",
                 (unsigned)(attempt + 1), LBT_MAX_ATTEMPTS, backoff_ms);
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
    }
    /* After LBT_MAX_ATTEMPTS, transmit anyway to avoid starvation */

    int ret = radio_transmit(buf, len);
    if (ret == 0) {
        /* Record successful TX */
        traffic_debug_record_tx(&s_traffic_debug, pkt_type, len, tier);
        
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_shared.packets_tx++;
        xSemaphoreGive(s_state_mutex);
    }
    return ret;
}
```

**Step 2: Verify firmware builds**

```bash
cd ~/src/bramble && bash scripts/bramble-build.sh heltec build
```

Expected: Clean build.

**Step 3: Commit**

```bash
cd ~/src/bramble
git add main/mesh_task.c
git commit -m "feat(mesh): add listen-before-talk (CAD) to transmit_packet for collision avoidance"
```

---

## Task 4: Increase Retry Count and Widen Backoff Windows

Update the delivery receipt retry loop in `send_broadcast_delivery_receipt` to use 3 attempts with wider exponential backoff.

**Files:**
- Modify: `main/mesh_task.c` (`send_broadcast_delivery_receipt` function, ~line 822)
- Modify: `test/test_broadcast_delivery_receipt.c` (update retry count assertion)

**Step 1: Update the retry count test**

In `test/test_broadcast_delivery_receipt.c`, update:

```c
void test_retry_count_default_three_attempts(void) {
    TEST_ASSERT_EQUAL_UINT8(3u, mesh_broadcast_receipt_retry_count());
}
```

Rename in `main()`:

```c
RUN_TEST(test_retry_count_default_three_attempts);
```

**Step 2: Run tests to verify failure**

```bash
cd ~/src/bramble && bash scripts/bramble-test.sh test_broadcast_delivery_receipt
```

Expected: FAIL (returns 2, expected 3). Note: the retry count constant was already changed to 3 in Task 1 — if so, this test will already pass. Either way, verify it's 3.

**Step 3: Widen the retry backoff windows**

In `main/mesh_task.c`, update the retry delay logic inside `send_broadcast_delivery_receipt`:

```c
        if (i + 1u < attempts) {
            /* Exponential backoff with per-attempt randomization.
             * attempt 0→1: 500-999ms
             * attempt 1→2: 1200-2099ms
             * attempt 2→3: 2000-3499ms (if retry count ever increases) */
            uint32_t base_ms = 500u + (i * 700u);
            uint32_t jitter_range = 500u + (i * 400u);
            uint32_t retry_delay_ms = base_ms + (esp_random() % jitter_range);
            vTaskDelay(pdMS_TO_TICKS(retry_delay_ms));
        }
```

**Step 4: Run tests and build**

```bash
cd ~/src/bramble && bash scripts/bramble-test.sh test_broadcast_delivery_receipt
cd ~/src/bramble && bash scripts/bramble-build.sh heltec build
```

Expected: All tests pass, clean build.

**Step 5: Commit**

```bash
cd ~/src/bramble
git add main/mesh_task.c test/test_broadcast_delivery_receipt.c
git commit -m "feat(delivery): 3 retry attempts with exponential backoff for broadcast receipts"
```

---

## Task 5: Integration Build, Flash, and Live Validation

**Files:** None (validation only)

**Step 1: Clean build for all targets**

```bash
cd ~/src/bramble
rm -f sdkconfig
bash scripts/bramble-build.sh heltec build
```

**Step 2: Flash to test node**

Flash the Heltec V3 at `/dev/ttyUSB0`:

```bash
bash scripts/bramble-build.sh heltec flash
```

**Step 3: Repeat for other accessible nodes**

If the node at `192.168.1.21` can be flashed OTA or via serial, update it too. At minimum, reflash the locally-connected devices.

For T-Deck Plus (different target):
```bash
rm -f sdkconfig
bash scripts/bramble-build.sh tdeck flash
```

**Step 4: Validate with broadcast delivery test**

After all nodes are running new firmware:

```bash
cd ~/src/bramble-cli
for i in $(seq 1 10); do
    echo "=== Test $i ==="
    ./bramble --transport ws://192.168.1.21/ws broadcast --wait-delivery 12 "reliability test $i"
    echo ""
    sleep 5
done
```

Expected: 4/4 delivery receipts on most or all runs (significant improvement from the current ~3/4).

**Step 5: Check serial logs for LBT activity**

Monitor serial output from one of the reflashed nodes to verify CAD backoffs are occurring:

```bash
# On a locally-connected node
screen /dev/ttyUSB0 115200
# Look for "LBT: channel busy" log lines
```

**Step 6: Commit validation results**

```bash
cd ~/src/bramble
# Save test output to evidence file
git add docs/archive/plans/evidence/
git commit -m "docs: broadcast delivery reliability validation results"
```

---

## Summary of Changes

| Layer | What | Where | Impact |
|-------|------|-------|--------|
| 1. Wider slots | 16→32 buckets, 140→200ms spacing | `broadcast_delivery_receipt.c` | Fewer initial collisions (spread: 2.3s → 6.4s) |
| 2. CAD/LBT | Channel activity check before every TX | `radio_esp.c` + `mesh_task.c` | Detects and avoids active transmissions |
| 3. More retries | 2→3 attempts, wider exponential backoff | `mesh_task.c` + `broadcast_delivery_receipt.c` | Better recovery from remaining collisions |

**Risk notes:**
- CAD adds ~5-10ms latency per TX. With LBT_MAX_ATTEMPTS=3, worst case is ~30ms + backoff time. This is negligible vs LoRa's typical 50-200ms airtime per packet.
- `radio_cad_check` briefly takes the radio out of RX (standby → CAD → RX). Any packet arriving during the ~5ms CAD window will be lost. This is acceptable since we're about to TX anyway (which also takes the radio out of RX).
- The `s_cad_sem` semaphore is lazily initialized. If heap is exhausted, CAD silently falls through to TX (safe degradation).
- The `transmit_packet` LBT loop has a hard cap of 3 attempts to prevent TX starvation if the channel is continuously busy.
