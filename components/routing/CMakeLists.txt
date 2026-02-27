idf_component_register(
    SRCS "routing.c" "discovery.c" "beacon.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES packet crypto
)
