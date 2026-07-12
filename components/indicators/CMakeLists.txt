# indicators: the pager's alert outputs (LED / buzzer / vibra). On-device
# (indicators.c) is a trivial GPIO/LEDC wrapper; on the IDF linux target the
# virtual driver indicator_virt.c emits emu-link `ind` state events. The gate
# mirrors components/gps and components/button: real driver on the device,
# virtual driver on linux.
idf_build_get_property(target IDF_TARGET)
if(${target} STREQUAL "linux")
    idf_component_register(
        SRCS "indicator_virt.c" "alerts.c"
        INCLUDE_DIRS "include"
        REQUIRES emu_link json
    )
else()
    idf_component_register(
        SRCS "indicators.c" "alerts.c"
        INCLUDE_DIRS "include"
        REQUIRES driver esp_driver_gpio esp_driver_ledc
    )
endif()
