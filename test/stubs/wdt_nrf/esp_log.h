/* Host stub for esp_log.h, scoped to test_wdt_nrf(_*). The real
 * nrf/shim/include/esp_log.h routes through bramble_log_write(), the
 * nRF UART logger; this test target has no UART, so the macros just drop
 * the arguments (matching test/stubs/esp_log.h's approach for the shared
 * host test suite, kept as a private copy here so this target does not
 * depend on that directory's include order). */
#pragma once

#define ESP_LOGE(tag, fmt, ...) ((void)0)
#define ESP_LOGW(tag, fmt, ...) ((void)0)
#define ESP_LOGI(tag, fmt, ...) ((void)0)
#define ESP_LOGD(tag, fmt, ...) ((void)0)
