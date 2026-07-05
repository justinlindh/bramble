idf_component_register(
    SRCS "identity.c" "identity_store.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_keys crypto nvs_flash packet
)
