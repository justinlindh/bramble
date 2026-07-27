// P1 boot path for the nRF52840 target: NVS, identity, then hand the node to
// the real mesh loop. mesh_task_start owns every subsystem init (network
// key, beacon key, routing tables, DM table, tx_gate, timers, the radio via
// radio_init) exactly as it does on the ESP32 fleet; keeping the init chain
// there is what keeps the two fleets running the same boot code.
#include "app_init.h"

#include <FreeRTOS.h>

#include "esp_log.h"
#include "identity.h"
#include "mesh_task.h"
#include "nvs_flash.h"

static const char* TAG = "app_init";

static bramble_identity_t s_identity;

void app_init_stack(void) {
    ESP_LOGI(TAG, "nvs_flash_init: %d", nvs_flash_init());

    if (identity_load(&s_identity) == 0) {
        ESP_LOGI(TAG, "identity loaded: addr %08lx", (unsigned long)s_identity.address);
    } else {
        int rc = identity_generate_and_save(&s_identity);
        if (rc != 0) {
            ESP_LOGE(TAG, "identity generation FAILED (rc %d), mesh not started", rc);
            return;
        }
        ESP_LOGI(TAG, "identity generated: addr %08lx", (unsigned long)s_identity.address);
    }

    mesh_task_start(&s_identity);
    ESP_LOGI(TAG, "mesh_task_start returned; free heap %u bytes", (unsigned)xPortGetFreeHeapSize());
}
