/*
 * BLE bring-up for the nRF52840 target: start the clocks the controller
 * needs, initialize NimBLE (host + controller in one image), then hand off
 * to the fleet's GATT server (components/ble/ble_server.c), which owns the
 * NUS service, the RPC transport registration and the pairing policy.
 */
#include "ble_host.h"

#include "ble_server.h"
#include "esp_log.h"
#include <FreeRTOS.h>
#include <task.h>

#include "nimble_glue.h"

static const char* TAG = "ble_host";

int ble_host_start(void) {
    /* RTC0, the controller's time base, does not tick without this. */
    bool xtal = nimble_glue_start_lfclk();
    ESP_LOGI(TAG, "LFCLK source: %s", xtal ? "crystal" : "RC");

    int rc = ble_server_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_server_init failed (%d)", rc);
        return rc;
    }
    rc = ble_server_start();
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_server_start failed (%d)", rc);
        return rc;
    }
    ESP_LOGI(TAG, "BLE up; free heap %u bytes", (unsigned)xPortGetFreeHeapSize());
    return 0;
}
