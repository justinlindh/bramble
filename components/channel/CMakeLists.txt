idf_component_register(
    SRCS "channel_key.c" "channel_msg.c" "public_channel.c" "channel_storage.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_keys crypto nvs_flash packet
)
