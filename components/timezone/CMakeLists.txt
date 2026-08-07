idf_component_register(
    SRCS "bramble_tz.c" "tz_store.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash nvs_keys
)
