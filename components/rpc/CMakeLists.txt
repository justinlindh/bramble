idf_component_register(
    SRCS "rpc_dispatcher.c" "rpc_auth.c"
    INCLUDE_DIRS "include"
    REQUIRES json packet routing dedup identity channel crypto airtime freq_plan
)
