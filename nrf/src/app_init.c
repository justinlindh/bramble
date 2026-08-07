// P1 boot path for the nRF52840 target: NVS, identity, then hand the node to
// the real mesh loop. mesh_task_start owns every subsystem init (network
// key, beacon key, routing tables, DM table, tx_gate, timers, the radio via
// radio_init) exactly as it does on the ESP32 fleet; keeping the init chain
// there is what keeps the two fleets running the same boot code.
#include "app_init.h"

#include "boot_trace.h"

#include <FreeRTOS.h>

#include "battery.h"
#include "battery_nrf.h"
#include "esp_log.h"
#include "identity.h"
#include "ble_host.h"
#include "mesh_task.h"
#include "msg_store.h"
#include "rpc_dispatcher.h"
#include "rpc_methods.h"
#include "ws_server.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "bramble_board.h"
#if BOARD_HAS_GNSS
#include "gps.h"
#include "gps_events.h"
#endif

static const char* TAG = "app_init";

static bramble_identity_t s_identity;

void app_init_stack(void) {
    /* Mounts the LittleFS settings partition; identity, network key and
     * channel state live here now and survive a reboot. */
    int nvs_rc = nvs_flash_init();
    ESP_LOGI(TAG, "nvs_flash_init: %d", nvs_rc);
    boot_trace_mark(BT_NVS_INIT, (uint32_t)nvs_rc);

    if (identity_load(&s_identity) == 0) {
        ESP_LOGI(TAG, "identity loaded: addr %08lx", (unsigned long)s_identity.address);
        boot_trace_mark(BT_IDENTITY, 0);
    } else {
        int rc = identity_generate_and_save(&s_identity);
        if (rc != 0) {
            ESP_LOGE(TAG, "identity generation FAILED (rc %d), mesh not started", rc);
            boot_trace_fail(BT_IDENTITY, (uint32_t)rc);
            return;
        }
        ESP_LOGI(TAG, "identity generated: addr %08lx", (unsigned long)s_identity.address);
        boot_trace_mark(BT_IDENTITY, 1);
    }
    boot_trace_mark(BT_IDENTITY_ADDR, s_identity.address);

    /* Restores persisted messages before the mesh starts, matching the ESP
     * boot order (main.c calls this before mesh_task_start). */
    msg_store_init_with_persistence();
    boot_trace_mark(BT_MSG_STORE, 0);

    /* Battery before the mesh starts, matching the ESP boot order (main.c
     * calls battery_init() before mesh_task_start): the mesh task's
     * immediate first beacon reads the battery, so the backend must exist
     * by then. On the T1000-E, init deliberately does NOT touch the P1.06
     * sensor rail: energizing it from boot context is the one action that
     * ever stopped an instrumented build dead, so the backend stays
     * disarmed (reads return 0, no hardware touched) until the arm call
     * after BT_BOOT_DONE below, and the first real gated window runs at
     * the first post-boot poll, the context the bench probe proved safe.
     * The stamp's aux is the persisted rail-probe verdict (battery_nrf.h):
     * 0 untried, 1 previous window died (voltage disabled), 2 proven. */
    battery_init();
    boot_trace_mark(BT_BATTERY_INIT, battery_probe_state());

    /* The dispatcher and its method table must exist before any transport
     * registers, because rpc_init() clears both tables. */
    rpc_init();
    rpc_methods_init(&s_identity);
    boot_trace_mark(BT_RPC_READY, 0);

    mesh_task_start(&s_identity);
    boot_trace_mark(BT_MESH_STARTED, 0);

#if BOARD_HAS_GNSS
    /* GNSS after the mesh: the fix callback's consumers (location policy,
     * RPC events) exist once the mesh is up. gps_init() reads the
     * persisted gps_pref preference itself and skips the first power-on
     * when it is off (still registering the fix callback either way, for
     * ESP boot parity with a later setGpsEnabled(true)), so this stage
     * does not make a separate gps_set_enabled(false) call. gps_init()
     * also returns almost immediately regardless of the pref: the AG3335's
     * ~1.55s power-on sequence runs entirely on the gnss task, not here. */
    int gps_rc = gps_init(nrf_on_gps_fix, NULL);
    boot_trace_mark(BT_GPS_INIT, (uint32_t)gps_rc);
#endif

    /* BLE last: the mesh owns the node's identity and RPC state, and the
     * transport should not accept a connection before they exist. */
    /* Consoleless boards cannot surface a minted token (the mint is logged
     * once, over a UART the T1000-E does not have), so a build-time token is
     * seeded first when one was provided. Seeds only if none is stored, and
     * must precede ws_server_load_token, which mints when it finds none. */
    extern int nrf_seed_auth_token_from_build(void);
    boot_trace_mark(BT_TOKEN_SEED, (uint32_t)nrf_seed_auth_token_from_build());

    /* Mints or loads the per-device RPC auth token. The entropy gate is
     * already open here (the hardware RNG opened it at boot), so unlike the
     * ESP boot path this cannot be deferred. */
    ws_server_load_token();
    boot_trace_mark(BT_TOKEN_LOADED, 0);

    if (ble_host_start() != 0) {
        ESP_LOGE(TAG, "BLE did not start; the node is mesh-only this boot");
    }
    boot_trace_mark(BT_BOOT_DONE, (uint32_t)xPortGetFreeHeapSize());

    /* Boot is over: allow rail-gated battery reads from here on. Kept
     * strictly after BT_BOOT_DONE, see the battery stanza above. */
    battery_runtime_arm();
    ESP_LOGI(TAG, "mesh_task_start returned; free heap %u bytes", (unsigned)xPortGetFreeHeapSize());
}
