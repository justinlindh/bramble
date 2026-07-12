# The POSIX/Linux simulator has no driver components; board_config.h falls
# back to its host-side type stubs there (see the header).
idf_build_get_property(target IDF_TARGET)
if(${target} STREQUAL "linux")
    idf_component_register(INCLUDE_DIRS "include")
else()
    idf_component_register(
        INCLUDE_DIRS "include"
        REQUIRES driver esp_driver_spi esp_driver_gpio
    )
endif()
