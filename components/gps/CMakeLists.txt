# The POSIX/Linux simulator has no UART driver: gps.c compiles its host stub
# half there (see the CONFIG_IDF_TARGET_LINUX gate in gps.c) and the pure
# nmea_parser.c builds as-is.
idf_build_get_property(target IDF_TARGET)
if(${target} STREQUAL "linux")
    idf_component_register(
        SRCS "gps.c" "nmea_parser.c"
        INCLUDE_DIRS "include"
        REQUIRES board_config location esp_timer
    )
else()
    idf_component_register(
        SRCS "gps.c" "nmea_parser.c"
        INCLUDE_DIRS "include"
        REQUIRES driver board_config location esp_timer esp_driver_uart
    )
endif()
