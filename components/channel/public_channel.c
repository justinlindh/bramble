#include "include/public_channel.h"
#include <string.h>

int public_channel_init(bramble_channel_t* channels, int* num_channels) {
    if (!channels || !num_channels)
        return -1;
    int ret =
        channel_derive_key(BRAMBLE_PUBLIC_CHANNEL_PSK, &channels[BRAMBLE_PUBLIC_CHANNEL_INDEX]);
    if (ret != 0)
        return ret;
    /* Public channel ID is protocol-fixed at index 0 (don't use hash-derived channel_id). */
    channels[BRAMBLE_PUBLIC_CHANNEL_INDEX].channel_id = BRAMBLE_PUBLIC_CHANNEL_INDEX;
    if (*num_channels < 1)
        *num_channels = 1;
    return 0;
}
