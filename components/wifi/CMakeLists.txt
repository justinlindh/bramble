# esp_wifi does not exist on the POSIX/Linux simulator: headers only there;
# the emulator's driver component (emulator/node/null_drivers) provides the
# wifi_manager symbols, all reporting "no wifi".
idf_build_get_property(target IDF_TARGET)
if(${target} STREQUAL "linux")
    idf_component_register(INCLUDE_DIRS "include")
else()
    idf_component_register(
        SRCS "wifi_manager.c"
        INCLUDE_DIRS "include"
        REQUIRES nvs_keys esp_wifi esp_event esp_netif nvs_flash
    )
endif()
