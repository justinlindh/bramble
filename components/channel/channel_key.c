#include "include/channel_key.h"
#include <string.h>

int channel_derive_key(const char* psk, bramble_channel_t* ch) {
    uint8_t psk_hash[32];
    if (crypto_sha256((const uint8_t*)psk, strlen(psk), psk_hash) != 0)
        return -1;

    const char* salt = "bramble-channel-v1";
    uint8_t info = 0x00;
    if (crypto_hkdf_sha256((const uint8_t*)salt, strlen(salt), psk_hash, 32, &info, 1, ch->key,
                           32) != 0)
        return -1;

    uint8_t key_hash[32];
    crypto_sha256(ch->key, 32, key_hash);
    ch->channel_id = key_hash[0] % 16;
    ch->epoch = 0;
    return 0;
}

int channel_advance_epoch(bramble_channel_t* ch) {
    const char* salt = "bramble-channel-epoch";
    uint8_t info[2];
    uint16_t next = ch->epoch + 1;
    info[0] = (next >> 8) & 0xFF;
    info[1] = next & 0xFF;

    uint8_t new_key[32];
    if (crypto_hkdf_sha256((const uint8_t*)salt, strlen(salt), ch->key, 32, info, 2, new_key, 32) !=
        0)
        return -1;
    memcpy(ch->key, new_key, 32);
    ch->epoch = next;
    return 0;
}
