/* Host stub for FreeRTOS.h, scoped to test_wdt_nrf(_*). wdt_nrf.c uses the
 * bare nRF port's zero-argument critical section API (matching
 * nrf/src/ble_store_nvs.c and nrf/shim/gps_t1000e.c), not the ESP-IDF SMP
 * form (taskENTER_CRITICAL(&mux)) that test/stubs/freertos/FreeRTOS.h
 * provides for shared ESP32-and-host code; the two are not
 * interchangeable, which is why this test target gets its own copy rather
 * than reusing that one. A single-threaded host test has nothing to
 * protect against, so both are no-ops here. */
#pragma once

#include <stdbool.h> /* the real FreeRTOS.h pulls this in transitively; wdt_nrf.c uses bool */

#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
