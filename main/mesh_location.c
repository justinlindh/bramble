/**
 * mesh_location.c: Location share TX/RX, peer-location persistence, and the policy tick.
 *
 * Split out of mesh_task.c (issue #86); pure code motion, no behavior change.
 * Shared state and cross-module entry points come from mesh_internal.h.
 */
#include "mesh_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvs.h"
#include "nvs_keys.h"
#include "cJSON.h"
#include "rpc_dispatcher.h"

static const char* TAG = "mesh";

typedef struct __attribute__((packed)) {
    int32_t latitude_e7;
    int32_t longitude_e7;
    int16_t altitude_m;
    uint8_t accuracy_m;
    uint8_t speed_kmh;
    uint8_t heading_deg2;
    uint32_t timestamp;
    uint32_t received_ms;
    uint8_t tier;
} persisted_peer_location_t;

/* A channel target is identified by its index into the channel table, so the
 * key space this header reserves has to cover every channel the node can
 * hold. location.h keeps the literal (it stays free of the channel
 * component); this is where the two are held to each other. */
_Static_assert(LOCATION_MAX_CHANNEL_TARGETS >= MAX_CHANNELS,
               "location channel-target key space must cover every channel");
_Static_assert(LOCATION_PUBLIC_CHANNEL_INDEX == BRAMBLE_PUBLIC_CHANNEL_INDEX,
               "location must reject the same index the channel component calls public");

/* How often the share policy is evaluated. Every pass reads the location NVS
 * namespace, and the shortest interval a target can be configured with is
 * LOCATION_MIN_INTERVAL_S, so evaluating faster than this only adds NVS
 * traffic on the mesh task. It also bounds how long a configuration change
 * takes to reach the radio. */
#define LOCATION_POLICY_TICK_MS 5000U

/* Enabled share targets resolved from NVS for one round. */
typedef struct {
    location_target_t items[LOCATION_MAX_TARGETS];
    size_t count;
    size_t skipped;
} location_target_set_t;

/* Per-target send pacing. Lives here rather than in NVS: it is scheduling
 * state, not configuration, and a reboot legitimately re-shares. */
static location_schedule_t s_location_schedule;
/* Handshake pacing for directed targets with no session. Owned solely by the
 * mesh task. The backoff is cleared from the share round itself, on observing
 * that a session now exists, rather than from the DM state transitions: those
 * run on handshake_worker_task, and hooking them would make this table
 * cross-task shared state needing a lock it does not otherwise want. */
static location_hs_table_t s_location_hs;

/* Forward declarations for intra-module static helpers. */
static void location_policy_load_or_defaults(nvs_handle_t nvs, location_policy_t* policy);
static void location_collect_targets(location_target_set_t* out);
static bool location_build_inner(const bramble_position_t* pos, uint8_t tier, uint8_t* inner);
static uint32_t location_tx_directed(uint32_t dest_addr, const uint8_t* inner, uint8_t tier,
                                     bool* needs_session);
static uint32_t location_tx_channel(int channel_idx, const uint8_t* inner, uint8_t tier);
static void mesh_emit_location_event(const char* event, uint32_t peer_addr, uint8_t tier,
                                     uint32_t timestamp_ms, int16_t rssi, int8_t snr,
                                     uint32_t count);
static void mesh_send_location_updates(uint32_t t, const location_policy_t* policy,
                                       const bramble_position_t* source_pos,
                                       const location_target_set_t* targets);
static void mesh_persist_peer_location(uint32_t peer_addr, const bramble_position_t* pos,
                                       uint8_t tier, uint32_t now_ms);
static int location_rx_decode_channel(const uint8_t* nonce, const uint8_t* ciphertext,
                                      size_t ct_len, const uint8_t* tag, const uint8_t* aad,
                                      size_t aad_len, uint8_t* tier_out,
                                      bramble_position_t* pos_out, int* channel_index_out);

static void location_policy_load_or_defaults(nvs_handle_t nvs, location_policy_t* policy) {
    location_policy_set_defaults(policy);

    uint8_t enabled = 0;
    if (nvs_get_u8(nvs, "enabled", &enabled) == ESP_OK) {
        policy->enabled = (enabled != 0);
    }

    uint16_t interval_s = 0;
    if (nvs_get_u16(nvs, "interval_s", &interval_s) == ESP_OK) {
        policy->interval_s = interval_s;
    }

    char tier[16] = {0};
    size_t tier_len = sizeof(tier);
    if (nvs_get_str(nvs, "def_tier", tier, &tier_len) == ESP_OK) {
        policy->default_tier = location_tier_from_string(tier);
    }

    location_policy_normalize(policy);
}

/*
 * Read every enabled share target out of the location namespace in one pass.
 *
 * This is the COLLECT half of the two-phase structure documented at
 * mesh_send_location_updates: it opens NVS, iterates, and closes, and it
 * transmits nothing. Both target kinds come out of the same iteration, so
 * adding a channel target cannot be accepted by the config surface and then
 * quietly ignored by the sender.
 */
static void location_collect_targets(location_target_set_t* out) {
    out->count = 0;
    out->skipped = 0;

    nvs_handle_t nvs;
    if (nvs_open(NVS_NS_LOCATION, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }

    nvs_iterator_t it = NULL;
    if (nvs_entry_find(NVS_PARTITION, NVS_NS_LOCATION, NVS_TYPE_ANY, &it) == ESP_OK) {
        while (it != NULL) {
            nvs_entry_info_t info;
            nvs_entry_info(it, &info);

            char raw[64] = {0};
            size_t raw_len = sizeof(raw);
            location_target_t target;
            if (nvs_get_str(nvs, info.key, raw, &raw_len) == ESP_OK &&
                location_target_from_entry(info.key, raw, &target)) {
                /* Two keys can resolve to one target (the contact key spells
                 * an address in hex, and hex has two spellings per digit), and
                 * sharing to the same peer twice a round is a waste of
                 * airtime, not a feature. */
                bool duplicate = false;
                for (size_t i = 0; i < out->count; i++) {
                    if (out->items[i].kind == target.kind && out->items[i].id == target.id) {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate) {
                    if (out->count < LOCATION_MAX_TARGETS) {
                        out->items[out->count++] = target;
                    } else {
                        /* Nothing caps how many target keys the config RPCs can
                         * write, so this really is reachable. Count and log
                         * rather than truncate quietly: silently dropping a
                         * target from a share round is exactly the kind of
                         * thing that should be visible. */
                        out->skipped++;
                    }
                }
            }

            if (nvs_entry_next(&it) != ESP_OK) {
                break;
            }
        }
        nvs_release_iterator(it);
    }

    nvs_close(nvs);

    if (out->skipped > 0) {
        ESP_LOGW(TAG, "location share: %u targets over the %d-target cap were not sent",
                 (unsigned)out->skipped, LOCATION_MAX_TARGETS);
    }
}

/* SEC-C1: tier moves into the encrypted plaintext (byte
 * LOCATION_INNER_TIER_OFFSET), padded to L_LOC_INNER so every tier
 * (PRESENCE/COARSE/FULL) produces an identical ciphertext length; an
 * observer cannot infer the tier from packet size. Zeroed first so
 * unused padding is deterministic, not stack garbage. */
static bool location_build_inner(const bramble_position_t* pos, uint8_t tier, uint8_t* inner) {
    memset(inner, 0, L_LOC_INNER);
    inner[LOCATION_INNER_TIER_OFFSET] = tier;
    return location_serialize_for_tier(pos, tier, inner + 1, LOCATION_FULL_SIZE) > 0;
}

uint32_t mesh_send_location_packet(uint32_t dest_addr, const bramble_position_t* pos,
                                   uint8_t tier) {
    if (!pos || !pos->valid)
        return 0;

    if (tier > LOCATION_TIER_PRESENCE) {
        tier = LOCATION_TIER_COARSE;
    }

    uint8_t inner[L_LOC_INNER];
    if (!location_build_inner(pos, tier, inner)) {
        return 0;
    }

    if (dest_addr != 0xFFFFFFFFu) {
        /* No session request from here. This is the one-shot path
         * (bramble.shareLocationOnce) and it runs on the RPC task, while
         * s_location_hs is owned by the mesh task alone. Asking for a session
         * here would make that table cross-task shared state for a caller that
         * reports its own failure to the user anyway; the periodic tick raises
         * the handshake for any address that is a configured target. */
        return location_tx_directed(dest_addr, inner, tier, NULL);
    }
    /* Broadcast with no channel named: the default channel is the one this
     * node speaks on. */
    return location_tx_channel(s_default_channel_idx, inner, tier);
}

static uint32_t location_tx_directed(uint32_t dest_addr, const uint8_t* inner, uint8_t tier,
                                     bool* needs_session) {
    uint32_t pkt_id = next_packet_id();
    uint8_t pkt[BRAMBLE_MAX_PACKET_SIZE] = {0};
    uint8_t nonce[BRAMBLE_NONCE_SIZE];
    uint8_t tag[BRAMBLE_TAG_SIZE];

    /* Directed share (lcr_<addr>): only ever under the recipient's session
     * key, never the channel key (would defeat per-contact confidentiality,
     * the SEC-C1 point). No ACTIVE session reports needs_session to the
     * caller, which asks for a handshake so the target stops being dead;
     * downgrading to a weaker key is never the answer.
     *
     * The POSITION is deliberately not queued to await that handshake the way
     * DM chat does (Task 1.4). Location is real-time presence (RFC M6, never
     * mailbox-deferred), and the receive path already refuses a late one: it
     * drops REPLAY_REJECT_DUP and REPLAY_BELOW_WINDOW identically rather than
     * accepting out of order. Delivering the coordinate captured at handshake
     * time would present a stale fix as current. The next due tick sends a
     * fresh one instead, so the cost of establishing the session is at most
     * one interval of latency, not a wrong position. */
    bramble_header_t header = {
        .version = BRAMBLE_VERSION,
        .type = PKT_TYPE_LOCATION,
        .flags = FLAG_ENCRYPT, /* no FLAG_CHANNEL: session-keyed (SEC-C1) */
        .hop_limit = 3,
        .dest_addr = dest_addr,
        .packet_id = pkt_id,
    };
    bramble_header_serialize(&header, pkt, HEADER_SIZE);

    /* A directed LOCATION share is a session payload exactly like a chat DM
     * (SEC-C1) and rides the SAME per-message ratchet: dm_session_ratchet_
     * encrypt prepends the 3-byte cleartext ratchet header (epoch||index)
     * ahead of the ciphertext, so the framed output is DM_RATCHET_HEADER_SIZE
     * bytes longer than the plaintext. Pad the plaintext out to L_LOC_INNER +
     * CHANNEL_MSG_OVERHEAD so every tier lands on one fixed session-path size
     * (M11 tier-hiding); the directed vs channel path is already
     * distinguishable from the cleartext FLAG_CHANNEL bit, so the 3-byte
     * ratchet header is not new metadata. */
    uint8_t session_inner[L_LOC_INNER + CHANNEL_MSG_OVERHEAD] = {0};
    memcpy(session_inner, inner, L_LOC_INNER);
    uint8_t ciphertext[DM_RATCHET_HEADER_SIZE + sizeof(session_inner)];
    size_t framed_len = 0;

    xSemaphoreTake(s_dm_mutex, portMAX_DELAY);
    dm_session_t* sess = dm_lookup(s_dm_table, dest_addr);
    int enc_ret = -1;
    bool no_session = !(sess && sess->state == DM_STATE_ACTIVE);
    if (sess && sess->state == DM_STATE_ACTIVE) {
        xSemaphoreTake(s_nonce_mutex, portMAX_DELAY);
        int nonce_ret = nonce_counter_next(nonce);
        xSemaphoreGive(s_nonce_mutex);
        if (nonce_ret == 0) {
            enc_ret = dm_session_ratchet_encrypt(sess, &header, s_identity->address, session_inner,
                                                 sizeof(session_inner), nonce, ciphertext, tag,
                                                 &framed_len);
            if (enc_ret == 0)
                sess->last_active_ms = now_ms(); /* Fix 1: real activity, not eviction bait */
        }
    }
    DM_MUTEX_GIVE();

    if (needs_session)
        *needs_session = no_session;

    if (enc_ret != 0) {
        if (no_session) {
            ESP_LOGD(TAG,
                     "No session yet for directed location share to %08" PRIX32
                     ", requesting handshake",
                     dest_addr);
        } else {
            ESP_LOGW(TAG, "Directed location share to %08" PRIX32 " failed to encrypt", dest_addr);
        }
        return 0;
    }

    memcpy(pkt + BRAMBLE_DATA_SRC_ADDR_OFFSET, &s_identity->address, 4);
    /* Wire v4: originator writes its own address as prev_hop, same as
     * send_data_packet/send_dm_packet. */
    memcpy(pkt + BRAMBLE_DATA_PREV_HOP_OFFSET, &s_identity->address, 4);
    /* Wire v4 (F1): origin-authenticate; see send_data_packet. LOCATION
     * shares the envelope so it carries the field, though it is never
     * relayed today (handle_location delivers dest==self/broadcast only).
     * Mandatory-provisioning (Task 2): abort if unprovisioned. */
    if (data_auth_sign(&header, s_identity->address, pkt + BRAMBLE_DATA_AUTH_HMAC_OFFSET) != 0) {
        ESP_LOGD(TAG, "unprovisioned: inert, dropping location (session) send");
        return 0;
    }
    memcpy(pkt + BRAMBLE_DATA_NONCE_OFFSET, nonce, BRAMBLE_NONCE_SIZE);
    memcpy(pkt + BRAMBLE_DATA_NONCE_OFFSET + BRAMBLE_NONCE_SIZE, ciphertext, framed_len);
    memcpy(pkt + BRAMBLE_DATA_NONCE_OFFSET + BRAMBLE_NONCE_SIZE + framed_len, tag,
           BRAMBLE_TAG_SIZE);
    size_t wire_len =
        BRAMBLE_DATA_ENVELOPE_PREFIX_SIZE + BRAMBLE_NONCE_SIZE + framed_len + BRAMBLE_TAG_SIZE;

    int rc = mesh_tx(pkt, (uint8_t)wire_len, TX_KIND_DATA);
    if (rc == TX_GATE_OK) {
        ESP_LOGI(TAG, "TX location (session) to %08" PRIX32 " tier=%u len=%u", dest_addr, tier,
                 (unsigned)wire_len);
        return pkt_id;
    }
    return 0;
}

/*
 * Channel-shared (broadcast): channel_msg_encrypt under the named channel's
 * key, addressed to 0xFFFFFFFF.
 *
 * This is what makes group location sharing work on a mesh that has only
 * ever broadcast. A directed share needs an ACTIVE DM session, a session
 * needs a route, and routes are only built by directed traffic, so a set of
 * nodes that have exchanged nothing but beacons and channel messages have no
 * routes and can never complete a unicast share. A channel share needs none
 * of that: every holder of the channel key decrypts the same frame.
 *
 * The tier here is the resolution every member of the channel receives.
 * Per-contact tiers grade how much a sender trusts one peer, which has no
 * meaning against a whole channel, so one tier is chosen for the audience
 * and applied to the single frame.
 *
 * Reach is one radio hop. LOCATION has no relay path in either direction
 * (mesh_task.c hands a LOCATION packet to handle_location and nothing
 * forwards it), so a channel share is read by the members of that channel
 * that hear the sender directly.
 */
static uint32_t location_tx_channel(int channel_idx, const uint8_t* inner, uint8_t tier) {
    if (channel_idx < 0 || channel_idx >= s_num_channels) {
        ESP_LOGW(TAG, "Location channel target %d is not a channel this node holds", channel_idx);
        return 0;
    }
    /* Last line of defence before the radio. Target collection already refuses
     * to resolve a public-channel rule, so reaching this is a bug in a caller,
     * not user configuration: fail loudly rather than emit coordinates under a
     * well-known key. */
    if (!location_channel_target_is_permitted(channel_idx)) {
        ESP_LOGE(TAG, "Refusing to share location on the public channel (index %d)", channel_idx);
        return 0;
    }

    const uint32_t dest_addr = 0xFFFFFFFFu;
    uint32_t pkt_id = next_packet_id();
    uint8_t pkt[BRAMBLE_MAX_PACKET_SIZE] = {0};
    uint8_t nonce[BRAMBLE_NONCE_SIZE];
    uint8_t tag[BRAMBLE_TAG_SIZE];

    bramble_header_t header = {
        .version = BRAMBLE_VERSION,
        .type = PKT_TYPE_LOCATION,
        .flags = FLAG_ENCRYPT | FLAG_CHANNEL, /* no tier bits: tier lives in the ciphertext now */
        .hop_limit = 3,
        .dest_addr = dest_addr,
        .packet_id = pkt_id,
    };
    bramble_header_serialize(&header, pkt, HEADER_SIZE);

    uint8_t aad[HEADER_SIZE + 4];
    bramble_build_aead_aad(&header, s_identity->address, aad, sizeof(aad));

    xSemaphoreTake(s_nonce_mutex, portMAX_DELAY);
    int nonce_ret = nonce_counter_next(nonce);
    xSemaphoreGive(s_nonce_mutex);
    if (nonce_ret != 0) {
        ESP_LOGE(TAG, "Nonce counter unavailable, dropping location send: %d", nonce_ret);
        return 0;
    }

    uint8_t ciphertext[CHANNEL_MSG_OVERHEAD + L_LOC_INNER];
    if (channel_msg_encrypt(&s_channels[channel_idx], s_identity->address, APP_TYPE_LOCATION, 0,
                            inner, L_LOC_INNER, aad, sizeof(aad), nonce, ciphertext, tag) != 0) {
        ESP_LOGE(TAG, "Channel encrypt failed for location send");
        return 0;
    }

    memcpy(pkt + BRAMBLE_DATA_SRC_ADDR_OFFSET, &s_identity->address, 4);
    /* Wire v4: originator writes its own address as prev_hop. */
    memcpy(pkt + BRAMBLE_DATA_PREV_HOP_OFFSET, &s_identity->address, 4);
    /* Wire v4 (F1): origin-authenticate; see send_data_packet. Mandatory-
     * provisioning (Task 2): abort if unprovisioned. */
    if (data_auth_sign(&header, s_identity->address, pkt + BRAMBLE_DATA_AUTH_HMAC_OFFSET) != 0) {
        ESP_LOGD(TAG, "unprovisioned: inert, dropping location (channel) send");
        return 0;
    }
    memcpy(pkt + BRAMBLE_DATA_NONCE_OFFSET, nonce, BRAMBLE_NONCE_SIZE);
    memcpy(pkt + BRAMBLE_DATA_NONCE_OFFSET + BRAMBLE_NONCE_SIZE, ciphertext, sizeof(ciphertext));
    memcpy(pkt + BRAMBLE_DATA_NONCE_OFFSET + BRAMBLE_NONCE_SIZE + sizeof(ciphertext), tag,
           BRAMBLE_TAG_SIZE);

    size_t wire_len = BRAMBLE_DATA_ENVELOPE_PREFIX_SIZE + BRAMBLE_NONCE_SIZE + sizeof(ciphertext) +
                      BRAMBLE_TAG_SIZE;
    int rc = mesh_tx(pkt, (uint8_t)wire_len, TX_KIND_DATA);
    if (rc == TX_GATE_OK) {
        ESP_LOGI(TAG, "TX location (channel) to %08" PRIX32 " tier=%u len=%u", dest_addr, tier,
                 (unsigned)wire_len);
        return pkt_id;
    }
    return 0;
}

static void mesh_emit_location_event(const char* event, uint32_t peer_addr, uint8_t tier,
                                     uint32_t timestamp_ms, int16_t rssi, int8_t snr,
                                     uint32_t count) {
    cJSON* params = cJSON_CreateObject();
    if (!params)
        return;
    cJSON_AddStringToObject(params, "event", event);
    if (peer_addr != 0) {
        char addr_buf[9];
        snprintf(addr_buf, sizeof(addr_buf), "%08" PRIX32, peer_addr);
        cJSON_AddStringToObject(params, "peer", addr_buf);
    }
    cJSON_AddStringToObject(params, "tier", location_tier_to_string(tier));
    cJSON_AddNumberToObject(params, "timestamp_ms", timestamp_ms);
    if (rssi != 0 || snr != 0) {
        cJSON_AddNumberToObject(params, "rssi", rssi);
        cJSON_AddNumberToObject(params, "snr", snr);
    }
    if (count > 0) {
        cJSON_AddNumberToObject(params, "count", count);
    }
    rpc_notify("bramble.onLocationEvent", params);
    cJSON_Delete(params);
}

/*
 * Ask for the DM session a directed target needs, so a contact target on a
 * peer nobody has ever messaged stops being permanently dead.
 *
 * Lock ordering, from the call graph rather than a scan of critical sections.
 * This runs in the TRANSMIT phase, which holds nothing on entry, and the
 * DH-heavy INIT is not performed here: it is queued to handshake_worker_task,
 * the same M7 rule maybe_trigger_dm_rehandshake and maybe_schedule_dm_epoch_
 * rekey follow from this same mesh task. xQueueSend takes only the queue's own
 * lock, while holding nothing. So this adds NO new lock edge: the transmit
 * phase already takes s_dm_mutex (location_tx_directed's dm_lookup) and
 * s_nonce_mutex (nonce_counter_next, whose reserve-ceiling flush reaches NVS
 * two frames down), which is the forward ordering. The two documented AB BA
 * deadlocks both need NVS as the OUTER lock, and it never is here. This must
 * never be called from location_collect_targets, which does hold the NVS shim
 * mutex across its iteration.
 *
 * Gated on being a current neighbor, like the self-heal path: a first-contact
 * INIT is a unicast DATA envelope, so a peer that is not a direct neighbor has
 * no route for it and spraying at one buys nothing.
 */
static void location_request_dm_session(uint32_t peer, uint32_t t) {
    if (peer == 0 || peer == s_identity->address)
        return;
    if (!neighbor_lookup(&s_neighbors, peer))
        return;
    if (!location_hs_should_attempt(&s_location_hs, peer, t))
        return;

    dm_handshake_work_item_t item;
    memset(&item, 0, sizeof(item));
    item.src_addr = peer;
    item.channel_idx = s_default_channel_idx;
    item.initiate = true;
    if (xQueueSend(s_handshake_work_q, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Handshake queue full, location session request dropped for %08" PRIX32,
                 peer);
        return;
    }
    ESP_LOGI(TAG, "Location target %08" PRIX32 " has no session; initiating handshake", peer);
}

/*
 * TRANSMIT phase of a share round: nothing here holds an NVS handle, an
 * iterator, or any mutex on entry.
 *
 * The split from location_collect_targets is not stylistic. The NVS iterator
 * holds the shim's mutex for the whole iteration, so sending from inside the
 * loop would make NVS the outer lock over everything the send path touches.
 * Two reverse orderings exist and each one is a deadlock:
 *   - s_state_mutex then NVS, via mesh_add_channel() ->
 *     channel_storage_save() (mesh_channels.c), on the ble_rpc task. An
 *     addChannel landing during a share round wedges both tasks.
 *   - s_dm_mutex then s_nonce_mutex then NVS, via nonce_counter_next()'s
 *     reserve-ceiling flush -> mesh_nonce_write() (nonce_counter.c,
 *     mesh_persist.c), on any sendMessage. The NVS call is two frames below
 *     the mutex, which is why a textual scan of the critical sections does
 *     not show it.
 * Collecting first keeps the shim mutex strictly below every other lock in
 * the system: it is taken while holding nothing else, and nothing else is
 * taken while holding it.
 *
 * It also stops the NVS mutex being pinned across synchronous LBT radio
 * transmits (up to about a second per target in the worst case), which was
 * starving the gnss drain task and the bond writer.
 */
static void mesh_send_location_updates(uint32_t t, const location_policy_t* policy,
                                       const bramble_position_t* source_pos,
                                       const location_target_set_t* targets) {
    bramble_position_t pos = *source_pos;
    pos.timestamp = t / 1000;
    pos.valid = true;

    uint32_t sent_count = 0;
    /* At most one handshake started per round. DM_MAX_HANDSHAKING is a quarter
     * of DM_MAX_SESSIONS while LOCATION_MAX_CONTACTS is 16, so a round that
     * asked for every target at once could consume every handshake slot and
     * starve chat and the desync self-heal. The remaining targets ask on
     * subsequent rounds. */
    bool handshake_started = false;
    for (size_t i = 0; i < targets->count; i++) {
        const location_target_t* target = &targets->items[i];
        if (!location_schedule_is_due(&s_location_schedule, target, t)) {
            continue;
        }

        uint8_t inner[L_LOC_INNER];
        bool sent = false;
        bool needs_session = false;
        if (location_build_inner(&pos, target->tier, inner)) {
            if (target->kind == LOCATION_TARGET_CHANNEL) {
                sent = location_tx_channel((int)target->id, inner, target->tier) != 0;
            } else {
                sent = location_tx_directed(target->id, inner, target->tier, &needs_session) != 0;
            }
        }

        if (target->kind == LOCATION_TARGET_CONTACT) {
            if (needs_session) {
                if (!handshake_started) {
                    location_request_dm_session(target->id, t);
                    handshake_started = true;
                }
            } else {
                /* A session exists, so this peer is answering: drop its
                 * backoff, and a later outage starts from the fast first
                 * attempt again rather than from a decayed delay. */
                location_hs_clear(&s_location_hs, target->id);
            }
        }

        location_schedule_record(&s_location_schedule, target, t, sent);
        if (sent) {
            sent_count++;
        }
    }

    if (sent_count > 0) {
        /* The mesh clock of the last share that reached the radio. The GNSS
         * duty cycler schedules the receiver against this, so an attempt that
         * never transmitted must not move it. */
        s_location_last_send_ms = t;
        mesh_emit_location_event("sent", 0, policy->default_tier, t, 0, 0, sent_count);
    }
}

static void mesh_persist_peer_location(uint32_t peer_addr, const bramble_position_t* pos,
                                       uint8_t tier, uint32_t now_ms) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS_LOCATION, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }

    char key[16];
    snprintf(key, sizeof(key), "lp_%08" PRIX32, peer_addr);

    persisted_peer_location_t stored = {
        .latitude_e7 = pos->latitude_e7,
        .longitude_e7 = pos->longitude_e7,
        .altitude_m = pos->altitude_m,
        .accuracy_m = pos->accuracy_m,
        .speed_kmh = pos->speed_kmh,
        .heading_deg2 = pos->heading_deg2,
        .timestamp = pos->timestamp,
        .received_ms = now_ms,
        .tier = tier,
    };

    nvs_set_blob(nvs, key, &stored, sizeof(stored));
    nvs_commit(nvs);
    nvs_close(nvs);
}

/*
 * SEC-C1 RX channel-path glue (Task 2.2): trial-decrypts against the known
 * channels, then hands the resulting plaintext to location_parse_inner
 * (decrypt-mechanism-agnostic tier + position parsing, exported by the
 * location component; see its own comment for why it stays dependency-free
 * rather than owning this glue itself). This function is intentionally
 * thin: the only logic worth testing (tier extraction, tier-appropriate
 * position parsing) lives in location_parse_inner, already covered by
 * test_location_crypto.c.
 */
static int location_rx_decode_channel(const uint8_t* nonce, const uint8_t* ciphertext,
                                      size_t ct_len, const uint8_t* tag, const uint8_t* aad,
                                      size_t aad_len, uint8_t* tier_out,
                                      bramble_position_t* pos_out, int* channel_index_out) {
    uint8_t plaintext[CHANNEL_MSG_MAX_PLAINTEXT_SIZE];
    channel_msg_info_t info;
    if (channel_msg_decrypt(s_channels, s_num_channels, nonce, ciphertext, ct_len, tag, aad,
                            aad_len, plaintext, &info, now_ms()) != 0) {
        return -1;
    }
    *channel_index_out = info.channel_index;
    return location_parse_inner(info.data, info.data_len, tier_out, pos_out);
}

void handle_location(const uint8_t* data, uint8_t len, int16_t rssi, int8_t snr) {
    /* SEC-C1 RX (Task 2.2): location packet layout matches DATA's wire v4
     * envelope: header(12) + src_addr(4) + prev_hop(4) + nonce(12) +
     * ciphertext(N) + tag(16). LOCATION is never forwarded today (no relay
     * path exists for it), so prev_hop is written by the originator only
     * and not consulted for reverse-route learning here; see
     * task-4-report.md for why that is in scope but deliberately not
     * turned on. */
    if (len < BRAMBLE_DATA_ENVELOPE_PREFIX_SIZE + BRAMBLE_NONCE_SIZE + BRAMBLE_TAG_SIZE + 1) {
        ESP_LOGW(TAG, "Location packet too short: %u", len);
        return;
    }

    uint32_t src_addr = 0;
    memcpy(&src_addr, data + BRAMBLE_DATA_SRC_ADDR_OFFSET, 4);
    if (src_addr == s_identity->address) {
        return;
    }

    bramble_header_t header;
    if (bramble_header_deserialize(&header, data, len) != ESP_OK) {
        return;
    }

    /* Origin-authenticate before the position is believed, the same
     * network-key check handle_data applies (Task 4-fix F1). Every originator
     * signs a LOCATION frame (both branches of the send path call
     * data_auth_sign), so this drops nothing legitimate. It matters most on
     * the channel path: BRAMBLE_PUBLIC_CHANNEL_PSK is public, so the AEAD tag
     * proves only that the sender holds a public key, and src_addr on its own
     * is a free-to-forge claim. Without this check anyone within radio range
     * could plant an arbitrary peer at arbitrary coordinates on every map in
     * earshot. */
    if (!data_auth_verify(&header, src_addr, data + BRAMBLE_DATA_AUTH_HMAC_OFFSET)) {
        ESP_LOGW(TAG, "Location auth_hmac failed (src=%08" PRIX32 "), drop", src_addr);
        return;
    }

    const uint8_t* nonce = data + BRAMBLE_DATA_NONCE_OFFSET;
    size_t ct_len = len - BRAMBLE_DATA_ENVELOPE_PREFIX_SIZE - BRAMBLE_NONCE_SIZE - BRAMBLE_TAG_SIZE;
    const uint8_t* ciphertext = nonce + BRAMBLE_NONCE_SIZE;
    const uint8_t* tag = ciphertext + ct_len;

    if (ct_len > BRAMBLE_MAX_PACKET_SIZE) {
        ESP_LOGW(TAG, "Location ciphertext too large: %u", (unsigned)ct_len);
        return;
    }

    uint8_t aad[HEADER_SIZE + 4];
    bramble_build_aead_aad(&header, src_addr, aad, sizeof(aad));

    /* Discriminator mirrors handle_data (SEC-C1/SEC-C2 share the same
     * mechanism): FLAG_CHANNEL set means channel-shared, trial-decrypt
     * against known channels; absent means a directed share, look up the
     * ACTIVE session for src_addr under s_dm_mutex. */
    uint8_t tier = 0;
    bramble_position_t pos = {0};
    int ok = -1;
    int is_channel_message = (header.flags & FLAG_CHANNEL) ? 1 : 0;
    int channel_index = 0;

    if (is_channel_message) {
        ok = location_rx_decode_channel(nonce, ciphertext, ct_len, tag, aad, sizeof(aad), &tier,
                                        &pos, &channel_index);
    } else {
        xSemaphoreTake(s_dm_mutex, portMAX_DELAY);
        dm_session_t* sess = dm_lookup(s_dm_table, src_addr);
        if (sess && sess->state == DM_STATE_ACTIVE) {
            /* Canonical session-path size (Task 2.1, M11): the encoder always
             * pads the plaintext to exactly L_LOC_INNER + CHANNEL_MSG_OVERHEAD
             * bytes; the ratchet then prepends its DM_RATCHET_HEADER_SIZE
             * cleartext header, so the on-wire ciphertext is exactly that much
             * longer. Reject anything else outright rather than risk decrypting
             * into an undersized buffer. dm_session_ratchet_decrypt reads the
             * cleartext epoch||index header (part of ciphertext here) and writes
             * only the payload into plaintext. */
            uint8_t plaintext[L_LOC_INNER + CHANNEL_MSG_OVERHEAD];
            size_t loc_pt_len = 0;
            if (ct_len == DM_RATCHET_HEADER_SIZE + sizeof(plaintext) &&
                dm_session_ratchet_decrypt(sess, &header, src_addr, nonce, ciphertext, ct_len, tag,
                                           plaintext, &loc_pt_len) == DM_DECRYPT_OK &&
                loc_pt_len == sizeof(plaintext)) {
                ok = location_parse_inner(plaintext, sizeof(plaintext), &tier, &pos);
                sess->last_active_ms = now_ms(); /* Fix 1: real activity, not eviction bait */
            }
        }
        DM_MUTEX_GIVE();
    }

    if (ok != 0) {
        ESP_LOGW(TAG, "Failed to decrypt/parse location from %08" PRIX32, src_addr);
        return;
    }

    /* SEC-M1/M6: the SAME node-global replay window DATA uses (Task 0.5):
     * one nonce counter and one replay window per sender, shared across
     * every packet type that sender's node encrypts, since a counter is
     * only ever used once regardless of what it authenticates. Never
     * consults the deferred cache (that is chat-only, Task 0.6): location
     * is real-time presence, so both REPLAY_REJECT_DUP and
     * REPLAY_BELOW_WINDOW are dropped identically, never accepted late.
     * Fix 2 (red-team panel): skip this SHARED window entirely for a
     * public-channel decrypt, whose src_addr is a free-to-forge claim
     * (BRAMBLE_PUBLIC_CHANNEL_PSK is public), same reasoning and same
     * helper as handle_data. Public-channel location updates rely on the
     * pre-existing packet_id/type dedup for loop suppression instead. */
    uint64_t rx_counter = nonce_counter_extract(nonce);
    if (channel_source_is_replay_trustworthy(is_channel_message, channel_index)) {
        int rp = replay_check_and_add(&s_replay, src_addr, rx_counter, now_ms());
        if (rp != REPLAY_ACCEPT) {
            ESP_LOGD(TAG, "Location replay drop from %08" PRIX32 " ctr=%llu (rp=%d)", src_addr,
                     (unsigned long long)rx_counter, rp);
            return;
        }
    }

    uint32_t t = now_ms();
    location_cache_update(&s_location_mgr, src_addr, &pos, t);
    mesh_persist_peer_location(src_addr, &pos, tier, t);

    ESP_LOGI(TAG, "RX location from %08" PRIX32 " tier=%u RSSI:%d SNR:%d", src_addr, tier, rssi,
             snr);
    rpc_notify("bramble.onPeerLocation", NULL);
    mesh_emit_location_event("received", src_addr, tier, t, rssi, snr, 0);
}

/* Resolve this node's own position: live GPS first, manual NVS coords as the
 * fallback for GPS-less boards. This is THE self-position source; the policy
 * tick below and mesh_get_location_state both use it, and it is exported
 * (mesh_task.h) so bramble.shareLocationOnce shares it too instead of
 * re-reading manual NVS coords on its own and hard-failing GPS-only nodes. It
 * exists because the codebase grew two location_manager_t instances: main.c's
 * g_location_mgr received every GPS fix and nothing ever read it, while the
 * s_location_mgr that the T-Deck map reads had no writer for my_position at
 * all, so the map showed "waiting for position fix" forever against a 3 m
 * GPS fix. */
bool mesh_resolve_self_position(bramble_position_t* out) {
    bramble_position_t gps_pos;
    if (gps_get_position(&gps_pos) && gps_pos.valid) {
        *out = gps_pos;
        return true;
    }

    nvs_handle_t nvs;
    if (nvs_open(NVS_NS_LOCATION, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    int32_t lat_e6 = 0;
    int32_t lon_e6 = 0;
    bool has_manual = (nvs_get_i32(nvs, "lat_e6", &lat_e6) == ESP_OK) &&
                      (nvs_get_i32(nvs, "lon_e6", &lon_e6) == ESP_OK) &&
                      !(lat_e6 == 0 && lon_e6 == 0);
    nvs_close(nvs);
    if (!has_manual) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->latitude_e7 = lat_e6 * 10;
    out->longitude_e7 = lon_e6 * 10;
    out->valid = true;
    return true;
}

void mesh_location_policy_tick(uint32_t t) {
    if ((t - s_location_last_policy_tick_ms) < LOCATION_POLICY_TICK_MS) {
        return;
    }
    s_location_last_policy_tick_ms = t;

    nvs_handle_t nvs;
    if (nvs_open(NVS_NS_LOCATION, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }

    location_policy_t policy;
    location_policy_load_or_defaults(nvs, &policy);
    nvs_close(nvs);

    if (!policy.enabled) {
        return;
    }

    bramble_position_t source_pos = {0};
    bool has_source = mesh_resolve_self_position(&source_pos);

    location_target_set_t targets;
    location_collect_targets(&targets);
    /* Release the pacing slots of targets the operator has removed before
     * deciding who is due, so a slot cannot outlive its rule. */
    location_schedule_retain(&s_location_schedule, targets.items, targets.count);

    if (!location_share_round_enabled(&policy, has_source, targets.count)) {
        return;
    }

    /* Whether an individual target is due is the schedule's call, not this
     * one: each target paces itself off its own interval. */
    mesh_send_location_updates(t, &policy, &source_pos, &targets);
}

/* Cache window for the NVS-backed half of the share state. The gnss task
 * calls this once a second for its duty-cycle decision, and answering it
 * meant an nvs_open plus a policy load plus a full directory iteration every
 * time: a second 1Hz NVS reader alongside the mesh task's policy tick, which
 * is the traffic that made the iterator race reachable at all. The policy is
 * operator configuration that changes on human timescales, so re-reading it
 * every 10s instead of every 1s costs nothing real and cuts this caller's
 * share of the NVS load by 90%. */
#define LOCATION_SHARE_STATE_CACHE_MS 10000U

/* Feeds the GNSS duty-cycling decision (gps_duty_should_power). Reads the
 * policy exactly like mesh_location_policy_tick above (its own NVS handle,
 * not shared state), because the caller here is the nRF GNSS task, not the
 * mesh task: on the nRF target this runs on a different FreeRTOS task
 * entirely, once per second, so it cannot reuse the mesh task's read. The
 * one piece of mesh-task state this does read, s_location_last_send_ms, is
 * a plain uint32_t whose only writer is the share round the policy tick
 * above drives, which runs on the mesh task and nowhere else; a
 * naturally aligned 32-bit load on this target cannot tear, so the worst
 * case is a snapshot up to one policy tick stale, the same lock-free
 * cross-task reasoning mesh_get_location_state uses for my_position. No
 * lock is taken here on purpose. */
void mesh_location_get_share_state(mesh_location_share_state_t* out) {
    /* Unsynchronized on purpose: these are only ever touched from this
     * function, and on every target it has exactly one caller (the nRF GNSS
     * task's duty tick), so there is no second writer to race. If a second
     * caller is ever added, this cache needs a lock. */
    static bool s_cached_valid;
    static bool s_cached_sharing_active;
    static uint32_t s_cached_interval_s;
    static uint32_t s_cached_at_ms;

    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);

    if (!s_cached_valid || (now - s_cached_at_ms) >= LOCATION_SHARE_STATE_CACHE_MS) {
        nvs_handle_t nvs;
        if (nvs_open(NVS_NS_LOCATION, NVS_READONLY, &nvs) != ESP_OK) {
            /* Not cached: a failed open is usually "namespace does not exist
             * yet", which the very next write changes, so caching it would
             * hide the transition for up to the whole window. */
            out->sharing_active = false;
            out->interval_s = 0;
            out->last_send_ms = s_location_last_send_ms;
            return;
        }

        location_policy_t policy;
        location_policy_load_or_defaults(nvs, &policy);
        nvs_close(nvs);

        location_target_set_t targets;
        location_collect_targets(&targets);

        s_cached_sharing_active = policy.enabled && targets.count > 0;
        /* The cadence the receiver has to be ready for is the SHORTEST of the
         * configured targets, not the node-wide default: a target may pace
         * itself faster than the policy interval, and waking against the
         * slower number would miss every share in between. */
        uint16_t min_interval_s = location_targets_min_interval_s(targets.items, targets.count);
        s_cached_interval_s = min_interval_s > 0 ? min_interval_s : policy.interval_s;
        s_cached_at_ms = now;
        s_cached_valid = true;
    }

    out->sharing_active = s_cached_sharing_active;
    out->interval_s = s_cached_interval_s;
    /* Never cached: last_send_ms is a plain in-RAM counter that the share path
     * updates, and the duty-cycle logic wakes the receiver relative to it, so
     * a stale value here would move every scheduled wake. */
    out->last_send_ms = s_location_last_send_ms;
}

void mesh_get_location_state(location_manager_t* out) {
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    *out = s_location_mgr;
    xSemaphoreGive(s_state_mutex);
    /* s_location_mgr only accumulates PEER positions from the mesh; nothing
     * feeds its my_position (GPS fixes historically went to a separate manager
     * in main.c that nobody read). Resolve self-position from the live source
     * on the way out, same GPS-then-manual-NVS logic the location-share TX
     * path uses, so the map sees exactly what the mesh would transmit. Outside
     * the mutex on purpose: gps_get_position has its own lock and NVS reads
     * must not run under s_state_mutex. */
    if (!mesh_resolve_self_position(&out->my_position)) {
        out->my_position.valid = false;
    }
}
