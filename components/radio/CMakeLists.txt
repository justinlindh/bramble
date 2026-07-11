# On the POSIX/Linux simulator the SX1262 driver cannot exist (no SPI/GPIO
# drivers); only the hardware-independent sources build there. The radio.h
# implementation on host comes from radio_virt.c (the emu_link ether client,
# replacing the spike's null_drivers/radio_null.c); tx_gate_esp.c is kept so
# the real TX gate logic runs on host. radio_profiles.c holds the profile
# table (single source of truth) and builds on every target.
idf_build_get_property(target IDF_TARGET)
if(${target} STREQUAL "linux")
    idf_component_register(
        SRCS "radio_airtime.c" "radio_profiles.c" "radio_virt.c" "tx_gate.c" "tx_gate_esp.c"
             "phy_passthrough.c"
        INCLUDE_DIRS "include"
        REQUIRES freertos esp_timer board_config esp_system esp_hw_support airtime emu_link json
    )
    target_link_libraries(${COMPONENT_LIB} PRIVATE pthread)
else()
    idf_component_register(
        SRCS "radio_airtime.c" "radio_profiles.c" "sx1262.c" "radio_esp.c" "tx_gate.c" "tx_gate_esp.c"
             "phy_passthrough.c"
        INCLUDE_DIRS "include"
        REQUIRES driver esp_driver_spi esp_driver_gpio freertos esp_timer board_config esp_system airtime
    )
endif()
