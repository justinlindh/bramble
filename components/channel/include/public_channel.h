#ifndef BRAMBLE_PUBLIC_CHANNEL_H
#define BRAMBLE_PUBLIC_CHANNEL_H

#include "channel_key.h"
#include <stdint.h>

#define BRAMBLE_PUBLIC_CHANNEL_INDEX 0
#define BRAMBLE_PUBLIC_CHANNEL_NAME "Bramble Common"
#define BRAMBLE_PUBLIC_CHANNEL_HOP_LIMIT 3

/* Initialize Channel 0 with well-known PSK. Call on boot. */
int public_channel_init(bramble_channel_t* channels, int* num_channels);

/*
 * Fix 2 (red-team panel, post-Task-3.6): decides whether a decrypt's
 * src_addr is trustworthy enough to feed the shared per-sender replay
 * window (components/replay_window). BRAMBLE_PUBLIC_CHANNEL_PSK is known
 * to literally everyone, not just channel members, so a packet decrypted
 * under the public channel can claim ANY src_addr the sender wants: an
 * attacker forges one with src_addr=victim to slam the victim's shared
 * high-water mark, causing the victim's own later, genuine packets to
 * read BELOW_WINDOW and drop (a mesh-wide DoS on location/chat). A
 * SECRET channel's src_addr costs at least channel membership (insider
 * forgery is the accepted symmetric-auth residual, not this bug); a
 * DM/session decrypt's src_addr costs a negotiated session key.
 *
 * is_channel_message MUST be false for DM/session decrypts: those leave
 * channel_index at its zero default (mesh_task.c memsets the info struct
 * for the session path), the SAME value as BRAMBLE_PUBLIC_CHANNEL_INDEX,
 * so checking channel_index alone would misclassify every DM as public.
 */
static inline int channel_source_is_replay_trustworthy(int is_channel_message, int channel_index) {
    if (!is_channel_message)
        return 1;
    return channel_index != BRAMBLE_PUBLIC_CHANNEL_INDEX;
}

#endif
