idf_component_register(
    SRCS "radio_airtime.c" "sx1262.c" "radio_esp.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_spi esp_driver_gpio freertos esp_timer board_config
)
