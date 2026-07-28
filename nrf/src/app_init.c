// P1 boot path for the nRF52840 target: NVS, identity, then hand the node to
// the real mesh loop. mesh_task_start owns every subsystem init (network
// key, beacon key, routing tables, DM table, tx_gate, timers, the radio via
// radio_init) exactly as it does on the ESP32 fleet; keeping the init chain
// there is what keeps the two fleets running the same boot code.
#include "app_init.h"

#include <FreeRTOS.h>

#include "esp_log.h"
#include "identity.h"
#include "ble_host.h"
#include "mesh_task.h"
#include "msg_store.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char* TAG = "app_init";

static bramble_identity_t s_identity;

void app_init_stack(void) {
    /* Mounts the LittleFS settings partition; identity, network key and
     * channel state live here now and survive a reboot. */
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

    /* Restores persisted messages before the mesh starts, matching the ESP
     * boot order (main.c calls this before mesh_task_start). */
    msg_store_init_with_persistence();

    mesh_task_start(&s_identity);

    /* BLE last: the mesh owns the node's identity and RPC state, and the
     * transport should not accept a connection before they exist. */
    if (ble_host_start() != 0) {
        ESP_LOGE(TAG, "BLE did not start; the node is mesh-only this boot");
    }
    ESP_LOGI(TAG, "mesh_task_start returned; free heap %u bytes", (unsigned)xPortGetFreeHeapSize());
}
