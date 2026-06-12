/*
 * Firmware binding for the TX gate: wires the host-testable gate core to
 * the real radio, timer, and RTOS primitives, and serializes all
 * transmissions behind one mutex (the radio is a single shared resource;
 * previously concurrent senders raced both the radio and the budget).
 */
#include "tx_gate.h"
#include "radio.h"
#include "radio_internal.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_random.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"

static tx_gate_t s_gate;
static SemaphoreHandle_t s_gate_mutex;

/*
 * WDT-safe lock (review MAJOR on PR #82). Worst-case mutex hold by a
 * sender inside the gate:
 *   LBT: 3 backoffs of (50+<50) + (100+<100) + (300+<300) ms  < ~900 ms
 *   radio_transmit_raw: standby/setup + TX-done wait bounded by the 4 s
 *   FreeRTOS notify timeout (3 s SX1262 hardware timeout inside it)      ~4 s
 *   total                                                                ~5 s
 * The mesh task's TWDT window is 5 s, so a competing sender blocking in
 * xSemaphoreTake(portMAX_DELAY) could starve its WDT and reset the node.
 * Waiters therefore poll with a 500 ms timed take and feed the TWDT on
 * every contention loop (a no-op error for tasks not subscribed). The
 * holder feeds its own WDT via ops.wdt_feed inside the gate and inside
 * radio_transmit_raw before the blocking wait.
 */
static void gate_lock(void) {
    while (xSemaphoreTake(s_gate_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        (void)esp_task_wdt_reset();
    }
}

static bool ops_channel_busy(void) { return radio_cad_check(); }

static int ops_transmit(const uint8_t* data, uint8_t len) { return radio_transmit_raw(data, len); }

static void ops_get_toa_params(uint8_t* sf, uint32_t* bw_hz, uint8_t* cr) {
    radio_config_t cfg;
    radio_get_config(&cfg);
    *sf = cfg.sf;
    *bw_hz = cfg.bw_hz;
    *cr = cfg.coding_rate;
}

static uint32_t ops_now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000ULL); }

static uint32_t ops_random_u32(void) { return esp_random(); }

static void ops_delay_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

static void ops_wdt_feed(void) {
    /* Harmless no-op error for tasks not subscribed to the TWDT. */
    (void)esp_task_wdt_reset();
}

void tx_gate_global_init(uint8_t max_duty_cycle_pct, bool duty_cycle_enforced) {
    if (!s_gate_mutex)
        s_gate_mutex = xSemaphoreCreateMutex();

    tx_gate_ops_t ops = {
        .channel_busy = ops_channel_busy,
        .transmit = ops_transmit,
        .get_toa_params = ops_get_toa_params,
        .now_ms = ops_now_ms,
        .random_u32 = ops_random_u32,
        .delay_ms = ops_delay_ms,
        .wdt_feed = ops_wdt_feed,
    };
    tx_gate_init(&s_gate, &ops, max_duty_cycle_pct, duty_cycle_enforced);
}

int tx_gate_send(const uint8_t* buf, uint8_t len, tx_kind_t kind) {
    gate_lock();
    int rc = tx_gate_transmit(&s_gate, buf, len, kind);
    xSemaphoreGive(s_gate_mutex);
    return rc;
}

bool tx_gate_check(uint8_t wire_len, tx_kind_t kind) {
    gate_lock();
    bool ok = tx_gate_can_transmit(&s_gate, wire_len, kind);
    xSemaphoreGive(s_gate_mutex);
    return ok;
}

void tx_gate_set_peer_count(uint8_t peer_count) {
    gate_lock();
    tx_gate_set_mesh_size(&s_gate, peer_count);
    xSemaphoreGive(s_gate_mutex);
}

void tx_gate_set_beacon_size(uint8_t beacon_wire_len) {
    gate_lock();
    tx_gate_set_beacon_profile(&s_gate, beacon_wire_len);
    xSemaphoreGive(s_gate_mutex);
}

uint32_t tx_gate_beacon_min_interval(void) {
    gate_lock();
    uint32_t interval = tx_gate_min_beacon_interval_ms(&s_gate);
    xSemaphoreGive(s_gate_mutex);
    return interval;
}

uint32_t tx_gate_remaining(uint8_t tier) {
    gate_lock();
    airtime_budget_refill(&s_gate.budget, ops_now_ms());
    uint32_t left = airtime_budget_remaining(&s_gate.budget, tier);
    xSemaphoreGive(s_gate_mutex);
    return left;
}

void tx_gate_snapshot(airtime_budget_t* out) {
    gate_lock();
    memcpy(out, &s_gate.budget, sizeof(*out));
    xSemaphoreGive(s_gate_mutex);
}
