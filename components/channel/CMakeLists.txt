idf_component_register(
    SRCS "channel_key.c" "channel_msg.c" "public_channel.c"
    INCLUDE_DIRS "include"
    REQUIRES crypto
)
