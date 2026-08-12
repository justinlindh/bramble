// Real nRF52840 watchdog backing esp_task_wdt.h (see that file for the
// portable API shared code calls) and the nRF-only init/arm lifecycle in
// bramble_wdt.h.
//
// Hardware model: nrfx_wdt exposes up to 8 independent reload-request
// channels (RR0-RR7) sharing one countdown. Every ENABLED channel must be
// fed within the window or the peripheral resets, regardless of which
// channel was missed (nRF52840 Product Specification, WDT chapter: "To
// reload the watchdog counter, the special value 0x6E524635 needs to be
// written to all enabled reload registers"). That maps directly onto
// ESP-IDF's task watchdog "every subscribed task must check in" semantics,
// one hardware channel per task instead of software subscriber
// bookkeeping. This build gives a channel to exactly 2 tasks: mesh and
// radio. See "Why only mesh and radio" below for why NimBLE's link-layer
// and host tasks do not get one, and why that is not a coverage gap for
// the failure this change targets.
//
// Timeout: 60 seconds. This number is critical for DFU safety, not
// just hang-detection latency; read "DFU survival" below before changing
// it. It comfortably covers every legitimate blocking operation in the
// firmware: the worst is the 4s LoRa TX wait (main/mesh_task.c), NVMC page
// erase is 85ms (nRF52840 Product Specification, NVMC chapter,
// tERASEPAGE), and littlefs operations are built from those same word
// writes and page erases. A hung node recovering in up to 60s instead of
// never is the entire win this change delivers; a mesh node that beacons
// on the order of minutes loses essentially nothing by that margin
// compared to a shorter window, and a shorter window is unsafe here (see
// below).
//
// Two hard nRF52840 constraints shape this file:
//
//   - nrfx_wdt_channel_alloc() cannot be called once nrfx_wdt_enable() has
//     run (channel_alloc asserts driver state == INITIALIZED; enable moves
//     it to POWERED_ON, and there is no going back). bramble_wdt_arm() is
//     therefore called exactly once, from nrf/src/app_init.c, only after
//     every subscriber task is guaranteed to have already registered. That
//     guarantee comes from FreeRTOS's fixed-priority preemptive scheduler,
//     not a timing heuristic: mesh (prio 5) and radio (5) both outrank the
//     boot task (3) that calls bramble_wdt_arm(), each calls
//     esp_task_wdt_add() as the first thing it does, and creating a
//     strictly-higher-priority task preempts the creator immediately in
//     this port (configUSE_PREEMPTION 1). The boot task therefore cannot
//     reach the arm call until both of those tasks have run at least as
//     far as their own wdt_add and then blocked, which is exactly the
//     registration this file depends on. esp_task_wdt_add() below still
//     refuses an add() that arrives after arm rather than trusting that
//     argument to hold forever, since a future subscriber added without
//     extending this reasoning would otherwise hit an NRFX_ASSERT and
//     reboot the board.
//   - The nRF52840's WDT has no TASKS_STOP register at all (confirmed in
//     the vendored SVD headers: NRF_WDT_HAS_STOP, which nrfx_wdt.h derives
//     from whether WDT_TASKS_STOP_TASKS_STOP_Msk is defined, resolves to 0
//     for this part; that symbol is simply absent from nrf52840.h). Once
//     armed, the watchdog runs until a full power-on or pin reset; nothing
//     in this file, or anywhere else in software, can turn it back off.
//     That is why bramble_wdt_arm() is deferred to steady state instead of
//     called from main(): see the comment at its call site.
//
// DFU survival: the stock Adafruit nRF52 bootloader this board ships
// (Seeed's build) has NO watchdog code anywhere in it (checked its
// src/main.c: no reference to NRF_WDT, WDT_RUNSTATUS, or any reload
// register, in any DFU mode: UF2, serial, or BLE OTA). It cannot feed a
// channel this file armed, and per the "all enabled channels" rule above,
// feeding only some would not help anyway. Whether the WDT keeps counting
// across the SYSRESETREQ that nrf/src/boot_trace.c's reboot_to_dfu() (and
// bramble.enterDfu, nrf/shim/esp_system_nrf.c) use to enter DFU is not
// settled by the Product Specification text alone ("after a reset ... the
// watchdog configuration registers will be available for configuration
// again" is ambiguous about a still-running countdown), but Nordic's own
// nRF5 SDK bootloader includes watchdog-feed code for exactly this
// situation, which would be pointless if a soft reset stopped it. This
// file treats SURVIVAL AS THE DESIGN ASSUMPTION, the unsafe direction,
// since the watchdog cannot be stopped once armed regardless. The
// consequence: once armed, every DFU session on this board (a UF2
// drag-and-drop copy, ~695 KB measured from this build's own
// bramble-nrf.uf2, 1357 blocks of 512 bytes) is on a clock, and must
// complete within one watchdog period or risk a reset mid-write that
// strands a partially flashed app recoverable only by the physical button
// gesture. 60s is chosen with that in mind, not just hang latency: a
// drag-and-drop copy of an image this size, including USB enumeration and
// the host noticing the new volume, plausibly takes low tens of seconds,
// and 60s leaves real margin over that without being so long that a
// resumed-but-still-hung board takes an unreasonable time to self-heal in
// the field. This is a design assumption, not a bench-confirmed fact; the
// bench test that would confirm or refute it (arm the watchdog, trigger
// enterDfu, and watch whether the UF2 volume stays mounted past one
// watchdog period) has not been run, per this repo's evidence-before-
// assertion standard. If survival turns out to be false, this design is
// still safe (the DFU session simply is never at risk); if survival is
// true and a session ever runs long, this is the residual risk that
// remains.
//
// Why only mesh and radio: NimBLE's link layer and host tasks (both real
// stall risks per the investigation this change implements) block forever
// on the vendored NimBLE OS porting layer's single blocking primitive
// (porting/npl/freertos/src/npl_os_freertos.c), which has no per-iteration
// hook to feed from without patching link-layer-owned upstream code. This
// port already carries two patches against that area (nrf/CMakeLists.txt)
// for correctness fixes that needed real bench time to trust; a third,
// purely for a watchdog feed, was tried and reverted; a patch that only
// applies cleanly to a pristine NimBLE checkout is fragile against normal
// CI cache behavior (a restored, differently-patched source tree makes the
// whole multi-patch git apply abort, observed in this PR's own CI), and
// every extra channel is another task that must never block longer than
// the (now 60s) period or it becomes an instant reset loop. Mesh and
// radio, which this build does cover, catch the cross-cutting hangs the
// field evidence actually shows (BLE and mesh going dark together,
// PR #514's bench report); a BLE-only hang that leaves mesh and radio both
// healthy is the residual, explicitly accepted gap.
//
// Identification by task NAME, not TaskHandle_t: the t1000e static-RAM gate
// (nrf/scripts/size_report.py) has essentially no slack (it was passing by
// 8 bytes before this file existed), so a per-task table cannot afford a
// 4-byte TaskHandle_t slot on top of the channel it maps to. Both tasks
// this build gives a channel to already have a fixed name assigned at
// xTaskCreate (main/mesh_task.c "mesh", nrf/src/radio_lr1110.c "radio"),
// and pcTaskGetName() reads it straight out of the TCB FreeRTOS already
// allocated, so matching on that name costs this file only the channel
// byte itself, not an identity field alongside it.
//
// A registered task that stops feeding its channel resets the device by
// design. The corollary is the one real correctness hazard in this file:
// esp_task_wdt_delete() cannot ask the hardware to forget a channel either
// (the same missing-stop constraint applies to nrfx_wdt_channels_free(),
// documented as callable only when the driver is not running). A task that
// legitimately exits after registering, such as mesh_task when
// radio_init() fails (main/mesh_task.c), would otherwise starve its own
// channel and reset the device forever: a graceful "no radio" degraded
// mode turned into a boot loop. s_state's deleted bits track exactly this
// case. A deleted channel is kept fed forever, as a proxy, by every other
// live esp_task_wdt_reset() call, standing in for "no longer monitored".
// If every other subscriber ever stops too, the deleted channel stops
// being fed along with them, and the device resets, which is correct: at
// that point the whole system, not just the task that opted out, has
// actually hung.
//
// Known hardware limit, not solved here: a wedge that also stops LFCLK
// (the watchdog's clock source) leaves the WDT unable to fire at all.
// Nothing in software can recover that case; it is a genuine limit of the
// hardware, not a design choice this file makes.
//
// s_state is shared, mutable, and written from more than one task context:
// bramble_wdt_arm() runs on the boot task, esp_task_wdt_delete() runs on
// mesh_task when radio_init() fails (main/mesh_task.c). The priority
// argument above proves boot cannot run add() concurrently with mesh or
// radio (both outrank it and haven't blocked yet), but it says nothing
// about mesh going blocked-to-ready and calling delete() while arm() is
// mid-update: `s_state |= X` is a plain load-modify-store, and an
// interleaved pair of those loses whichever update's store lands first.
// A lost deleted-bit is not a bookkeeping curiosity here: it leaves a
// channel enabled with nobody feeding it, and per the "every enabled
// channel" rule above, that resets the board on its own, for reasons
// having nothing to do with an actual hang. On a consoleless board that
// looks exactly like the wedge this file exists to catch and recover from,
// which would poison the one diagnostic signal (BOOT_BEGIN [DOG] vs
// [RESETPIN]) this change gives the field investigation. esp_task_wdt_add,
// esp_task_wdt_delete, and bramble_wdt_arm each therefore run their
// check-then-mutate sequence inside one taskENTER_CRITICAL/EXIT_CRITICAL
// pair (same pattern as nrf/src/ble_store_nvs.c and
// nrf/shim/gps_t1000e.c), covering the nrfx calls too: nrfx_wdt_enable()
// and nrfx_wdt_channel_alloc() are unsynchronized against each other in
// nrfx itself, and add() calling alloc() after arm() has already called
// enable() trips an NRFX_ASSERT (reboot to DFU), the same bad outcome as a
// lost deleted-bit by a different path. Logging never happens inside a
// critical section (it can block on a UART write on the dev kit); each
// function decides what to log from a plain local afterward. Byte reads of
// s_state elsewhere (esp_task_wdt_reset, bramble_wdt_feed_all) are
// unguarded on purpose: a naturally-aligned uint8_t load is atomic on this
// core, so a plain read can only see a fully-old or fully-new value, never
// a torn one, and reset()/feed_all() never write it, so there is no
// update to lose.
#include "esp_task_wdt.h"
#include "bramble_wdt.h"

#include <nrfx_wdt.h>

#include <FreeRTOS.h>
#include <task.h>

#include <string.h>

#include "esp_log.h"

// See the file header's "Timeout" section for the full justification.
#define BRAMBLE_WDT_RELOAD_MS 60000u

// One instance on this part (WDT0); NRFX_WDT0_ENABLED selects it in
// nrf/config/nrfx_config.h. const: lives in flash, not RAM.
static const nrfx_wdt_t s_wdt = NRFX_WDT_INSTANCE(0);

typedef enum {
    WDT_TASK_MESH = 0,
    WDT_TASK_RADIO,
    WDT_TASK_COUNT,
} wdt_task_id_t;

#define WDT_CHANNEL_NONE 0xFFu

static uint8_t s_channel[WDT_TASK_COUNT]; // nrfx channel per task id, or WDT_CHANNEL_NONE

// One byte, not three: the t1000e static-RAM gate has no room to spare
// (see the file header). Bits 0-1 are the per-task deleted mask
// (esp_task_wdt_delete, one bit per wdt_task_id_t); bit 6 is "nrfx_wdt_init
// succeeded"; bit 7 is "nrfx_wdt_enable has run, countdown is live".
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
    return -1;
}

void bramble_wdt_init(void) {
    taskENTER_CRITICAL();
    bool already = (s_state & WDT_STATE_DRIVER_READY) != 0;
    taskEXIT_CRITICAL();
    if (already) {
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
    taskENTER_CRITICAL();
    s_state |= WDT_STATE_DRIVER_READY;
    taskEXIT_CRITICAL();
}

// esp_task_wdt_add()'s outcome, decided inside the critical section so the
// function can log and return afterward: see the file header for why
// logging never happens with interrupts disabled.
typedef enum {
    WDT_ADD_OK,
    WDT_ADD_NOT_READY, // driver not initialized yet: no-op, matches the old shim
    WDT_ADD_ALREADY,   // this task already has a channel
    WDT_ADD_ARMED,     // too late: nrfx_wdt_channel_alloc can no longer run
    WDT_ADD_ALLOC_FAILED,
} wdt_add_result_t;

esp_err_t esp_task_wdt_add(TaskHandle_t task_handle) {
    TaskHandle_t task = task_handle ? task_handle : xTaskGetCurrentTaskHandle();
    int id = task_id_for(task);
    if (id < 0) {
        // Not one of this build's known subscribers (see file header):
        // give it nothing to feed rather than fail a caller that has no
        // channel available for it. Matches the old shim's blanket ESP_OK.
        return ESP_OK;
    }

    wdt_add_result_t result;
    nrfx_err_t alloc_rc = NRFX_SUCCESS;
    taskENTER_CRITICAL();
    if (!(s_state & WDT_STATE_DRIVER_READY)) {
        result = WDT_ADD_NOT_READY;
    } else if (s_channel[id] != WDT_CHANNEL_NONE) {
        result = WDT_ADD_ALREADY;
    } else if (s_state & WDT_STATE_ARMED) {
        // See the file header: every task this build gives a channel to
        // is guaranteed to have already called this before bramble_wdt_arm()
        // ran. Reaching here means that guarantee did not hold for this
        // caller; nrfx_wdt_channel_alloc() cannot be called anymore, so
        // refuse instead of asserting the board into a DFU reboot.
        result = WDT_ADD_ARMED;
    } else {
        nrfx_wdt_channel_id ch;
        alloc_rc = nrfx_wdt_channel_alloc(&s_wdt, &ch);
        if (alloc_rc == NRFX_SUCCESS) {
            s_channel[id] = (uint8_t)ch;
            result = WDT_ADD_OK;
        } else {
            result = WDT_ADD_ALLOC_FAILED;
        }
    }
    taskEXIT_CRITICAL();

    switch (result) {
    case WDT_ADD_NOT_READY:
    case WDT_ADD_ALREADY:
    case WDT_ADD_OK:
        return ESP_OK;
    case WDT_ADD_ARMED:
        ESP_LOGW("wdt", "WDT already armed, cannot add task id %d", id);
        return ESP_ERR_INVALID_STATE;
    case WDT_ADD_ALLOC_FAILED:
    default:
        ESP_LOGE("wdt", "nrfx_wdt_channel_alloc failed: 0x%x", (unsigned)alloc_rc);
        return ESP_ERR_NO_MEM;
    }
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
    if (id < 0) {
        return ESP_OK; // never subscribed: matches the old shim's blanket ESP_OK
    }

    bool feed_now = false;
    taskENTER_CRITICAL();
    if (s_channel[id] != WDT_CHANNEL_NONE) {
        s_state |= (uint8_t)(1u << id);
        // Immediate feed once we leave the critical section: nothing else
        // touches this channel until some other still-live task's
        // esp_task_wdt_reset() runs.
        feed_now = (s_state & WDT_STATE_ARMED) != 0;
    }
    taskEXIT_CRITICAL();

    if (feed_now) {
        nrfx_wdt_channel_feed(&s_wdt, (nrfx_wdt_channel_id)s_channel[id]);
    }
    return ESP_OK;
}

void bramble_wdt_arm(void) {
    bool armed_now = false;
    bool any = false;
    taskENTER_CRITICAL();
    if (!(s_state & WDT_STATE_ARMED) && (s_state & WDT_STATE_DRIVER_READY)) {
        for (int i = 0; i < WDT_TASK_COUNT; i++) {
            if (s_channel[i] != WDT_CHANNEL_NONE) {
                any = true;
                break;
            }
        }
        // nrfx_wdt_enable() and the subscriber-registration side of
        // nrfx_wdt_channel_alloc() (inside esp_task_wdt_add(), same
        // critical section) are unsynchronized against each other in nrfx
        // itself; running both here, atomically with the WDT_STATE_ARMED
        // flag that gates them, is what makes esp_task_wdt_add()'s
        // post-arm refusal actually enforceable instead of a race.
        nrfx_wdt_enable(&s_wdt);
        s_state |= WDT_STATE_ARMED;
        for (int i = 0; i < WDT_TASK_COUNT; i++) {
            // First feed for anything already deleted before arm ran (in
            // principle: radio_init() failing before boot even finished).
            // Everything else gets its first feed from its own owning
            // task's ordinary post-boot cadence, which by construction is
            // already running by this point.
            if (s_channel[i] != WDT_CHANNEL_NONE && (s_state & (1u << i))) {
                nrfx_wdt_channel_feed(&s_wdt, (nrfx_wdt_channel_id)s_channel[i]);
            }
        }
        armed_now = true;
    }
    taskEXIT_CRITICAL();

    if (!armed_now) {
        return;
    }
    if (!any) {
        ESP_LOGW("wdt", "arming WDT with zero registered channels");
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
