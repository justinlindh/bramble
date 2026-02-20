idf_component_register(
    SRCS "audio.c"
    INCLUDE_DIRS "include"
    REQUIRES driver board_config freertos esp_driver_i2s nvs_flash
)
