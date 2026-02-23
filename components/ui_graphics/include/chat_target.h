#ifndef CHAT_TARGET_H
#define CHAT_TARGET_H

#include <stdbool.h>
#include <stdint.h>
#include "msg_store.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CHAT_TARGET_BROADCAST = 0,
    CHAT_TARGET_CHANNEL = 1,
    CHAT_TARGET_DM = 2,
} chat_target_kind_t;

typedef struct {
    chat_target_kind_t kind;
    int16_t channel_index; /* valid for CHANNEL; -1 otherwise */
    uint32_t peer_addr;    /* valid for DM; 0 otherwise */
} chat_target_t;

chat_target_t chat_target_default(void);

chat_target_t chat_target_normalize(chat_target_kind_t kind,
                                    int channel_index,
                                    int channel_count);

chat_target_t chat_target_dm(uint32_t peer_addr);

bool chat_target_matches_message(chat_target_t target,
                                 const stored_msg_t *msg,
                                 int message_channel_index);

/* Cycle target: broadcast -> channel 1..N-1 -> broadcast */
chat_target_t chat_target_cycle(chat_target_t current, int channel_count);

#ifdef __cplusplus
}
#endif

#endif /* CHAT_TARGET_H */
