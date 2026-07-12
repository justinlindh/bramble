# Audio hardware is T-Deck-only and audio.c needs the I2S driver, which does
# not exist on the POSIX/Linux simulator: headers only there (no caller on
# the simulator; audio calls in main.c are inside T-Deck config gates).
idf_build_get_property(target IDF_TARGET)
if(${target} STREQUAL "linux")
    idf_component_register(INCLUDE_DIRS "include" REQUIRES board_config)
else()
    idf_component_register(
        SRCS "audio.c"
        INCLUDE_DIRS "include"
        REQUIRES nvs_keys driver board_config freertos esp_driver_i2s nvs_flash
    )
endif()
