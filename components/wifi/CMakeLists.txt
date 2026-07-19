# esp_wifi does not exist on the POSIX/Linux simulator: headers only there;
# the emulator's driver component (emulator/node/null_drivers) provides the
# wifi_manager symbols, all reporting "no wifi".
idf_build_get_property(target IDF_TARGET)
if(${target} STREQUAL "linux")
    # wifi_ap_password.c is pure (crypto only, no esp_wifi), so it builds on
    # the linux target too and the emulator's null drivers can still resolve
    # the derivation if they need it.
    idf_component_register(
        SRCS "wifi_ap_password.c"
        INCLUDE_DIRS "include"
        REQUIRES crypto
    )
else()
    idf_component_register(
        SRCS "wifi_manager.c" "wifi_ap_password.c"
        INCLUDE_DIRS "include"
        REQUIRES nvs_keys esp_wifi esp_event esp_netif nvs_flash crypto
    )
endif()
