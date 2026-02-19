if(CONFIG_BRAMBLE_TOUCH_ENABLED)
    set(touch_srcs "touch_gt911.c")
else()
    set(touch_srcs "")
endif()

idf_component_register(
    SRCS ${touch_srcs}
    INCLUDE_DIRS "include"
    REQUIRES driver board_config keyboard
)
