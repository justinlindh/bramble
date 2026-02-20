if(CONFIG_BRAMBLE_BOARD_TDECK_PLUS)
    set(display_srcs "st7789.c")
    set(display_requires driver esp_driver_spi esp_driver_gpio esp_driver_ledc board_config)
else()
    set(display_srcs "ssd1306.c")
    set(display_requires driver esp_driver_i2c esp_driver_gpio board_config)
endif()

idf_component_register(
    SRCS ${display_srcs}
    INCLUDE_DIRS "include"
    REQUIRES ${display_requires}
)
