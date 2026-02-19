if(CONFIG_BRAMBLE_UI_GRAPHICAL)
    set(SRCS
        "ui_graphics.c"
        "lv_port_display.c"
    )

    idf_component_register(
        SRCS ${SRCS}
        INCLUDE_DIRS "include"
        REQUIRES display keyboard trackball board_config
                 channel msg_store identity radio battery
                 location gps rpc airtime wifi ble
        PRIV_REQUIRES lvgl
    )
else()
    idf_component_register()
endif()
