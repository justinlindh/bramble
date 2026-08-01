# The POSIX/Linux simulator has no ADC: on the IDF linux target the virtual
# driver battery_virt.c implements battery.h, serving VBAT millivolts from
# emu-link `batt` messages.
idf_build_get_property(target IDF_TARGET)
if(${target} STREQUAL "linux")
    idf_component_register(
        SRCS "battery_virt.c" "battery_pct.c" "battery_helpers.c" "battery_wrappers.c"
        INCLUDE_DIRS "include"
        REQUIRES board_config emu_link json
    )
else()
    idf_component_register(
        SRCS "battery.c" "battery_pct.c" "battery_helpers.c" "battery_wrappers.c"
        INCLUDE_DIRS "include"
        REQUIRES esp_adc board_config
    )
endif()
