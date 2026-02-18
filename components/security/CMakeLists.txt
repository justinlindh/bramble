idf_component_register(
    SRCS "security.c" "dummy_traffic.c"
    INCLUDE_DIRS "include"
    REQUIRES crypto packet
)
