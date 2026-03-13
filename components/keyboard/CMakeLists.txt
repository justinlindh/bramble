idf_component_register(
    SRCS "keyboard.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_keys driver board_config esp_timer nvs_flash
)
