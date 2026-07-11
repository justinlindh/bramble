# trackball.c already compiles to its non-T-Deck stub half on other boards;
# on the POSIX/Linux simulator only the driver requirement must go (the
# driver component does not exist there).
idf_build_get_property(target IDF_TARGET)
if(${target} STREQUAL "linux")
    idf_component_register(
        SRCS "trackball.c"
        INCLUDE_DIRS "include"
        REQUIRES board_config ui
    )
else()
    idf_component_register(
        SRCS "trackball.c"
        INCLUDE_DIRS "include"
        REQUIRES driver board_config ui
    )
endif()
