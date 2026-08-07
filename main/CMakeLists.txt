set(main_srcs
    "main.c" "mesh_task.c"
    "mesh_beacon.c" "mesh_reliability.c" "mesh_probe.c" "mesh_dm.c"
    "mesh_routing.c" "mesh_location.c" "mesh_mailbox.c" "mesh_persist.c"
    "mesh_channels.c"
    "broadcast_delivery_receipt.c" "delivery_event_ring.c"
    "rpc_methods.c" "ws_server.c"
    "rpc_token.c" "ws_origin.c" "ws_auth_credential.c"
    "secure_nvs.c" "board.c" "util.c" "rreq_pseudonym.c" "beacon_policy_calc.c"
    "probe_results.c" "probe_reply.c" "rerr_ack_fastfail.c")

idf_build_get_property(target IDF_TARGET)
if(${target} STREQUAL "linux")
    # POSIX/Linux simulator (emulator/node): no UART/USB console (cli.c), no
    # esp_wifi/driver/esp_vfs_console/mdns components. null_drivers is the
    # emulator's spike-only peripheral implementation (radio/display/button/
    # battery/wifi/ota device halves).
    idf_component_register(
        SRCS ${main_srcs}
        INCLUDE_DIRS "."
        REQUIRES indicators nvs_keys nvs_flash esp_event esp_timer esp_app_format
                 display button ui crypto identity
                 radio packet routing dedup security freq_plan
                 channel console rpc airtime
                 wifi esp_http_server esp_netif msg_store spiffs
                 ble reliability battery ota board_config
                 keyboard trackball location gps sdcard audio
                 ui_graphics traffic_debug fragment timesync mailbox timezone
                 nonce_counter replay_window dm_session network_key routing_auth
                 null_drivers
    )
else()
    idf_component_register(
        SRCS ${main_srcs} "cli.c"
        INCLUDE_DIRS "."
        REQUIRES indicators nvs_keys nvs_flash esp_wifi esp_event driver esp_timer
                 display button ui crypto identity
                 radio packet routing dedup security freq_plan
                 channel console esp_vfs_console rpc airtime
                 wifi esp_http_server mdns esp_netif msg_store spiffs
                 ble reliability battery ota board_config
                 keyboard trackball location gps sdcard audio
                 ui_graphics traffic_debug fragment timesync mailbox timezone
                 bootloader_support nonce_counter replay_window dm_session network_key routing_auth

        PRIV_REQUIRES lvgl__lvgl
    )
endif()

if(CONFIG_BRAMBLE_BOARD_TDECK_PLUS)
    target_sources(${COMPONENT_LIB} PRIVATE "lv_malloc_psram.c")
    target_link_options(${COMPONENT_LIB} INTERFACE
        -Wl,-u,lv_malloc_core
        -Wl,-u,lv_free_core
        -Wl,-u,lv_realloc_core
        -Wl,-u,lv_mem_init
    )
endif()
