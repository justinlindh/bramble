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
} chat_target_kind_t;

typedef struct {
    chat_target_kind_t kind;
    int16_t channel_index; /* -1 when not applicable */
} chat_target_t;

chat_target_t chat_target_default(void);

chat_target_t chat_target_normalize(chat_target_kind_t kind,
                                    int channel_index,
                                    int channel_count);

bool chat_target_matches_message(chat_target_t target,
                                 const stored_msg_t *msg,
                                 int message_channel_index);

#ifdef __cplusplus
}
#endif

#endif /* CHAT_TARGET_H */
