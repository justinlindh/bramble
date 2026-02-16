idf_component_register(
    SRCS "identity.c"
    INCLUDE_DIRS "include"
    REQUIRES crypto nvs_flash
)
