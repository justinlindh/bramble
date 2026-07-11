# The POSIX/Linux simulator has no GPIO driver: headers only there; the
# emulator's driver component implements button.h (null_drivers for the
# spike, button_virt later).
idf_build_get_property(target IDF_TARGET)
if(${target} STREQUAL "linux")
    idf_component_register(INCLUDE_DIRS "include" REQUIRES ui board_config)
else()
    idf_component_register(
        SRCS "button.c"
        INCLUDE_DIRS "include"
        REQUIRES driver esp_driver_gpio ui board_config
    )
endif()
