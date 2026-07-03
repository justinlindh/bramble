#ifndef BRAMBLE_REPLAY_WINDOW_H
#define BRAMBLE_REPLAY_WINDOW_H
#include <stdint.h>

#define REPLAY_MAX_SENDERS 64
#define REPLAY_ACCEPT 0
#define REPLAY_REJECT_DUP 1
#define REPLAY_BELOW_WINDOW 2

typedef struct {
    uint32_t src_addr;
    uint64_t high_water;
    uint64_t window;       /* bitmap of the 64 positions below high_water */
    uint32_t last_seen_ms;
    uint8_t  used;
    uint8_t  seen;          /* has this slot's sender ever had a packet accepted?
                              * Distinct from high_water==0, which is a legitimate
                              * counter value (the nonce counter's first-boot
                              * value), not just "nothing seen yet". */
} replay_slot_t;

typedef struct { replay_slot_t slots[REPLAY_MAX_SENDERS]; } replay_table_t;

void replay_table_init(replay_table_t* t);
int replay_check_and_add(replay_table_t* t, uint32_t src_addr, uint64_t counter, uint32_t now_ms);
#endif
