idf_component_register(
    SRCS "sdcard.c"
    INCLUDE_DIRS "include"
    REQUIRES driver board_config fatfs esp_driver_sdspi sdmmc esp_vfs_console vfs
)
