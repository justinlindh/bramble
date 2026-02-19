idf_component_register(
    SRCS "main.c" "mesh_task.c" "cli.c" "rpc_methods.c" "ws_server.c" "board.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi esp_event driver esp_timer
             display button ui crypto identity
             radio packet routing dedup security freq_plan
             channel console esp_vfs_console rpc airtime
             wifi esp_http_server mdns esp_netif msg_store
             ble reliability battery ota board_config
             keyboard trackball location gps sdcard audio
             ui_graphics
)
