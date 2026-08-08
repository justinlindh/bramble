if(CONFIG_BRAMBLE_UI_GRAPHICAL)
    set(ui_gfx_srcs
        "ui_graphics.c"
        "lv_port_display.c"
        "lv_port_touch.c"
        "lv_port_trackball.c"
        "lv_port_keyboard.c"
        "sleep_manager.c"
        "theme/bramble_theme.c"
        "screens/scr_splash.c"
        "screens/scr_layout.c"
        "screens/scr_chat_list.c"
        "screens/scr_chat_messages.c"
        "screens/scr_chat_compose.c"
        "screens/scr_channel_create.c"
        "screens/scr_nodes.c"
        "screens/scr_node_detail.c"
        "screens/scr_sas_verify.c"
        "screens/ui_shared_state.c"
        "screens/scr_map.c"
        "screens/scr_stats.c"
        "screens/scr_traffic.c"
        "screens/scr_settings.c"
        "screens/scr_settings_identity.c"
        "screens/scr_settings_device.c"
        "screens/scr_settings_location.c"
        "screens/scr_settings_channels.c"
        "screens/scr_settings_radio.c"
        "screens/scr_settings_connectivity.c"
        "chat_target.c"
        "chat_unread.c"
        "chat_message_ui.c"
        "node_presence.c"
        "map_zoom.c"
        "ui_toast.c"
        "ui_confirm.c"
        "ui_pairing.c"
        "ui_focus.c"
        "ui_zone.c"
        "assets/img_bramble_logo.c"
    )
else()
    set(ui_gfx_srcs "")
endif()

idf_component_register(
    SRCS ${ui_gfx_srcs}
    INCLUDE_DIRS "include" "."
    REQUIRES nvs_keys nvs_flash display keyboard trackball board_config bramble_touch
             channel msg_store identity radio battery audio
             location gps rpc airtime wifi ble ui timezone
    PRIV_REQUIRES lvgl esp_app_format traffic_debug packet
)
