// Real nRF52840 watchdog backing esp_task_wdt.h (see that file for the
// portable API shared code calls) and the nRF-only init/arm lifecycle in
// bramble_wdt.h.
//
// Hardware model: nrfx_wdt exposes up to 8 independent reload-request
// channels (RR0-RR7) sharing one countdown. Every allocated channel must
// be fed within the window or the peripheral resets, regardless of which
// channel was missed; that maps directly onto ESP-IDF's task watchdog
// "every subscribed task must check in" semantics, one hardware channel
// per task instead of software subscriber bookkeeping. This build gives a
// channel to exactly 4 tasks (mesh, radio, the NimBLE link layer, the
// NimBLE host); see nrf/src/app_init.c and this file's identification
// scheme below for why not more.
//
// Two hard nRF52840 constraints shape this file:
//
//   - nrfx_wdt_channel_alloc() cannot be called once nrfx_wdt_enable() has
//     run (channel_alloc asserts driver state == INITIALIZED; enable moves
//     it to POWERED_ON, and there is no going back). bramble_wdt_arm() is
//     therefore called exactly once, from nrf/src/app_init.c, only after
//     every subscriber task is guaranteed to have already registered. That
//     guarantee comes from FreeRTOS's fixed-priority preemptive scheduler,
//     not a timing heuristic: mesh (prio 5), radio (5), the NimBLE link
//     layer (8) and host (6) all outrank the boot task (3) that calls
//     bramble_wdt_arm(), each calls esp_task_wdt_add() as the first thing
//     it does, and creating a strictly-higher-priority task preempts the
//     creator immediately in this port (configUSE_PREEMPTION 1). The boot
//     task therefore cannot reach the arm call until every one of those
//     tasks has run at least as far as its own wdt_add and then blocked,
//     which is exactly the registration this file depends on.
//     esp_task_wdt_add() below still refuses an add() that arrives after
//     arm rather than trusting that argument to hold forever, since a
//     future subscriber added without extending it would otherwise hit an
//     NRFX_ASSERT and reboot the board.
//   - The nRF52840's WDT has no TASKS_STOP register at all (confirmed in
//     the vendored SVD headers: NRF_WDT_HAS_STOP, which nrfx_wdt.h derives
//     from whether WDT_TASKS_STOP_TASKS_STOP_Msk is defined, resolves to 0
//     for this part; that symbol is simply absent from nrf52840.h). Once
//     armed, the watchdog runs until a full power-on or pin reset; nothing
//     in this file, or anywhere else in software, can turn it back off.
//     That is why bramble_wdt_arm() is deferred to steady state instead of
//     called from main(): see the comment at its call site.
//
// Identification by task NAME, not TaskHandle_t: the t1000e static-RAM gate
// (nrf/scripts/size_report.py) has essentially no slack (it was passing by
// 8 bytes before this file existed), so a per-task table cannot afford a
// 4-byte TaskHandle_t slot on top of the channel it maps to. Every task
// this build ever gives a channel to already has a fixed name assigned at
// xTaskCreate (main/mesh_task.c "mesh", nrf/src/radio_lr1110.c "radio",
// nrf/src/nimble_port_freertos.c "ll" and "ble"), and pcTaskGetName() reads
// it straight out of the TCB FreeRTOS already allocated, so matching on
// that name costs this file only the channel byte itself, not an identity
// field alongside it.
//
// A registered task that stops feeding its channel resets the device by
// design. The corollary is the one real correctness hazard in this file:
// esp_task_wdt_delete() cannot ask the hardware to forget a channel either
// (the same missing-stop constraint applies to nrfx_wdt_channels_free(),
// documented as callable only when the driver is not running). A task that
// legitimately exits after registering, such as mesh_task when
// radio_init() fails (main/mesh_task.c), would otherwise starve its own
// channel and reset the device forever: a graceful "no radio" degraded
// mode turned into a boot loop. s_deleted_mask tracks exactly this case. A
// deleted channel is kept fed forever, as a proxy, by every other live
// esp_task_wdt_reset() call, standing in for "no longer monitored". If
// every other subscriber ever stops too, the deleted channel stops being
// fed along with them, and the device resets, which is correct: at that
// point the whole system, not just the task that opted out, has actually
// hung.
#include "esp_task_wdt.h"
#include "bramble_wdt.h"

#include <nrfx_wdt.h>

#include <FreeRTOS.h>
#include <task.h>

#include <string.h>

#include "esp_log.h"

// 8-10s per the investigation this change implements: the same margin the
// ESP32 build proves over its worst legitimate blocking op, the 4s LoRa TX
// wait (main/mesh_task.c). Picked near the middle of that band.
#define BRAMBLE_WDT_RELOAD_MS 9000u

// One instance on this part (WDT0); NRFX_WDT0_ENABLED selects it in
// nrf/config/nrfx_config.h. const: lives in flash, not RAM.
static const nrfx_wdt_t s_wdt = NRFX_WDT_INSTANCE(0);

typedef enum {
    WDT_TASK_MESH = 0,
    WDT_TASK_RADIO,
    WDT_TASK_LL,
    WDT_TASK_HOST,
    WDT_TASK_COUNT,
} wdt_task_id_t;

#define WDT_CHANNEL_NONE 0xFFu

static uint8_t s_channel[WDT_TASK_COUNT]; // nrfx channel per task id, or WDT_CHANNEL_NONE

// One byte, not three: the t1000e static-RAM gate has no room to spare
// (see the file header). Bits 0-3 are the per-task deleted mask
// (esp_task_wdt_delete); bit 6 is "nrfx_wdt_init succeeded"; bit 7 is
// "nrfx_wdt_enable has run, countdown is live".
static uint8_t s_state;
#define WDT_STATE_DRIVER_READY (1u << 6)
#define WDT_STATE_ARMED (1u << 7)

static int task_id_for(TaskHandle_t task) {
    const char* name = pcTaskGetName(task);
    if (!name) {
        return -1;
    }
    if (strcmp(name, "mesh") == 0) {
        return WDT_TASK_MESH;
    }
    if (strcmp(name, "radio") == 0) {
        return WDT_TASK_RADIO;
    }
    if (strcmp(name, "ll") == 0) {
        return WDT_TASK_LL;
    }
    if (strcmp(name, "ble") == 0) {
        return WDT_TASK_HOST;
    }
    return -1;
}

void bramble_wdt_init(void) {
    if (s_state & WDT_STATE_DRIVER_READY) {
        return;
    }
    for (int i = 0; i < WDT_TASK_COUNT; i++) {
        s_channel[i] = WDT_CHANNEL_NONE;
    }
    nrfx_wdt_config_t cfg = {
        .behaviour = NRF_WDT_BEHAVIOUR_RUN_SLEEP_MASK,
        .reload_value = BRAMBLE_WDT_RELOAD_MS,
    };
    // NRFX_WDT_CONFIG_NO_IRQ is set in nrf/config/nrfx_config.h: nothing
    // useful can run in the two LFCLK ticks between the timeout event and
    // the reset it causes, and boot_trace.c already recovers the evidence
    // after the fact by decoding RESETREAS on the next boot, so no
    // interrupt handler is needed.
    nrfx_err_t rc = nrfx_wdt_init(&s_wdt, &cfg, NULL, NULL);
    if (rc != NRFX_SUCCESS) {
        ESP_LOGE("wdt", "nrfx_wdt_init failed: 0x%x", (unsigned)rc);
        return;
    }
    s_state |= WDT_STATE_DRIVER_READY;
}

esp_err_t esp_task_wdt_add(TaskHandle_t task_handle) {
    if (!(s_state & WDT_STATE_DRIVER_READY)) {
        // Boot has not reached bramble_wdt_init() yet (should not happen;
        // it runs before any task exists), or WDT init itself failed.
        // Behave like the no-op shim this file replaces rather than touch
        // uninitialized nrfx state.
        return ESP_OK;
    }
    TaskHandle_t task = task_handle ? task_handle : xTaskGetCurrentTaskHandle();
    int id = task_id_for(task);
    if (id < 0) {
        // Not one of this build's known subscribers (see file header):
        // give it nothing to feed rather than fail a caller that has no
        // channel available for it. Matches the old shim's blanket ESP_OK.
        return ESP_OK;
    }
    if (s_channel[id] != WDT_CHANNEL_NONE) {
        return ESP_OK; // already subscribed
    }
    if (s_state & WDT_STATE_ARMED) {
        // See the file header: every task this build gives a channel to
        // is guaranteed to have already called this before bramble_wdt_arm()
        // ran. Reaching here means that guarantee did not hold for this
        // caller; nrfx_wdt_channel_alloc() cannot be called anymore, so
        // refuse instead of asserting the board into a DFU reboot.
        ESP_LOGW("wdt", "WDT already armed, cannot add task id %d", id);
        return ESP_ERR_INVALID_STATE;
    }
    nrfx_wdt_channel_id ch;
    nrfx_err_t rc = nrfx_wdt_channel_alloc(&s_wdt, &ch);
    if (rc != NRFX_SUCCESS) {
        ESP_LOGE("wdt", "nrfx_wdt_channel_alloc failed: 0x%x", (unsigned)rc);
        return ESP_ERR_NO_MEM;
    }
    s_channel[id] = (uint8_t)ch;
    return ESP_OK;
}

esp_err_t esp_task_wdt_reset(void) {
    if (!(s_state & WDT_STATE_ARMED)) {
        return ESP_OK;
    }
    int self = task_id_for(xTaskGetCurrentTaskHandle());
    esp_err_t result = ESP_ERR_NOT_FOUND;
    for (int i = 0; i < WDT_TASK_COUNT; i++) {
        if (s_channel[i] == WDT_CHANNEL_NONE) {
            continue;
        }
        if (s_state & (1u << i)) {
            // Opted out or orphaned: keep it fed by proxy. See file header.
            nrfx_wdt_channel_feed(&s_wdt, (nrfx_wdt_channel_id)s_channel[i]);
            continue;
        }
        if (i == self) {
            nrfx_wdt_channel_feed(&s_wdt, (nrfx_wdt_channel_id)s_channel[i]);
            result = ESP_OK;
        }
    }
    return result;
}

esp_err_t esp_task_wdt_delete(TaskHandle_t task_handle) {
    TaskHandle_t task = task_handle ? task_handle : xTaskGetCurrentTaskHandle();
    int id = task_id_for(task);
    if (id < 0 || s_channel[id] == WDT_CHANNEL_NONE) {
        return ESP_OK; // never subscribed: matches the old shim's blanket ESP_OK
    }
    s_state |= (uint8_t)(1u << id);
    if (s_state & WDT_STATE_ARMED) {
        // Immediate feed: nothing else touches this channel until some
        // other still-live task's esp_task_wdt_reset() runs.
        nrfx_wdt_channel_feed(&s_wdt, (nrfx_wdt_channel_id)s_channel[id]);
    }
    return ESP_OK;
}

void bramble_wdt_arm(void) {
    if ((s_state & WDT_STATE_ARMED) || !(s_state & WDT_STATE_DRIVER_READY)) {
        return;
    }
    bool any = false;
    for (int i = 0; i < WDT_TASK_COUNT; i++) {
        if (s_channel[i] != WDT_CHANNEL_NONE) {
            any = true;
            break;
        }
    }
    if (!any) {
        ESP_LOGW("wdt", "arming WDT with zero registered channels");
    }
    nrfx_wdt_enable(&s_wdt);
    s_state |= WDT_STATE_ARMED;
    for (int i = 0; i < WDT_TASK_COUNT; i++) {
        // First feed for anything already deleted before arm ran (in
        // principle: radio_init() failing before boot even finished).
        // Everything else gets its first feed from its own owning task's
        // ordinary post-boot cadence, which by construction is already
        // running by this point.
        if (s_channel[i] != WDT_CHANNEL_NONE && (s_state & (1u << i))) {
            nrfx_wdt_channel_feed(&s_wdt, (nrfx_wdt_channel_id)s_channel[i]);
        }
    }
    ESP_LOGI("wdt", "WDT armed, %u ms window", (unsigned)BRAMBLE_WDT_RELOAD_MS);
}

void bramble_wdt_feed_all(void) {
    if (!(s_state & WDT_STATE_ARMED)) {
        return;
    }
    for (int i = 0; i < WDT_TASK_COUNT; i++) {
        if (s_channel[i] != WDT_CHANNEL_NONE) {
            nrfx_wdt_channel_feed(&s_wdt, (nrfx_wdt_channel_id)s_channel[i]);
        }
    }
}
