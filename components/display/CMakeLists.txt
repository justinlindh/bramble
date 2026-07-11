# The POSIX/Linux simulator has no I2C/SPI drivers: the display component is
# headers-only there and the emulator's driver component implements display.h
# (null_drivers for the spike, the ssd1680 pipeline later).
idf_build_get_property(target IDF_TARGET)
if(${target} STREQUAL "linux")
    idf_component_register(INCLUDE_DIRS "include" REQUIRES board_config)
    return()
endif()

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
