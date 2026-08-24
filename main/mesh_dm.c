/**
 * mesh_dm.c: DM handshake, session, and ratchet plumbing (SEC-C2).
 *
 * Owns the INIT/RESP handshake state machine, the session table and its
 * per-message ratchet, the queue that holds DMs until a session establishes,
 * and the proactive rekey schedule. Shared state and cross-module entry
 * points come from mesh_internal.h.
 */
#include "mesh_internal.h"

#include <string.h>

static const char* TAG = "mesh";

/* Forward declarations for intra-module static helpers. */
static bool dm_rehandshake_rate_ok(uint32_t peer, uint32_t now);
static uint32_t send_dm_packet(uint32_t dest_addr, const uint8_t* payload, size_t payload_len,
                               dm_session_t* sess);
static uint32_t send_ke_envelope(uint32_t dest_addr, int channel_idx,
                                 const bramble_key_exchange_t* ke);
static int dm_session_has_peer_id(const dm_session_t* s);
static void pending_eph_store(uint32_t peer_addr, const uint8_t eph_priv[32],
                              const uint8_t eph_pub[32]);
static dm_pending_eph_t* pending_eph_lookup(uint32_t peer_addr);
static void pending_eph_clear(uint32_t peer_addr);
static int hs_dedup_check_and_record(uint32_t src_addr, const uint8_t eph_pub[32],
                                     uint16_t ke_epoch, uint32_t now);
static uint32_t queue_session_message(uint32_t dest_addr, const uint8_t* data, size_t len,
                                      int channel_idx, uint32_t uid);
static void flush_session_queue(uint32_t dest_addr);
static void initiate_dm_handshake(uint32_t dest_addr, int channel_idx, uint16_t rekey_epoch);
static void process_ke_init(uint32_t src_addr, int channel_idx, const bramble_key_exchange_t* init,
                            const uint8_t* pinned_x25519_or_null);
static void process_ke_resp(uint32_t src_addr, const bramble_key_exchange_t* resp,
                            const uint8_t* pinned_x25519_or_null);

/* Self-heal for a desynced DM session. handle_data calls this when it cannot
 * decrypt a directed DM from a peer: the peer's session has diverged from ours
 * (typically it rebooted and lost the session while we kept our stale one), so
 * every DM it sends would fail forever with no recovery. Re-initiate the
 * handshake so DMs recover on their own. Guards against abuse: only for a peer
 * we currently neighbor with, and at most once per interval per peer, so a
 * spray of undecryptable packets cannot be turned into a re-key / airtime DoS.
 * The DH-heavy INIT is queued to handshake_worker_task, never run on this (mesh
 * RX) task, the same rule process_ke_init/resp follow. */
#define DM_REHANDSHAKE_MIN_INTERVAL_MS 15000u
#define DM_REHANDSHAKE_TRACK 8
static struct {
    uint32_t addr;
    uint32_t last_ms;
} s_dm_rehs[DM_REHANDSHAKE_TRACK];

static bool dm_rehandshake_rate_ok(uint32_t peer, uint32_t now) {
    int free_slot = -1, oldest = 0;
    for (int i = 0; i < DM_REHANDSHAKE_TRACK; i++) {
        if (s_dm_rehs[i].addr == peer) {
            if ((uint32_t)(now - s_dm_rehs[i].last_ms) < DM_REHANDSHAKE_MIN_INTERVAL_MS)
                return false;
            s_dm_rehs[i].last_ms = now;
            return true;
        }
        if (s_dm_rehs[i].addr == 0 && free_slot < 0)
            free_slot = i;
        if (s_dm_rehs[i].last_ms < s_dm_rehs[oldest].last_ms)
            oldest = i;
    }
    int slot = (free_slot >= 0) ? free_slot : oldest;
    s_dm_rehs[slot].addr = peer;
    s_dm_rehs[slot].last_ms = now;
    return true;
}

void maybe_trigger_dm_rehandshake(uint32_t peer) {
    if (peer == 0 || peer == s_identity->address)
        return;
    if (!neighbor_lookup(&s_neighbors, peer))
        return; /* only real, currently-neighboring peers */
    if (!dm_rehandshake_rate_ok(peer, now_ms()))
        return;
    dm_handshake_work_item_t item;
    memset(&item, 0, sizeof(item));
    item.src_addr = peer;
    item.channel_idx = s_default_channel_idx;
    item.initiate = true;
    if (xQueueSend(s_handshake_work_q, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Rehandshake queue full, self-heal dropped for %08" PRIX32, peer);
        return;
    }
    ESP_LOGI(TAG, "DM session desync with %08" PRIX32 "; re-initiating handshake (self-heal)",
             peer);
}

/* Proactive DH-ratchet rekey schedule (post-compromise recovery). A
 * session bumps to the next ke_epoch after DM_EPOCH_REKEY_MSGS messages sent OR
 * DM_EPOCH_REKEY_INTERVAL_MS elapsed on the current epoch, whichever comes
 * first. These bound the PCS latency (a device compromise heals within one
 * schedule tick) and trade it against rekey airtime; they are tuning constants,
 * NOT security boundaries, and are flagged for the review gate. Kept
 * conservative so steady-state DM traffic and short-lived test sessions do not
 * churn handshakes. */
#define DM_EPOCH_REKEY_MSGS 256u
#define DM_EPOCH_REKEY_INTERVAL_MS (30u * 60u * 1000u)

/* Scans the session table and, for any ACTIVE ratcheting session that is due
 * (and whose peer is a current neighbor), schedules ONE proactive rekey INIT via
 * the handshake worker. Only the lower-addressed party initiates, so the two
 * peers do not both fire a rekey and duel; a lost rekey simply leaves both on
 * the current epoch (never a dead session). Rate-limited by the same per-peer
 * guard as the desync-heal, so a scheduled rekey and a self-heal cannot combine
 * into an airtime DoS. Runs on the mesh task (the s_dm_table owner). */
void maybe_schedule_dm_epoch_rekey(uint32_t t) {
    for (int i = 0; i < DM_MAX_SESSIONS; i++) {
        uint32_t peer = 0;
        uint16_t next_epoch = 0;
        xSemaphoreTake(s_dm_mutex, portMAX_DELAY);
        dm_session_t* s = &s_dm_table->s[i];
        if (s->state == DM_STATE_ACTIVE && s->ratchet.send.valid && s->ke_epoch < 0xFFFF &&
            s_identity->address < s->peer_addr) {
            bool due = (s->msg_count >= DM_EPOCH_REKEY_MSGS) ||
                       ((uint32_t)(t - s->established_ms) >= DM_EPOCH_REKEY_INTERVAL_MS);
            if (due) {
                peer = s->peer_addr;
                next_epoch = (uint16_t)(s->ke_epoch + 1);
            }
        }
        DM_MUTEX_GIVE();
        if (peer == 0)
            continue;
        if (!neighbor_lookup(&s_neighbors, peer))
            continue; /* only rekey with a currently-reachable peer */
        if (!dm_rehandshake_rate_ok(peer, t))
            continue;
        dm_handshake_work_item_t item;
        memset(&item, 0, sizeof(item));
        item.src_addr = peer;
        item.channel_idx = s_default_channel_idx;
        item.initiate = true;
        item.rekey_epoch = next_epoch;
        if (xQueueSend(s_handshake_work_q, &item, 0) == pdTRUE) {
            ESP_LOGI(TAG, "DM proactive rekey for %08" PRIX32 " -> epoch %u", peer, next_epoch);
        }
    }
}

size_t mesh_dm_session_capacity(void) { return (size_t)DM_MAX_SESSIONS; }

/* Read-only snapshot of the used session slots, for diagnostics that need to
 * answer "does this peer have a session". Takes s_dm_mutex fresh per slot
 * rather than holding it across the whole sweep, matching the discipline of
 * maybe_schedule_dm_epoch_rekey above: the caller runs on the RPC task, and a
 * sweep held across 32 slots would stall the mesh task's send path for no
 * benefit. A slot that changes state mid-sweep is reported as it was when read,
 * which is all a point-in-time snapshot ever promises.
 *
 * Copies metadata only. dm_session_t carries session_key, peer_id_pub and the
 * ratchet chain keys; none of those are copied out. */
size_t mesh_get_dm_sessions(mesh_dm_session_info_t* out, size_t max) {
    if (!out || max == 0)
        return 0;

    uint32_t t = now_ms();
    size_t written = 0;

    for (int i = 0; i < DM_MAX_SESSIONS && written < max; i++) {
        mesh_dm_session_info_t info;
        memset(&info, 0, sizeof(info));

        xSemaphoreTake(s_dm_mutex, portMAX_DELAY);
        const dm_session_t* s = &s_dm_table->s[i];
        if (s->state != DM_STATE_NONE) {
            info.peer_addr = s->peer_addr;
            info.established_ms_ago = (uint32_t)(t - s->established_ms);
            info.last_active_ms_ago = (uint32_t)(t - s->last_active_ms);
            info.msg_count = s->msg_count;
            info.ke_epoch = s->ke_epoch;
            info.state = (s->state == DM_STATE_ACTIVE) ? MESH_DM_SESSION_ACTIVE
                                                       : MESH_DM_SESSION_HANDSHAKING;
            info.verified = (s->verified != 0);
            info.ratchet_valid = (s->ratchet.send.valid != 0);
        }
        DM_MUTEX_GIVE();

        if (info.state == MESH_DM_SESSION_NONE)
            continue;
        out[written++] = info;
    }

    return written;
}

/*
 * SEC-C2: sends a chat payload under an ESTABLISHED session key
 * (dm_session_ratchet_encrypt), FLAG_ENCRYPT WITHOUT FLAG_CHANNEL. This is the DM
 * PAYLOAD path; it never falls back to the channel key. Caller MUST already hold
 * s_dm_mutex (this function reads/writes *sess, which lives inside s_dm_table)
 * and must have already checked sess->state == DM_STATE_ACTIVE. Mirrors
 * send_data_packet's nonce/TX/pending_ack handling exactly, minus the channel_msg
 * framing (a session is 1:1, so there is no channel_id/ epoch/app_type to
 * multiplex; the wire layout is header+src_addr+nonce+ ciphertext+tag same as the
 * channel path, just under a different key and without FLAG_CHANNEL, which is
 * exactly the signal handle_data uses to pick the decrypt path on the other end).
 *
 */
static uint32_t send_dm_packet(uint32_t dest_addr, const uint8_t* payload, size_t payload_len,
                               dm_session_t* sess) {
    uint8_t ciphertext[BRAMBLE_MAX_PACKET_SIZE];
    uint8_t nonce[BRAMBLE_NONCE_SIZE];
    uint8_t tag[BRAMBLE_TAG_SIZE];

    /* The ratchet frames each DM as (3-byte cleartext header || ciphertext), so
     * the on-wire ciphertext is DM_RATCHET_HEADER_SIZE bytes longer than the chat
     * payload; account for it in the size gate (mesh_send_dm's FRAG_MAX_PLAINTEXT
     * cap already leaves room). */
    size_t total = BRAMBLE_DATA_ENVELOPE_PREFIX_SIZE + BRAMBLE_NONCE_SIZE + DM_RATCHET_HEADER_SIZE +
                   payload_len + BRAMBLE_TAG_SIZE;
    if (total > 255) {
        ESP_LOGE(TAG, "DM packet too large: %u bytes", (unsigned)total);
        return 0;
    }

    uint8_t buf[255];
    uint32_t pkt_id = next_packet_id();
    bramble_header_t header = {
        .version = BRAMBLE_VERSION,
        .type = PKT_TYPE_DATA,
        .flags = FLAG_ENCRYPT, /* no FLAG_CHANNEL: session-keyed DM (SEC-C2) */
        .hop_limit = flood_origination_hop_limit(s_flood_transport, s_flood_hop_limit),
        .dest_addr = dest_addr,
        .packet_id = pkt_id,
    };

    bramble_header_serialize(&header, buf, HEADER_SIZE);

    xSemaphoreTake(s_nonce_mutex, portMAX_DELAY);
    int nonce_ret = nonce_counter_next(nonce);
    xSemaphoreGive(s_nonce_mutex);
    if (nonce_ret != 0) {
        ESP_LOGE(TAG, "Nonce counter unavailable, dropping DM send: %d", nonce_ret);
        return 0;
    }

    size_t framed_len = 0;
    if (dm_session_ratchet_encrypt(sess, &header, s_identity->address, payload, payload_len, nonce,
                                   ciphertext, tag, &framed_len) != 0) {
        ESP_LOGE(TAG, "Session encrypt failed for %08" PRIX32, dest_addr);
        return 0;
    }

    memcpy(buf + BRAMBLE_DATA_SRC_ADDR_OFFSET, &s_identity->address, 4);
    /* Wire v4: ORIGINATOR writes its own address as prev_hop; see
     * send_data_packet's identical comment. */
    memcpy(buf + BRAMBLE_DATA_PREV_HOP_OFFSET, &s_identity->address, 4);
    /* Wire v4: origin-authenticate; see send_data_packet. An unprovisioned node
     * has no key to sign with, so the send aborts rather than emit an
     * unauthenticated DM. */
    if (data_auth_sign(&header, s_identity->address, buf + BRAMBLE_DATA_AUTH_HMAC_OFFSET) != 0) {
        ESP_LOGD(TAG, "unprovisioned: inert, dropping DM send");
        return 0;
    }
    memcpy(buf + BRAMBLE_DATA_NONCE_OFFSET, nonce, BRAMBLE_NONCE_SIZE);
    memcpy(buf + BRAMBLE_DATA_NONCE_OFFSET + BRAMBLE_NONCE_SIZE, ciphertext, framed_len);
    memcpy(buf + BRAMBLE_DATA_NONCE_OFFSET + BRAMBLE_NONCE_SIZE + framed_len, tag,
           BRAMBLE_TAG_SIZE);

    int ret = mesh_tx(buf, (uint8_t)total, TX_KIND_DATA);
    if (ret == TX_GATE_OK) {
        pending_ack_add(&s_pending_acks, pkt_id, dest_addr, MSG_TIER_NORMAL, buf, (uint16_t)total,
                        now_ms());
        sess->msg_count++;
        sess->last_active_ms = now_ms(); /* real activity, not eviction bait */
        return pkt_id;
    }
    return 0;
}

/*
 * SEC-C2: sends a handshake message (INIT or RESP) as an
 * APP_TYPE_KE inner payload of a DATA envelope under the CHANNEL key. This
 * is the handshake TRANSPORT (no session exists yet by definition), which
 * is why it reuses send_data_packet unmodified rather than send_dm_packet.
 */
static uint32_t send_ke_envelope(uint32_t dest_addr, int channel_idx,
                                 const bramble_key_exchange_t* ke) {
    if (channel_idx < 0 || channel_idx >= s_num_channels) {
        ESP_LOGE(TAG, "send_ke_envelope: invalid channel index %d", channel_idx);
        return 0;
    }
    uint8_t wire[KEY_EXCHANGE_SIZE];
    if (bramble_key_exchange_serialize(ke, wire, sizeof(wire)) != ESP_OK) {
        ESP_LOGE(TAG, "KE envelope serialize failed");
        return 0;
    }
    return send_data_packet(dest_addr, wire, sizeof(wire), &s_channels[channel_idx], APP_TYPE_KE);
}

/* Public identity-key caching heuristic: peer_id_pub is a public value (sent
 * in the clear as long_term_pubkey on every handshake message), so a plain
 * early-exit scan leaks nothing secret; it is not a tag/key comparison.
 * dm_alloc memsets a fresh or evicted slot, so all-zero reliably means "no
 * identity cached here yet" for any slot this node has allocated. */
static int dm_session_has_peer_id(const dm_session_t* s) {
    if (!s)
        return 0;
    for (int i = 0; i < 32; i++) {
        if (s->peer_id_pub[i] != 0)
            return 1;
    }
    return 0;
}

static void pending_eph_store(uint32_t peer_addr, const uint8_t eph_priv[32],
                              const uint8_t eph_pub[32]) {
    int free_idx = -1;
    for (int i = 0; i < DM_MAX_HANDSHAKING; i++) {
        if (s_pending_eph[i].used && s_pending_eph[i].peer_addr == peer_addr) {
            free_idx = i;
            break;
        }
        if (free_idx < 0 && !s_pending_eph[i].used)
            free_idx = i;
    }
    if (free_idx < 0) {
        /* Table sized to DM_MAX_HANDSHAKING, same cap dm_alloc enforces for
         * HANDSHAKING slots, so this should never actually trip: dm_alloc
         * would have already refused a new handshake before this is called. */
        ESP_LOGW(TAG, "Pending ephemeral table full, dropping entry for %08" PRIX32, peer_addr);
        return;
    }
    s_pending_eph[free_idx].peer_addr = peer_addr;
    memcpy(s_pending_eph[free_idx].eph_priv, eph_priv, 32);
    memcpy(s_pending_eph[free_idx].eph_pub, eph_pub, 32);
    s_pending_eph[free_idx].used = true;
}

static dm_pending_eph_t* pending_eph_lookup(uint32_t peer_addr) {
    for (int i = 0; i < DM_MAX_HANDSHAKING; i++) {
        if (s_pending_eph[i].used && s_pending_eph[i].peer_addr == peer_addr) {
            return &s_pending_eph[i];
        }
    }
    return NULL;
}

static void pending_eph_clear(uint32_t peer_addr) {
    for (int i = 0; i < DM_MAX_HANDSHAKING; i++) {
        if (s_pending_eph[i].used && s_pending_eph[i].peer_addr == peer_addr) {
            /* Holds eph_priv, an ephemeral X25519 private key: secure wipe, not
             * memset, for the dead-store elision hazard crypto.h documents. */
            crypto_secure_wipe(&s_pending_eph[i], sizeof(s_pending_eph[i]));
            return;
        }
    }
}

/*
 * Handshake dedup (SEC-C2). Returns 1 (dup, caller must drop without
 * reprocessing) or 0 (fresh, recorded so the next identical INIT dedups).
 * eph_pub_hash is a plain truncated SHA-256 over the ephemeral pubkey (a
 * public value): this is a dedup cache key, not an authentication tag, so
 * no HMAC/constant-time comparison is needed.
 */
static int hs_dedup_check_and_record(uint32_t src_addr, const uint8_t eph_pub[32],
                                     uint16_t ke_epoch, uint32_t now) {
    uint8_t digest[32] = {0};
    (void)crypto_sha256(eph_pub, 32, digest);
    uint32_t eph_hash;
    memcpy(&eph_hash, digest, 4);

    int lru = 0;
    for (int i = 0; i < DM_HS_DEDUP_MAX; i++) {
        dm_hs_dedup_entry_t* e = &s_hs_dedup[i];
        if (e->used && e->src_addr == src_addr && e->eph_pub_hash == eph_hash &&
            e->ke_epoch == ke_epoch) {
            return 1;
        }
        if (!e->used) {
            lru = i;
            break;
        }
        if (e->seen_ms < s_hs_dedup[lru].seen_ms)
            lru = i;
    }
    s_hs_dedup[lru].src_addr = src_addr;
    s_hs_dedup[lru].eph_pub_hash = eph_hash;
    s_hs_dedup[lru].ke_epoch = ke_epoch;
    s_hs_dedup[lru].seen_ms = now;
    s_hs_dedup[lru].used = true;
    return 0;
}

/*
 * Queue-and-trigger: queues a DM payload awaiting session establishment
 * and assigns it a real, trackable packet_id up front (unlike the
 * awaiting-route queue_message, which has no onAck story at all). Queue
 * pressure evicts the oldest QUEUE_REASON_SESSION entry with the same
 * visible onAck failure a TTL expiry gets, rather than dropping the new
 * send silently; if every slot is a QUEUE_REASON_ROUTE entry, this send
 * fails visibly instead (a route-awaiting entry belongs to a different
 * subsystem's queue and is not evicted here).
 *
 * Known limitation (documented, not fixed here): the pkt_id returned here
 * is a tracking placeholder, not the pkt_id that eventually appears on the
 * wire once the session establishes and flush_session_queue actually calls
 * send_dm_packet (which mints its own pkt_id via next_packet_id, matching
 * every other send_*_packet in this file). A caller polling status by the
 * placeholder id will not see the real send's ack/delivery events; it will
 * see a failure notification via rerr_fastfail_notify if the queue entry
 * expires or is evicted, and nothing further if it is flushed successfully.
 */
static uint32_t queue_session_message(uint32_t dest_addr, const uint8_t* data, size_t len,
                                      int channel_idx, uint32_t uid) {
    /* At most one entry per message, the same rule queue_message states in
     * full: a uid names one row, the payload for a row never changes, and a
     * second entry for it means the drain sends one written message twice.
     * Returning the existing entry's pkt_id keeps the caller's contract, since
     * that is the id this message is already being tracked under, and leaves
     * the original entry's TTL alone. uid 0 is untracked and exempt.
     *
     * The match deliberately spans both reasons rather than only session
     * entries, because a route entry and a session entry for one uid would
     * each be drained by their own path and send it twice. The cost is that a
     * uid still held as a route entry reports failure here (a route entry
     * carries no pkt_id), which is a spurious failure report and never a
     * duplicate: the row stays parked and the route entry still goes out. */
    if (uid != 0) {
        for (int i = 0; i < MAX_QUEUED_MSGS; i++) {
            if (s_queued_msgs[i].used && s_queued_msgs[i].uid == uid) {
                ESP_LOGD(TAG, "uid %" PRIu32 " already queued for %08" PRIX32, uid, dest_addr);
                return s_queued_msgs[i].pkt_id;
            }
        }
    }
    int free_idx = -1;
    for (int i = 0; i < MAX_QUEUED_MSGS; i++) {
        if (!s_queued_msgs[i].used) {
            free_idx = i;
            break;
        }
    }
    if (free_idx < 0) {
        int oldest = -1;
        for (int i = 0; i < MAX_QUEUED_MSGS; i++) {
            if (s_queued_msgs[i].used && s_queued_msgs[i].reason == QUEUE_REASON_SESSION &&
                (oldest < 0 || s_queued_msgs[i].timestamp < s_queued_msgs[oldest].timestamp)) {
                oldest = i;
            }
        }
        if (oldest < 0) {
            ESP_LOGW(TAG,
                     "Message queue full (no evictable session entry), failing DM for %08" PRIX32,
                     dest_addr);
            return 0;
        }
        ESP_LOGW(TAG, "Message queue full, evicting oldest awaiting-session entry for %08" PRIX32,
                 s_queued_msgs[oldest].dest_addr);
        rerr_fastfail_notify(s_queued_msgs[oldest].pkt_id, "queue_full");
        msg_store_update_by_uid(s_queued_msgs[oldest].uid, 0, MSG_STATUS_FAILED);
        free_idx = oldest;
    }

    uint32_t pkt_id = next_packet_id();
    s_queued_msgs[free_idx].dest_addr = dest_addr;
    memcpy(s_queued_msgs[free_idx].data, data, len);
    s_queued_msgs[free_idx].len = len;
    s_queued_msgs[free_idx].timestamp = now_ms();
    s_queued_msgs[free_idx].reason = QUEUE_REASON_SESSION;
    s_queued_msgs[free_idx].pkt_id = pkt_id;
    s_queued_msgs[free_idx].uid = uid;
    s_queued_msgs[free_idx].channel_idx = (int16_t)channel_idx;
    s_queued_msgs[free_idx].used = true;
    ESP_LOGI(TAG, "Queued DM for %08" PRIX32 " (awaiting session)", dest_addr);
    return pkt_id;
}

/* Sends every QUEUE_REASON_SESSION entry for dest_addr now that a session
 * is ACTIVE. Takes s_dm_mutex fresh per entry (not held across the whole
 * loop) so a long flush never blocks handle_ke_envelope/handshake_worker_task
 * for longer than a single send. */
static void flush_session_queue(uint32_t dest_addr) {
    for (int i = 0; i < MAX_QUEUED_MSGS; i++) {
        if (!s_queued_msgs[i].used || s_queued_msgs[i].reason != QUEUE_REASON_SESSION ||
            s_queued_msgs[i].dest_addr != dest_addr) {
            continue;
        }

        xSemaphoreTake(s_dm_mutex, portMAX_DELAY);
        dm_session_t* sess = dm_lookup(s_dm_table, dest_addr);
        uint32_t pkt_id = 0;
        if (sess && sess->state == DM_STATE_ACTIVE) {
            pkt_id = send_dm_packet(dest_addr, s_queued_msgs[i].data, s_queued_msgs[i].len, sess);
        }
        DM_MUTEX_GIVE();

        if (pkt_id != 0) {
            ESP_LOGI(TAG, "Flushed queued DM to %08" PRIX32 " (%u bytes)", dest_addr,
                     (unsigned)s_queued_msgs[i].len);
            /* A QUEUE_REASON_SESSION entry is always a DM (only DMs establish a
             * session), so store it channel-less exactly like the direct-send path
             * in mesh_send_dm. Passing the transport channel_idx here would file
             * the DM under that channel and hide it from its own thread; the
             * msg_store_add_dm* API takes no channel index, so the mistake is
             * unrepresentable.
             *
             * The row already exists (stored pending when the message was queued),
             * so stamp the real wire packet_id onto THAT row: the ACK correlates
             * by packet_id and lands on the one row this message owns. Adding here
             * is the fallback for a row evicted from the ring meanwhile, never the
             * normal path. */
            if (!msg_store_update_by_uid(s_queued_msgs[i].uid, pkt_id, MSG_STATUS_SENT)) {
                msg_store_add_dm_uid(dest_addr, MSG_DIR_OUTGOING,
                                     (const char*)s_queued_msgs[i].data, s_queued_msgs[i].len, 0, 0,
                                     pkt_id, MSG_STATUS_SENT, s_queued_msgs[i].uid);
            }
        } else {
            ESP_LOGW(TAG, "Failed to flush queued DM to %08" PRIX32, dest_addr);
            rerr_fastfail_notify(s_queued_msgs[i].pkt_id, "session_send_failed");
            msg_store_update_by_uid(s_queued_msgs[i].uid, 0, MSG_STATUS_FAILED);
        }
        s_queued_msgs[i].used = false;
    }
}

/*
 * Sends a DM handshake INIT (SEC-C2 handshake transport, under the channel key
 * via send_ke_envelope). rekey_epoch == 0 is the first-contact / desync-heal
 * path (key_id 0, no peer identity in the tag). rekey_epoch > 0 is a proactive
 * DH-ratchet rekey: it reuses the SAME INIT/RESP machinery with
 * key_id = rekey_epoch and the cached peer X25519 identity (the dm_build_init
 * rekey path), so both sides land on the new epoch's root. A rekey requires an
 * ACTIVE session (for the cached peer_id_pub); if it has vanished, fall back to
 * a first-contact INIT rather than stranding the peer.
 */
static void initiate_dm_handshake(uint32_t dest_addr, int channel_idx, uint16_t rekey_epoch) {
    bramble_identity_t my_eph;
    /* Fail closed (SEC-L1): crypto_generate_identity returns -1 without touching
     * the key material when the entropy gate is shut or the curve mult fails.
     * Using my_eph unchecked would put uninitialized stack on the wire as an
     * ephemeral public key. No handshake is strictly better than a weak one. */
    if (crypto_generate_identity(&my_eph) != 0) {
        ESP_LOGE(TAG, "Ephemeral keygen failed, INIT to %08" PRIX32 " aborted", dest_addr);
        crypto_secure_wipe(&my_eph, sizeof(my_eph));
        return;
    }

    uint8_t peer_id_pub[32] = {0}; /* only read when have_peer_id (guards cppcheck uninitvar) */
    int have_peer_id = 0;
    if (rekey_epoch > 0) {
        xSemaphoreTake(s_dm_mutex, portMAX_DELAY);
        dm_session_t* sess = dm_lookup(s_dm_table, dest_addr);
        if (sess && sess->state == DM_STATE_ACTIVE) {
            memcpy(peer_id_pub, sess->peer_id_pub, 32);
            have_peer_id = 1;
        }
        DM_MUTEX_GIVE();
        if (!have_peer_id)
            rekey_epoch = 0; /* no cached peer key: degrade to first contact */
    }

    bramble_key_exchange_t init;
    if (dm_build_init(s_identity, my_eph.public_key, my_eph.private_key, dest_addr, rekey_epoch,
                      have_peer_id ? peer_id_pub : NULL, &init) != 0) {
        ESP_LOGE(TAG, "dm_build_init failed for %08" PRIX32, dest_addr);
        crypto_secure_wipe(&my_eph, sizeof(my_eph));
        return;
    }

    pending_eph_store(dest_addr, my_eph.private_key, my_eph.public_key);
    /* pending_eph_store owns the copy the RESP will be verified against; this
     * stack identity (X25519 private key plus a 64-byte Ed25519 private key) is
     * dead from here. crypto_secure_wipe, not memset, so the compiler cannot
     * elide the wipe as a dead store to an about-to-go-out-of-scope object. */
    crypto_secure_wipe(&my_eph, sizeof(my_eph));

    if (send_ke_envelope(dest_addr, channel_idx, &init) == 0) {
        ESP_LOGW(TAG, "Failed to send INIT to %08" PRIX32, dest_addr);
    }
}

/*
 * SEC-C2 queue-and-trigger: the ONLY place a unicast DM decides
 * between the session path and queue-and-handshake. NEVER falls back to
 * the channel key for a DM payload: an ACTIVE session sends via
 * send_dm_packet; anything else queues and (if not already handshaking)
 * triggers an INIT, or fails visibly if the handshaking cap is reached.
 */
uint32_t mesh_send_dm(int channel_idx, uint32_t dest_addr, const uint8_t* data, size_t len,
                      uint32_t uid) {
    if (len > FRAG_MAX_PLAINTEXT) {
        /* Fragmentation under a session key is out of this task's scope
         * (DM chat payloads are short); fail visibly rather than silently
         * truncating or falling back to the channel key. */
        ESP_LOGW(TAG, "DM payload too large for the session path: %u bytes", (unsigned)len);
        msg_store_update_by_uid(uid, 0, MSG_STATUS_FAILED);
        return 0;
    }

    xSemaphoreTake(s_dm_mutex, portMAX_DELAY);
    dm_session_t* sess = dm_lookup(s_dm_table, dest_addr);
    if (sess && sess->state == DM_STATE_ACTIVE) {
        uint32_t pkt_id = send_dm_packet(dest_addr, data, len, sess);
        DM_MUTEX_GIVE();
        if (pkt_id != 0) {
            /* channel_idx only picked the transport channel the session's KE
             * envelope rode on; the payload is a DM. msg_store_add_dm files it
             * channel-less, so it can never land under channel 0 (the unicast
             * default) and hide from its own thread, exactly like a received DM.
             *
             * uid == 0 is the direct happy path (this is the first and only
             * stage to see the message): store its one row. uid != 0 means an
             * earlier stage already stored the row: update it in place. */
            if (!msg_store_update_by_uid(uid, pkt_id, MSG_STATUS_SENT)) {
                msg_store_add_dm_uid(dest_addr, MSG_DIR_OUTGOING, (const char*)data, len, 0, 0,
                                     pkt_id, MSG_STATUS_SENT, uid ? uid : msg_store_next_uid());
            }
        } else {
            msg_store_update_by_uid(uid, 0, MSG_STATUS_FAILED);
        }
        return pkt_id;
    }

    bool handshake_in_progress = sess && sess->state == DM_STATE_HANDSHAKING;
    dm_session_t* hs = sess;
    if (!hs) {
        hs = dm_alloc(s_dm_table, dest_addr, now_ms());
        if (hs)
            hs->state = DM_STATE_HANDSHAKING;
    }
    DM_MUTEX_GIVE();

    if (!hs) {
        /* M4 DoS defense: handshaking cap reached. Fail the send visibly;
         * never transmit this payload under the channel key instead. */
        ESP_LOGW(TAG, "No session and handshaking cap reached for %08" PRIX32, dest_addr);
        msg_store_update_by_uid(uid, 0, MSG_STATUS_FAILED);
        return 0;
    }

    uint32_t row_uid = (uid != 0) ? uid : msg_store_next_uid();
    uint32_t pkt_id = queue_session_message(dest_addr, data, len, channel_idx, row_uid);
    if (pkt_id == 0) {
        msg_store_update_by_uid(uid, 0, MSG_STATUS_FAILED);
        return 0;
    }

    if (!handshake_in_progress) {
        initiate_dm_handshake(dest_addr, channel_idx, 0);
    }

    /* Store the pending row ONLY for a brand new message (uid == 0). When
     * uid != 0 the awaiting-route stage already stored this message's row, and
     * flush_session_queue reconciles that same row on transmit: storing again
     * here is exactly what produced duplicate bubbles. Either way the queue
     * entry carries row_uid. packet_id stays 0 until the real send, because
     * queue_session_message's pkt_id is a caller-facing tracking placeholder
     * that never reaches the wire, so it must not be written into the row an
     * ACK is matched against. A queued DM is channel-less. */
    if (uid == 0) {
        msg_store_add_dm_uid(dest_addr, MSG_DIR_OUTGOING, (const char*)data, len, 0, 0, 0,
                             MSG_STATUS_NONE, row_uid);
    }
    return pkt_id;
}

/*
 * Responder side of the INIT/RESP state machine (runs on
 * handshake_worker_task, never inline on the mesh RX loop). Downgrade
 * defense: have_peer_id is derived from whatever s_dm_table already
 * holds for src_addr at the moment of the check, so a zero-tag INIT can
 * never be accepted as first-contact against an already-known identity.
 *
 */
static void process_ke_init(uint32_t src_addr, int channel_idx, const bramble_key_exchange_t* init,
                            const uint8_t* pinned_x25519_or_null) {
    xSemaphoreTake(s_dm_mutex, portMAX_DELAY);
    dm_session_t* existing = dm_lookup(s_dm_table, src_addr);
    int have_peer_id = dm_session_has_peer_id(existing);
    /* Zero-init: peer_id_pub is only read (passed below) when have_peer_id is
     * set, and it is filled here in exactly that case, so no uninitialized
     * read occurs. The initializer makes that invariant explicit and silences
     * a cppcheck uninitvar warning it cannot otherwise prove. */
    uint8_t peer_id_pub[32] = {0};
    if (have_peer_id)
        memcpy(peer_id_pub, existing->peer_id_pub, 32);
    DM_MUTEX_GIVE();

    int vrc = dm_verify_init(init, s_identity, have_peer_id, have_peer_id ? peer_id_pub : NULL,
                             pinned_x25519_or_null);
    if (vrc == DM_VERIFY_ERR_PIN_MISMATCH) {
        /* DM key continuity RED FLAG: this address has an
         * attestation-verified pinned X25519 key and the handshake showed
         * up with a DIFFERENT one. Refuse the session loudly; a silent
         * accept here would let a keyed insider splice itself into a
         * known peer's DMs. */
        ESP_LOGW(TAG,
                 "DM KEY CONTINUITY: INIT from %08" PRIX32 " does not match its pinned identity"
                 " key, session REFUSED",
                 src_addr);
        return;
    }
    if (vrc != 0) {
        /* One-sided desync recovery (DM self-heal PART 2). We hold a cached
         * peer_id for src_addr (have_peer_id) so we took dm_verify_init's
         * strict tag path, yet the INIT is from an ATTESTATION-PINNED peer:
         * pinned_x25519_or_null is set and, since we are past the
         * PIN_MISMATCH gate above, the INIT's long_term_pubkey MATCHED it.
         * So this is cryptographically that peer re-initiating fresh (it
         * rebooted / lost its half of the session, e.g. after the receiver
         * self-heal triggers a first-contact INIT). Tear our stale session
         * down and re-accept as first contact so DMs heal instead of failing
         * forever. Rate-limited: long_term_pubkey is public, so a spoofed
         * INIT naming a pinned peer must not be spammable into a
         * session-teardown DoS; the worst case is one bounded teardown that
         * the next genuine handshake repairs (the resulting session is only
         * usable by whoever can complete the DH, i.e. the real peer). */
        if (have_peer_id && pinned_x25519_or_null && dm_rehandshake_rate_ok(src_addr, now_ms())) {
            xSemaphoreTake(s_dm_mutex, portMAX_DELAY);
            dm_session_teardown(s_dm_table, src_addr);
            DM_MUTEX_GIVE();
            ESP_LOGI(TAG,
                     "DM session desync: pinned peer %08" PRIX32 " re-initiated; tore down stale"
                     " session, re-accepting as first contact (self-heal)",
                     src_addr);
            vrc = dm_verify_init(init, s_identity, 0, NULL, pinned_x25519_or_null);
        }
        if (vrc != 0) {
            ESP_LOGW(TAG, "INIT verify failed from %08" PRIX32, src_addr);
            return;
        }
    }

    bramble_identity_t my_eph;
    /* Fail closed (SEC-L1), same rationale as initiate_dm_handshake: an
     * unchecked failure here would answer the INIT with uninitialized stack as
     * our ephemeral public key. Drop the handshake instead; the peer retries. */
    if (crypto_generate_identity(&my_eph) != 0) {
        ESP_LOGE(TAG, "Ephemeral keygen failed, RESP to %08" PRIX32 " aborted", src_addr);
        crypto_secure_wipe(&my_eph, sizeof(my_eph));
        return;
    }
    uint16_t ke_epoch = (uint16_t)init->key_id;

    bramble_key_exchange_t resp;
    uint8_t session_key[32];
    if (dm_build_resp(s_identity, my_eph.public_key, my_eph.private_key, init, ke_epoch, &resp,
                      session_key) != 0) {
        ESP_LOGE(TAG, "dm_build_resp failed for %08" PRIX32, src_addr);
        crypto_secure_wipe(&my_eph, sizeof(my_eph));
        return;
    }

    /* Recompute the 128-byte handshake IKM to seed the ratchet chains: the same
     * quad-DH schedule dm_build_resp used internally, so RK_0 (ikm epoch 0) is
     * bit-identical to session_key. ikm[0:32] is the fresh ephemeral-ephemeral
     * DH, which is the new_dh a rekey folds into the root. */
    uint8_t ikm[128];
    int ikm_ok = dm_compute_ikm(s_identity->private_key, my_eph.private_key, init->long_term_pubkey,
                                init->ephemeral_pubkey, ikm);
    /* Last use of the ephemeral private key: wipe before the mutex section so
     * every path out of the rest of this function leaves no copy on the stack. */
    crypto_secure_wipe(&my_eph, sizeof(my_eph));

    xSemaphoreTake(s_dm_mutex, portMAX_DELAY);
    dm_session_t* sess = dm_alloc(s_dm_table, src_addr, now_ms());
    if (sess) {
        /* Rekey (DH ratchet) iff we already hold an ACTIVE ratcheting session
         * for this peer and the INIT names a higher epoch: chain the existing
         * root forward with the fresh DH (dm_session_epoch_bump), keeping the old
         * recv chain for the grace window. Otherwise (first contact or a
         * desync-heal re-handshake, ke_epoch resets to 0) install a fresh
         * ratchet from the new IKM. */
        int is_rekey = (ikm_ok == 0 && sess->state == DM_STATE_ACTIVE && sess->ratchet.recv.valid &&
                        ke_epoch > sess->ke_epoch);
        memcpy(sess->peer_id_pub, init->long_term_pubkey, 32);
        sess->established_ms = now_ms();
        sess->msg_count = 0;
        sess->state = DM_STATE_ACTIVE;
        /* Source verified from the persisted pin, not a hardcoded reset: the
         * verified bit keys on the pinned identity key (identity_store.h), so
         * a previously-verified peer re-establishes as verified across
         * reboot / desync-heal / epoch bump alike. */
        sess->verified = identity_store_is_verified(&s_identity_pins, src_addr) ? 1 : 0;
        /* A failed ratchet derivation wipes the session's chains and reports
         * -1; fold it into ikm_ok so it takes the existing "ratchet not seeded"
         * error path below instead of leaving a session that looks usable. */
        if (is_rekey) {
            if (dm_session_epoch_bump(sess, ikm, s_identity->address, src_addr, ke_epoch) != 0)
                ikm_ok = -1;
        } else {
            memcpy(sess->session_key, session_key, 32);
            sess->ke_epoch = ke_epoch;
            if (ikm_ok == 0 &&
                dm_session_ratchet_init_state(sess, ikm, s_identity->address, src_addr) != 0)
                ikm_ok = -1;
        }
    }
    DM_MUTEX_GIVE();
    /* crypto_secure_wipe, not memset: the IKM is root-key material and a plain
     * memset on a soon-dead buffer is elidable as a dead store (crypto.h). */
    crypto_secure_wipe(ikm, sizeof(ikm));

    if (!sess) {
        ESP_LOGW(TAG, "No DM session slot available to establish session with %08" PRIX32,
                 src_addr);
        return;
    }
    if (ikm_ok != 0) {
        ESP_LOGE(TAG, "IKM recompute failed for %08" PRIX32 ", ratchet not seeded", src_addr);
        return;
    }

    if (send_ke_envelope(src_addr, channel_idx, &resp) == 0) {
        ESP_LOGW(TAG, "Failed to send RESP to %08" PRIX32, src_addr);
    }

    flush_session_queue(src_addr);
}

/*
 * Initiator side: verifies a RESP against the ephemeral we generated when
 * we sent the matching INIT (dm_pending_eph_t; dm_session_t itself has no
 * field for in-flight handshake material, see its declaration above).
 */
static void process_ke_resp(uint32_t src_addr, const bramble_key_exchange_t* resp,
                            const uint8_t* pinned_x25519_or_null) {
    dm_pending_eph_t* pe = pending_eph_lookup(src_addr);
    if (!pe) {
        ESP_LOGW(TAG, "RESP from %08" PRIX32 " with no matching pending INIT", src_addr);
        return;
    }

    uint16_t ke_epoch = (uint16_t)resp->key_id;
    uint8_t session_key[32];
    int vrc = dm_verify_resp(resp, s_identity, pe->eph_priv, pe->eph_pub, ke_epoch,
                             pinned_x25519_or_null, session_key);
    if (vrc == DM_VERIFY_ERR_PIN_MISMATCH) {
        /* Same red flag as process_ke_init: pinned peer, different DM key. */
        ESP_LOGW(TAG,
                 "DM KEY CONTINUITY: RESP from %08" PRIX32 " does not match its pinned identity"
                 " key, session REFUSED",
                 src_addr);
        return;
    }
    if (vrc != 0) {
        ESP_LOGW(TAG, "RESP verify failed from %08" PRIX32, src_addr);
        return;
    }

    /* Same IKM recompute as process_ke_init, from the initiator's viewpoint: our
     * pending ephemeral + the responder's. ikm[0:32] is the fresh eph-eph DH (the
     * rekey new_dh); RK_0 equals session_key at epoch 0. Both sides compute the
     * identical IKM, so the ratchet chains agree. */
    uint8_t ikm[128];
    int ikm_ok = dm_compute_ikm(s_identity->private_key, pe->eph_priv, resp->long_term_pubkey,
                                resp->ephemeral_pubkey, ikm);

    xSemaphoreTake(s_dm_mutex, portMAX_DELAY);
    /* dm_alloc, not dm_lookup: a desync self-heal (maybe_trigger_dm_rehandshake ->
     * initiate_dm_handshake) stores a pending eph but no session slot, unlike the
     * outgoing-DM path where mesh_send_dm pre-allocs a HANDSHAKING slot. This RESP is
     * already verified against our own pending INIT (pending_eph_lookup above) and the
     * attestation pin (dm_verify_resp's PIN_MISMATCH gate), so allocate the slot now and
     * complete as first contact, mirroring the responder (process_ke_init also dm_allocs).
     * For the outgoing and rekey paths the slot already exists, so dm_alloc returns it. */
    dm_session_t* sess = dm_alloc(s_dm_table, src_addr, now_ms());
    if (sess) {
        int is_rekey = (ikm_ok == 0 && sess->state == DM_STATE_ACTIVE && sess->ratchet.recv.valid &&
                        ke_epoch > sess->ke_epoch);
        memcpy(sess->peer_id_pub, resp->long_term_pubkey, 32);
        sess->established_ms = now_ms();
        sess->msg_count = 0;
        sess->state = DM_STATE_ACTIVE;
        /* Same rationale as process_ke_init: source from the persisted pin. */
        sess->verified = identity_store_is_verified(&s_identity_pins, src_addr) ? 1 : 0;
        /* A failed ratchet derivation wipes the session's chains and reports
         * -1; fold it into ikm_ok so it takes the existing "ratchet not seeded"
         * error path below instead of leaving a session that looks usable. */
        if (is_rekey) {
            if (dm_session_epoch_bump(sess, ikm, s_identity->address, src_addr, ke_epoch) != 0)
                ikm_ok = -1;
        } else {
            memcpy(sess->session_key, session_key, 32);
            sess->ke_epoch = ke_epoch;
            if (ikm_ok == 0 &&
                dm_session_ratchet_init_state(sess, ikm, s_identity->address, src_addr) != 0)
                ikm_ok = -1;
        }
    }
    DM_MUTEX_GIVE();
    /* Same dead-store hazard as process_ke_init: wipe, do not memset. */
    crypto_secure_wipe(ikm, sizeof(ikm));

    pending_eph_clear(src_addr);

    if (!sess) {
        ESP_LOGW(TAG, "No DM session slot available to complete RESP session with %08" PRIX32,
                 src_addr);
        return;
    }
    if (ikm_ok != 0) {
        ESP_LOGE(TAG, "IKM recompute failed for %08" PRIX32 ", ratchet not seeded", src_addr);
        return;
    }

    flush_session_queue(src_addr);
}

/* M7 offload: drains handshake work items posted by handle_ke_envelope.
 * Low priority, small stack: the only work here is the occasional
 * four-X25519-mult handshake, never on the mesh RX critical path. */
void handshake_worker_task(void* arg) {
    (void)arg;
    dm_handshake_work_item_t item;
    for (;;) {
        if (xQueueReceive(s_handshake_work_q, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (item.initiate) {
            /* DM self-heal (rekey_epoch 0): our session with this peer desynced
             * (we could not decrypt its DM), so re-initiate a fresh handshake off
             * the RX task. rekey_epoch > 0: a scheduled proactive DH-ratchet
             * rekey to the next epoch. Both reuse the same INIT/RESP machinery. */
            initiate_dm_handshake(item.src_addr, item.channel_idx, item.rekey_epoch);
            continue;
        }
        const uint8_t* pinned = item.have_pin ? item.pinned_x25519 : NULL;
        if (item.msg.ke_type == KE_TYPE_INIT) {
            process_ke_init(item.src_addr, item.channel_idx, &item.msg, pinned);
        } else if (item.msg.ke_type == KE_TYPE_RESP) {
            process_ke_resp(item.src_addr, &item.msg, pinned);
        }
    }
}

/*
 * RX entry point for APP_TYPE_KE inner payloads (SEC-C2 handshake-in-DATA).
 * Does only cheap parsing/validation here; the DH-heavy INIT/RESP state
 * machine runs on handshake_worker_task. src_addr is the OUTER DATA
 * envelope's already-authenticated src_addr (from channel_msg_decrypt's AAD
 * binding), not yet trusted to equal the inner struct's own claimed
 * src_addr until checked below.
 */
void handle_ke_envelope(uint32_t src_addr, int channel_idx, const uint8_t* data, size_t data_len) {
    bramble_key_exchange_t msg;
    if (bramble_key_exchange_deserialize(&msg, data, data_len) != ESP_OK) {
        ESP_LOGW(TAG, "Malformed KE envelope from %08" PRIX32, src_addr);
        return;
    }
    if (msg.ke_type != KE_TYPE_INIT && msg.ke_type != KE_TYPE_RESP) {
        ESP_LOGW(TAG, "Unknown ke_type %u from %08" PRIX32, msg.ke_type, src_addr);
        return;
    }
    /* Outer/inner src_addr consistency: closes a confusion vector where a
     * valid channel-key sender embeds a KE payload claiming a different
     * address than the one the outer envelope's AAD already authenticated. */
    if (msg.src_addr != src_addr) {
        ESP_LOGW(TAG, "KE envelope src_addr mismatch: outer=%08" PRIX32 " inner=%08" PRIX32,
                 src_addr, msg.src_addr);
        return;
    }
    /* Reject self-addressed (role-confusion-at-dispatch defense). */
    if (src_addr == s_identity->address) {
        ESP_LOGW(TAG, "Dropping self-addressed KE envelope");
        return;
    }

    if (msg.ke_type == KE_TYPE_INIT) {
        uint16_t ke_epoch = (uint16_t)msg.key_id;
        if (hs_dedup_check_and_record(src_addr, msg.ephemeral_pubkey, ke_epoch, now_ms())) {
            ESP_LOGD(TAG, "Duplicate INIT from %08" PRIX32 ", not re-running handshake", src_addr);
            return;
        }
    }

    dm_handshake_work_item_t item;
    memset(&item, 0, sizeof(item));
    item.src_addr = src_addr;
    item.channel_idx = channel_idx;
    item.msg = msg;
    /* DM key continuity: snapshot the pinned X25519 key for this
     * peer HERE, on the mesh task (the only mutator of s_identity_pins),
     * so the handshake worker verifies against an immutable copy instead
     * of reading the pin store cross-thread. No pin = NULL downstream =
     * TOFU-grade first contact (stated residual until the peer's
     * attestation is heard and pinned). */
    const identity_pin_t* pin = identity_store_lookup(&s_identity_pins, src_addr);
    if (pin) {
        item.have_pin = true;
        memcpy(item.pinned_x25519, pin->x25519_pub, sizeof(item.pinned_x25519));
    }
    if (xQueueSend(s_handshake_work_q, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Handshake work queue full, dropping KE from %08" PRIX32, src_addr);
    }
}

/* Emulator only: tear down THIS node's DM sessions, leaving the peer's half
 * intact and this node still neighboring it. That is exactly the one-sided
 * "stale session" desync the emu-dm-desync scenario exists to reproduce.
 * Dropping the session in-process, rather than rebooting the receiver to clear
 * its RAM-held sessions, keeps three real-time races out of CI (RAM-clear
 * timing, beacon re-acquisition of the peer, and the sender's stale DM landing
 * inside that window): the desynced state is constructed at an exact
 * scenario-driven instant with the peer never lost as a neighbor, so the very
 * next session DM from the peer deterministically fails to decrypt here and
 * fires the desync self-heal. Returns the number of sessions torn down. Runs on
 * the emu_autosend task; s_dm_mutex serializes it against the mesh RX task
 * exactly like every other s_dm_table access. */
/* Emulator only: how many DM sessions this node currently holds in any
 * non-NONE state. The drop task (emu_autosend.c) polls it so the desync
 * inject waits for the session it is about to drop, instead of trusting a
 * subjective-time schedule that CI tick-loss skew can outrun. */
#ifdef CONFIG_IDF_TARGET_LINUX
int emu_mesh_dm_session_count(void) {
    int count = 0;
    xSemaphoreTake(s_dm_mutex, portMAX_DELAY);
    for (int i = 0; i < DM_MAX_SESSIONS; i++) {
        if (s_dm_table->s[i].state != DM_STATE_NONE)
            count++;
    }
    DM_MUTEX_GIVE();
    return count;
}

int emu_mesh_drop_dm_sessions(void) {
    int dropped = 0;
    for (int i = 0; i < DM_MAX_SESSIONS; i++) {
        uint32_t peer;
        xSemaphoreTake(s_dm_mutex, portMAX_DELAY);
        peer = (s_dm_table->s[i].state != DM_STATE_NONE) ? s_dm_table->s[i].peer_addr : 0;
        if (peer != 0)
            dm_session_teardown(s_dm_table, peer);
        DM_MUTEX_GIVE();
        if (peer != 0) {
            ESP_LOGI(TAG, "emu: dropped DM session with %08" PRIX32 " (one-sided desync inject)",
                     peer);
            /* Reboot-faithful drop: a RAM clear also loses the retransmit and
             * awaiting-session state, so purge both. Leaving them makes the
             * constructed desync racy: an undelivered delivery receipt for the
             * peer keeps retransmitting after the drop, its expiry requeues it
             * "awaiting session", and THIS node then initiates a fresh
             * handshake, replacing the peer's stale session half before the
             * scenario's stale-session DM can land and fire the
             * decrypt-failure symptom. Cross-task access to these tables
             * follows the existing discipline of the send paths, which already
             * run on arbitrary caller tasks. */
            size_t acks =
                rerr_ack_failfast_for_dest(&s_pending_acks, peer, "emu_desync_inject", NULL);
            int queued = 0;
            for (int q = 0; q < MAX_QUEUED_MSGS; q++) {
                if (s_queued_msgs[q].used && s_queued_msgs[q].dest_addr == peer) {
                    s_queued_msgs[q].used = false;
                    queued++;
                }
            }
            ESP_LOGI(TAG,
                     "emu: purged %u pending ack(s) and %d queued msg(s) for %08" PRIX32
                     " (reboot-faithful drop)",
                     (unsigned)acks, queued, peer);
            dropped++;
        }
    }
    return dropped;
}
#endif
