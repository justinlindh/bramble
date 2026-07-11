# The POSIX/Linux simulator has no ADC: headers only there; the emulator's
# driver component implements battery.h (null_drivers for the spike,
# battery_virt later).
idf_build_get_property(target IDF_TARGET)
if(${target} STREQUAL "linux")
    idf_component_register(INCLUDE_DIRS "include" REQUIRES board_config)
else()
    idf_component_register(
        SRCS "battery.c"
        INCLUDE_DIRS "include"
        REQUIRES esp_adc board_config
    )
endif()
