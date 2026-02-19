if(CONFIG_BRAMBLE_TOUCH_ENABLED)
    idf_component_register(
        SRCS "touch_gt911.c"
        INCLUDE_DIRS "include"
        REQUIRES driver board_config
    )
else()
    idf_component_register()
endif()
