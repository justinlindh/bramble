# On the POSIX/Linux simulator the SX1262 driver cannot exist (no SPI/GPIO
# drivers); only the hardware-independent sources build there. The radio.h
# implementation comes from the emulator's driver component (null_drivers for
# the spike, radio_virt later); tx_gate_esp.c is kept so the real TX gate
# logic runs on host.
idf_build_get_property(target IDF_TARGET)
if(${target} STREQUAL "linux")
    idf_component_register(
        SRCS "radio_airtime.c" "tx_gate.c" "tx_gate_esp.c"
        INCLUDE_DIRS "include"
        REQUIRES freertos esp_timer board_config esp_system esp_hw_support airtime
    )
else()
    idf_component_register(
        SRCS "radio_airtime.c" "sx1262.c" "radio_esp.c" "tx_gate.c" "tx_gate_esp.c"
        INCLUDE_DIRS "include"
        REQUIRES driver esp_driver_spi esp_driver_gpio freertos esp_timer board_config esp_system airtime
    )
endif()
