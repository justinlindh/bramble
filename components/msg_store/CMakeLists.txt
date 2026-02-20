idf_component_register(
    SRCS "msg_store.c" "msg_store_spiffs.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_timer spiffs
)
