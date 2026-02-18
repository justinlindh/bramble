idf_component_register(
    SRCS "ble_server.c"
    INCLUDE_DIRS "include"
    REQUIRES bt rpc nvs_flash freertos
)
