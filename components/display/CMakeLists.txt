# Display backend selection.
#
# The IDF linux target (emulator node) has no I2C/SPI drivers: it builds
# the pure SSD1680 engine plus display_virt.c, which ships framebuffers to
# the emu-link broker instead of a panel.
idf_build_get_property(target IDF_TARGET)
if(${target} STREQUAL "linux")
    idf_component_register(
        SRCS "ssd1680_engine.c" "display_virt.c"
        INCLUDE_DIRS "include"
        REQUIRES board_config emu_link json
    )
    return()
endif()

if(CONFIG_BRAMBLE_BOARD_TDECK_PLUS)
    set(display_srcs "st7789.c")
    set(display_requires driver esp_driver_spi esp_driver_gpio esp_driver_ledc board_config)
elseif(CONFIG_BRAMBLE_BOARD_PAGER)
    # Bramble Pager v1: SSD1680 e-paper = pure engine + SPI transport.
    set(display_srcs "ssd1680_engine.c" "ssd1680_io.c")
    set(display_requires driver esp_driver_spi esp_driver_gpio board_config)
else()
    set(display_srcs "ssd1306.c")
    set(display_requires driver esp_driver_i2c esp_driver_gpio board_config)
endif()

idf_component_register(
    SRCS ${display_srcs}
    INCLUDE_DIRS "include"
    REQUIRES ${display_requires}
)
