// P0 boot path for the nRF52840 target: initializes the portable protocol
// stack the same way main/main.c does on ESP32 (NVS, identity, network key,
// routing tables, message store, DM table, text UI, radio) so the linked
// image carries the stack's honest static footprint and the init paths run
// on real silicon. No mesh_task yet: that arrives with the radio in P1.
#include "app_init.h"

#include <FreeRTOS.h>

#include "crypto.h"
#include "dm_session.h"
#include "esp_log.h"
#include "identity.h"
#include "msg_store.h"
#include "network_key.h"
#include "nvs_flash.h"
#include "radio.h"
#include "routing.h"
#include "ui.h"

static const char* TAG = "app_init";

static bramble_identity_t s_identity;
static neighbor_table_t s_neighbors;
static routing_table_t s_routes;
static rreq_dedup_t s_rreq_dedup;
static reverse_route_table_t s_reverse_routes;
static dm_table_t s_dm_table;
static ui_state_t s_ui;

void app_init_stack(void) {
    ESP_LOGI(TAG, "nvs_flash_init: %d", nvs_flash_init());

    if (identity_load(&s_identity) == 0) {
        ESP_LOGI(TAG, "identity loaded: addr %08lx", (unsigned long)s_identity.address);
    } else {
        int rc = identity_generate_and_save(&s_identity);
        ESP_LOGI(TAG, "identity generated: rc %d addr %08lx", rc,
                 (unsigned long)s_identity.address);
    }

    int nk = network_key_load_from_nvs();
    ESP_LOGI(TAG, "network key: %s", nk == 0 ? "loaded" : "not provisioned");

    neighbor_init(&s_neighbors);
    route_init(&s_routes);
    rreq_dedup_init(&s_rreq_dedup);
    reverse_route_init(&s_reverse_routes);
    ESP_LOGI(TAG, "routing tables initialized");

    msg_store_init();
    ESP_LOGI(TAG, "msg store initialized");

    dm_table_init(&s_dm_table);
    ESP_LOGI(TAG, "dm table initialized");

    ui_init(&s_ui);
    ESP_LOGI(TAG, "text ui initialized");

    radio_config_t cfg;
    radio_get_profile_config(RADIO_PROFILE_LONG_RANGE, &cfg);
    ESP_LOGI(TAG, "radio_init: %d (profile LR %u.%02u MHz sf%u)", radio_init(&cfg),
             (unsigned)cfg.frequency_mhz, (unsigned)((uint32_t)(cfg.frequency_mhz * 100) % 100),
             cfg.sf);

    ESP_LOGI(TAG, "stack up: free heap %u bytes", (unsigned)xPortGetFreeHeapSize());
}
