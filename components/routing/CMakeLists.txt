idf_component_register(
    SRCS "routing.c" "discovery.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES packet
)
