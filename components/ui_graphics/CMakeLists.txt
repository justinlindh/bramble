if(CONFIG_BRAMBLE_UI_GRAPHICAL)
    set(SRCS
        "ui_graphics.c"
        "lv_port_display.c"
        "lv_port_touch.c"
        "lv_port_trackball.c"
        "lv_port_keyboard.c"
        "theme/bramble_theme.c"
        "screens/scr_layout.c"
        "screens/scr_chat_list.c"
        "screens/scr_chat_messages.c"
        "screens/scr_nodes.c"
        "screens/scr_stats.c"
    )

    idf_component_register(
        SRCS ${SRCS}
        INCLUDE_DIRS "include" "."
        REQUIRES display keyboard trackball board_config touch
                 channel msg_store identity radio battery
                 location gps rpc airtime wifi ble ui
        PRIV_REQUIRES lvgl
    )
else()
    idf_component_register()
endif()
