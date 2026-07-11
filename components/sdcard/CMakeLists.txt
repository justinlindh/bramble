# SD card hardware is T-Deck-only and needs sdmmc/sdspi, which do not exist
# on the POSIX/Linux simulator: headers only there (no caller on the
# simulator; sdcard calls in main.c are inside T-Deck config gates).
idf_build_get_property(target IDF_TARGET)
if(${target} STREQUAL "linux")
    idf_component_register(INCLUDE_DIRS "include" REQUIRES board_config)
else()
    idf_component_register(
        SRCS "sdcard.c"
        INCLUDE_DIRS "include"
        REQUIRES driver board_config fatfs esp_driver_sdspi sdmmc esp_vfs_console vfs
    )
endif()
