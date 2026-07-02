idf_component_register(
    SRCS "main.c" "mesh_task.c" "broadcast_delivery_receipt.c" "delivery_event_ring.c" "cli.c" "rpc_methods.c" "ws_server.c" "ws_origin.c" "ws_auth_credential.c" "board.c" "util.c"
         "rerr_ack_fastfail.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_keys nvs_flash esp_wifi esp_event driver esp_timer
             display button ui crypto identity
             radio packet routing dedup security freq_plan
             channel console esp_vfs_console rpc airtime
             wifi esp_http_server mdns esp_netif msg_store spiffs
             ble reliability battery ota board_config
             keyboard trackball location gps sdcard audio
             ui_graphics traffic_debug fragment timesync mailbox
             bootloader_support

    PRIV_REQUIRES lvgl__lvgl
)

if(CONFIG_BRAMBLE_BOARD_TDECK_PLUS)
    target_sources(${COMPONENT_LIB} PRIVATE "lv_malloc_psram.c")
    target_link_options(${COMPONENT_LIB} INTERFACE
        -Wl,-u,lv_malloc_core
        -Wl,-u,lv_free_core
        -Wl,-u,lv_realloc_core
        -Wl,-u,lv_mem_init
    )
endif()
