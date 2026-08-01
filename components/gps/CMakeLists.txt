# The POSIX/Linux simulator has no UART driver: on the IDF linux target the
# virtual driver gps_virt.c implements gps.h (feeding the pure nmea_parser.c
# from emu-link `nmea` messages and gating GNSS power via `gpsgate`); gps.c
# itself contributes nothing there (see the CONFIG_IDF_TARGET_LINUX gate).
idf_build_get_property(target IDF_TARGET)
if(${target} STREQUAL "linux")
    idf_component_register(
        SRCS "gps.c" "gps_virt.c" "gps_feed.c" "gps_pref.c" "gps_duty.c" "nmea_parser.c"
        INCLUDE_DIRS "include"
        REQUIRES board_config location esp_timer emu_link json nvs_flash nvs_keys
    )
else()
    idf_component_register(
        SRCS "gps.c" "gps_feed.c" "gps_pref.c" "gps_duty.c" "nmea_parser.c"
        INCLUDE_DIRS "include"
        REQUIRES driver board_config location esp_timer esp_driver_uart nvs_flash nvs_keys
    )
endif()
