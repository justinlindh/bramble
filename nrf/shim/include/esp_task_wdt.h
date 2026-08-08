// esp_task_wdt shim for the nRF52840 target, backed by the real nrfx_wdt
// peripheral (nrf/shim/wdt_nrf.c). Same call signatures as ESP-IDF's
// esp_task_wdt so every existing esp_task_wdt_add/reset/delete call site in
// shared code (main/mesh_task.c, main/mesh_reliability.c, main/mesh_probe.c,
// components/radio/tx_gate_esp.c) and nrf/src/radio_lr1110.c works
// unchanged; this header intentionally carries no nRF-only additions to
// that surface, so shared code stays portable.
//
// Every call here is a harmless no-op until the watchdog is armed
// (bramble_wdt_arm(), nrf/shim/include/bramble_wdt.h), matching the no-op
// shim this file replaces. It is armed only after boot reaches steady
// state, so the many WDT calls that already run during boot today (mesh
// task's early jitter delay and first beacon, for example) stay exactly as
// safe as before.
#pragma once

#include "esp_err.h"

#include <FreeRTOS.h>
#include <task.h>

#ifdef __cplusplus
extern "C" {
#endif

// task_handle NULL means "the calling task", matching ESP-IDF.
esp_err_t esp_task_wdt_add(TaskHandle_t task_handle);

// Feeds the calling task's own channel. No-op (ESP_OK) before the watchdog
// is armed; ESP_ERR_NOT_FOUND if the calling task never subscribed.
esp_err_t esp_task_wdt_reset(void);

// task_handle NULL means "the calling task", matching ESP-IDF. The nRF WDT
// has no live per-channel free (see wdt_nrf.c); this marks the channel
// opted out and keeps it fed by proxy so it can never starve on its own.
esp_err_t esp_task_wdt_delete(TaskHandle_t task_handle);

#ifdef __cplusplus
}
#endif
