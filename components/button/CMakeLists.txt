# The POSIX/Linux simulator has no GPIO driver: on the IDF linux target the
# virtual driver button_virt.c implements button.h, turning emu-link `btn`
# messages into ui_button_t events.
idf_build_get_property(target IDF_TARGET)
if(${target} STREQUAL "linux")
    idf_component_register(
        SRCS "button_virt.c"
        INCLUDE_DIRS "include"
        REQUIRES ui board_config emu_link json
    )
else()
    idf_component_register(
        SRCS "button.c"
        INCLUDE_DIRS "include"
        REQUIRES driver esp_driver_gpio ui board_config
    )
endif()
