#ifndef BRAMBLE_MESH_DM_SESSION_INFO_H
#define BRAMBLE_MESH_DM_SESSION_INFO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * The observable shape of one DM session slot.
 *
 * Deliberately kept in its own dependency-free header rather than inside
 * mesh_task.h: the host test stubs mirror mesh_task.h's heavier types by hand
 * and cannot include it, and a hand-mirrored copy of this struct is exactly the
 * kind of silent layout drift that has bitten those stubs before. One
 * definition, included by both the firmware and the stubs.
 */

typedef enum {
    MESH_DM_SESSION_NONE = 0,
    MESH_DM_SESSION_HANDSHAKING = 1,
    MESH_DM_SESSION_ACTIVE = 2,
} mesh_dm_session_state_t;

/**
 * Metadata for one used session slot.
 *
 * NOT a copy of dm_session_t: that struct carries the session key, the peer's
 * cached X25519 key and the ratchet chain keys, none of which may leave the
 * firmware. This is the observable state a diagnostic needs, which is whether a
 * peer has a usable session and how fresh it is.
 */
typedef struct {
    uint32_t peer_addr;
    uint32_t established_ms_ago; /* since the slot was (re)established */
    uint32_t last_active_ms_ago; /* since the last send or receive */
    uint32_t msg_count;
    uint16_t ke_epoch;
    mesh_dm_session_state_t state;
    bool verified;      /* peer identity pinned and confirmed */
    bool ratchet_valid; /* send chain established */
} mesh_dm_session_info_t;

#endif /* BRAMBLE_MESH_DM_SESSION_INFO_H */
