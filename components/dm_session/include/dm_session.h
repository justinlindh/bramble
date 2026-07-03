#ifndef BRAMBLE_DM_SESSION_H
#define BRAMBLE_DM_SESSION_H
#include <stdint.h>
#include "crypto.h"

int dm_derive_session_key(const uint8_t my_id_priv[32], const uint8_t my_eph_priv[32],
                          const uint8_t peer_id_pub[32], const uint8_t peer_eph_pub[32],
                          uint32_t addr_a, uint32_t addr_b, uint16_t ke_epoch,
                          uint8_t session_key_out[32]);
int dm_derive_sas(const uint8_t session_ikm[128], char sas_out[8]);

/*
 * DM session table (Task 1.2). State-priority LRU with a bounded
 * DM_STATE_HANDSHAKING count: this is the DoS defense for the session
 * layer (M4 in the crypto design RFC). A spoofed-INIT flood can occupy at
 * most DM_MAX_HANDSHAKING slots regardless of table size, and an
 * ACTIVE session (verified or not; "UNVERIFIED" == state==ACTIVE &&
 * verified==0, still protected) can never be evicted to make room for a
 * new handshake.
 */
#define DM_MAX_SESSIONS 32
#define DM_MAX_HANDSHAKING (DM_MAX_SESSIONS / 4)

#define DM_STATE_NONE 0
#define DM_STATE_HANDSHAKING 1
#define DM_STATE_ACTIVE 2

typedef struct {
    uint32_t peer_addr;
    uint8_t  session_key[32];
    uint8_t  peer_id_pub[32];  /* cached for rekey-path msg1 auth + SAS */
    uint32_t established_ms;   /* also doubles as last-activity for LRU ordering */
    uint32_t msg_count;
    uint16_t ke_epoch;
    uint8_t  state;
    uint8_t  verified;
} dm_session_t;

typedef struct { dm_session_t s[DM_MAX_SESSIONS]; } dm_table_t;

void dm_table_init(dm_table_t* t);

/* Returns the used slot for peer_addr, or NULL if none. */
dm_session_t* dm_lookup(dm_table_t* t, uint32_t peer_addr);

/*
 * Returns a slot for peer_addr: an existing slot for that peer if one
 * already exists (no cap check; not a new handshake), else a free
 * (DM_STATE_NONE) slot, else the oldest slot (by established_ms) that is
 * NEITHER DM_STATE_ACTIVE NOR UNVERIFIED (state==ACTIVE && verified==0) is
 * LRU-evicted (in practice this means only a DM_STATE_HANDSHAKING slot is
 * ever evictable, since ACTIVE covers UNVERIFIED as a subset). Returns NULL
 * if a brand-new slot (free or evicted) would push the table's
 * DM_STATE_HANDSHAKING count past DM_MAX_HANDSHAKING, or if the table is
 * full with no evictable slot. The caller is responsible for transitioning
 * a freshly-returned slot's state (typically to DM_STATE_HANDSHAKING)
 * immediately; dm_alloc itself only initializes peer_addr/established_ms.
 */
dm_session_t* dm_alloc(dm_table_t* t, uint32_t peer_addr, uint32_t now_ms);
#endif
