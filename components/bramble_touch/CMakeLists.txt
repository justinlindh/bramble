if(CONFIG_BRAMBLE_TOUCH_ENABLED)
    set(touch_srcs "touch_gt911.c")
else()
    set(touch_srcs "")
endif()

# The POSIX/Linux simulator has no driver component (touch hardware is
# T-Deck-only anyway): headers only there.
idf_build_get_property(target IDF_TARGET)
if(${target} STREQUAL "linux")
    idf_component_register(INCLUDE_DIRS "include" REQUIRES board_config keyboard)
else()
    idf_component_register(
        SRCS ${touch_srcs}
        INCLUDE_DIRS "include"
        REQUIRES driver board_config keyboard
    )
endif()
