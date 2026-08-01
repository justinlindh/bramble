/**
 * mesh_beacon.c: Beacons, identity attestation, and adaptive beacon-interval policy.
 *
 * Split out of mesh_task.c (issue #86); pure code motion, no behavior change.
 * Shared state and cross-module entry points come from mesh_internal.h.
 */
#include "mesh_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_random.h"
#include "nvs.h"
#include "nvs_keys.h"
#include "cJSON.h"
#include "rpc_dispatcher.h"

#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
#include "audio.h"
#endif

static const char* TAG = "mesh";

/* Forward declarations for intra-module static helpers. */
static int send_identity_attestation(void);

/* ── Beacon TX ──────────────────────────────────────────────────────── */

int send_beacon(void) {
    /* Mandatory-provisioning (Task 2): an unprovisioned node is INERT. It has
     * no beacon key (mesh_rederive_beacon_key zeroes it) and must emit no
     * network-key-authenticated frame, so skip the beacon entirely. */
    if (!network_key_is_provisioned()) {
        ESP_LOGD(TAG, "unprovisioned: inert, skipping beacon");
        return -1;
    }

    bramble_beacon_t beacon = {0};

    beacon.header.version = BRAMBLE_VERSION;
    beacon.header.type = PKT_TYPE_BEACON;
    beacon.header.flags = 0;
    beacon.header.hop_limit = 1;          /* beacons are 1-hop only */
    beacon.header.dest_addr = 0xFFFFFFFF; /* broadcast */
    beacon.header.packet_id = next_packet_id();

    beacon.src_addr = s_identity->address;
    beacon.pubkey_hash = s_identity->pubkey_hash;
    beacon.uptime_min = (uint16_t)(now_ms() / 60000);
    /* Wave 2: a plugged-in node's cell voltage is meaningless (the charge
     * rail reads a dead-flat ~4798 mV on the T-Deck), so a confirmed
     * charging node emits the protocol's documented 0xFF sentinel
     * (docs/bramble-protocol-spec.md) instead of a fabricated percentage. */
    battery_status_t bstat;
    battery_get_status(&bstat);
    beacon.battery_pct = battery_beacon_pct(bstat.charging, bstat.pct);
    beacon.tx_queue_depth = 0;
    beacon.neighbor_count = (uint8_t)neighbor_count(&s_neighbors);
    beacon.flags = s_mailbox_enabled ? MAILBOX_BEACON_FLAG : 0;
    /* Timesync: piggyback network time on beacon when synchronized */
    if (s_timesync.synchronized) {
        beacon.network_time = (uint32_t)timesync_get_network_time(&s_timesync, now_ms());
        beacon.time_confidence = timesync_get_stratum(&s_timesync);
    } else {
        beacon.network_time = 0;
        beacon.time_confidence = 0xFFFF; /* no confidence */
    }

    /* Include node name in beacon (if set) */
    if (s_node_name[0] != '\0') {
        beacon.name_len = (uint8_t)strlen(s_node_name);
        if (beacon.name_len > BEACON_NAME_MAX)
            beacon.name_len = BEACON_NAME_MAX;
        memcpy(beacon.name, s_node_name, beacon.name_len);
        beacon.name[beacon.name_len] = '\0';
    }

    /* ws 1.3b: draw the 48-bit origin seq before the HMAC, since seq lives
     * inside the HMAC-covered prefix. Fail-closed: no seq means this
     * interval's beacon doesn't go out; the next scheduled beacon tries
     * again. */
    uint64_t beacon_seq;
    if (control_seq_next(&beacon_seq) != 0) {
        ESP_LOGE(TAG, "Seq counter unavailable, dropping beacon this interval");
        return -1;
    }
    beacon.seq[0] = (uint8_t)(beacon_seq >> 40);
    beacon.seq[1] = (uint8_t)(beacon_seq >> 32);
    beacon.seq[2] = (uint8_t)(beacon_seq >> 24);
    beacon.seq[3] = (uint8_t)(beacon_seq >> 16);
    beacon.seq[4] = (uint8_t)(beacon_seq >> 8);
    beacon.seq[5] = (uint8_t)beacon_seq;

    /* HMAC auth: use shared beacon key (derived from public channel PSK) */
    beacon_compute_hmac(&beacon, s_beacon_key, sizeof(s_beacon_key));

    /* Red-team fix: was buf[64], a hand-counted constant that predates the
     * ws 1.3b size bumps. BEACON_SIZE + 1 + BEACON_NAME_MAX (the max wire
     * size with a full-length name) is 71 as of BEACON_SIZE 54, so any
     * name of 10+ characters overflowed this buffer, bramble_beacon_
     * serialize's own len < need guard rejected it, and the node silently
     * stopped beaconing entirely (no neighbor announce, mailbox flush, or
     * timesync) until the name was cleared. Same size expression
     * beacon_compute_hmac already uses for its own buffer, not a new
     * magic number. */
    uint8_t buf[BEACON_SIZE + 1 + BEACON_NAME_MAX];
    if (bramble_beacon_serialize(&beacon, buf, sizeof(buf)) != ESP_OK) {
        ESP_LOGE(TAG, "Beacon serialize failed");
        return -1;
    }

    size_t beacon_wire_len = bramble_beacon_wire_size(&beacon);
    /* Register the live beacon size with the gate: it funds the beacon
     * reserve (one beacon ToA held back from broadcast-data spenders) and
     * the budget-derived minimum interval used by the scheduler below. */
    tx_gate_set_beacon_size((uint8_t)beacon_wire_len);
    /* Beacons fit the budget by design (reserve + stretched interval);
     * denial is the never-expected backstop and only logs. */
    int ret = mesh_tx(buf, (uint8_t)beacon_wire_len, TX_KIND_BEACON);
    if (ret == TX_GATE_OK) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_shared.beacon_tx_count++;
        xSemaphoreGive(s_state_mutex);
        ESP_LOGI(TAG, "Beacon TX #%" PRIu32 " (neighbors: %d)", s_shared.beacon_tx_count,
                 neighbor_count(&s_neighbors));
    } else if (ret == TX_GATE_ERR_BUDGET) {
        ESP_LOGD(TAG, "Beacon skipped this interval: airtime budget exhausted");
    } else {
        ESP_LOGE(TAG, "Beacon TX failed: %d", ret);
    }
    return ret;
}

/* ── Identity attestation TX (per-node identity Phase 2) ────────────── */

/* Low-cadence self-signed identity broadcast: 230 bytes (the relay-gated
 * frame carrying the endorsement cert, IDENTITY_ATTESTATION_SIZE) every 15
 * minutes is the design's approved airtime budget. One frame is 1164.3 ms at
 * the PHY a stock node actually transmits on (the frequency plan's SF9/125k,
 * which mesh_init_radio_config programs over RADIO_PROFILE_LONG_RANGE's SF10)
 * or 181.9 ms on MEDIUM_RANGE (SF7/250k), so the per-node duty at this cadence
 * is ~0.129% and ~0.0202% respectively (computed via
 * components/radio/radio_airtime.c; the same frame is 2123.8 ms at the profile
 * table's SF10, the figure this comment used to quote, in us by a units typo.
 * The trust-anchor cert grew the frame 158 -> 230, a ~40% airtime bump that
 * stays negligible). Do not raise the cadence without re-flagging that
 * budget. */
#define ATTESTATION_INTERVAL_MS (15u * 60u * 1000u)
/* Short retry after a failed/denied send, so a boot-time budget denial
 * does not leave the node unattested for a full interval. */
#define ATTESTATION_RETRY_MS 60000u

/*
 * Build, self-sign and broadcast this node's identity attestation
 * (PKT_TYPE_IDENTITY_ATTESTATION): {address, X25519 pub, Ed25519 pub}
 * signed by the node's OWN Ed25519 key over the canonical message
 * bramble_identity_attestation_signed_msg builds (packet.h), then
 * relay-gated under the network-key MAC (Phase 3, ident_relay_sign): the
 * Ed25519 sig carries the claim's truth, the MAC carries relay privilege
 * (see the struct comment in packet.h). Ordering is load-bearing: seq is
 * drawn and the Ed25519 sig computed BEFORE ident_relay_sign, because the
 * MAC covers both. seq is drawn once here at origination and never
 * re-drawn by relays (the frame floods unmodified except hop_limit).
 *
 * Broadcast on the BROADCAST budget lane (TX_KIND_DATA_BROADCAST, the
 * same tier the beacon and the flood relay debit). hop_limit uses the
 * same origination helper as flood DATA/ACK sends: ROUTE_HOP_LIMIT_MAX
 * reactive, the configured flood hop budget under s_flood_transport.
 */
static int send_identity_attestation(void) {
    if (!s_identity)
        return -1;

    /* Mandatory-provisioning (Task 2): inert when unprovisioned. The relay-gate
     * MAC (ident_relay_sign) requires the network key; emit nothing without it. */
    if (!network_key_is_provisioned()) {
        ESP_LOGD(TAG, "unprovisioned: inert, skipping identity attestation");
        return -1;
    }

    /* ws 1.3b pattern (send_ack): draw the 48-bit origin seq up front,
     * fail-closed. No seq means no attestation goes out; the retry timer
     * covers it exactly like a budget denial. */
    uint64_t att_seq;
    if (control_seq_next(&att_seq) != 0) {
        ESP_LOGE(TAG, "Seq counter unavailable, skipping identity attestation");
        return -1;
    }

    bramble_identity_attestation_t att = {0};
    att.header.version = BRAMBLE_VERSION;
    att.header.type = PKT_TYPE_IDENTITY_ATTESTATION;
    att.header.flags = 0;
    att.header.hop_limit = flood_origination_hop_limit(s_flood_transport, s_flood_hop_limit);
    att.header.dest_addr = 0xFFFFFFFF; /* broadcast */
    att.header.packet_id = next_packet_id();

    att.src_addr = s_identity->address;
    memcpy(att.x25519_pub, s_identity->public_key, sizeof(att.x25519_pub));
    memcpy(att.ed25519_pub, s_identity->ed25519_public_key, sizeof(att.ed25519_pub));

    /* Endorsement cert (trust-anchor campaign, P1): carry our own cert when we
     * have one, else leave the zero-initialized fields (not_after == 0 ==
     * "no cert"). Set before ident_relay_sign below, which MACs the cert. The
     * cert is NOT part of the Ed25519 self-signature (that stays the 84-byte
     * canonical message); it is the anchor's signature, verified by receivers
     * in a later phase. */
    identity_endorsement_get(&att.not_after, att.endorsement_sig);

    uint8_t msg[IDENTITY_ATTESTATION_MSG_SIZE];
    if (bramble_identity_attestation_signed_msg(&att, msg, sizeof(msg)) != ESP_OK) {
        ESP_LOGE(TAG, "Attestation message build failed");
        return -1;
    }
    if (crypto_ed25519_sign(s_identity->ed25519_private_key, msg, sizeof(msg), att.sig) != 0) {
        ESP_LOGE(TAG, "Attestation sign failed");
        return -1;
    }

    /* Relay gate (Phase 3): write seq, then MAC. Both after the Ed25519
     * sign above, since the MAC covers sig and seq. */
    att.seq[0] = (uint8_t)(att_seq >> 40);
    att.seq[1] = (uint8_t)(att_seq >> 32);
    att.seq[2] = (uint8_t)(att_seq >> 24);
    att.seq[3] = (uint8_t)(att_seq >> 16);
    att.seq[4] = (uint8_t)(att_seq >> 8);
    att.seq[5] = (uint8_t)att_seq;
    ident_relay_sign(&att);

    uint8_t buf[IDENTITY_ATTESTATION_SIZE];
    if (bramble_identity_attestation_serialize(&att, buf, sizeof(buf)) != ESP_OK) {
        ESP_LOGE(TAG, "Attestation serialize failed");
        return -1;
    }

    int ret = mesh_tx(buf, IDENTITY_ATTESTATION_SIZE, TX_KIND_DATA_BROADCAST);
    if (ret == TX_GATE_OK) {
        ESP_LOGI(TAG, "Identity attestation TX (addr=%08" PRIX32 ")", att.src_addr);
    } else if (ret == TX_GATE_ERR_BUDGET) {
        ESP_LOGD(TAG, "Identity attestation deferred: airtime budget exhausted");
    } else {
        ESP_LOGE(TAG, "Identity attestation TX failed: %d", ret);
    }
    return ret;
}

/* Send now and schedule the next attempt: the full interval on success,
 * the short retry on budget denial / radio failure. Called from the
 * post-boot send hook, the periodic maintenance tick, and identity
 * regeneration (so a new identity is announced promptly). */
void attempt_identity_attestation(uint32_t t) {
    int rc = send_identity_attestation();
    s_attestation_last_ms = t;
    s_attestation_wait_ms = (rc == TX_GATE_OK) ? ATTESTATION_INTERVAL_MS : ATTESTATION_RETRY_MS;
}

/*
 * ws 1.3b infra: control-plane seq draw + replay check. Called at the
 * control-plane origination sites (beacons and attestations here, RERR/RREP
 * in mesh_routing.c, delivery receipts and ACKs in mesh_reliability.c) and
 * exported via mesh_internal.h so every site shares the one counter.
 *
 * control_seq_next mirrors the data-plane nonce draw above (e.g.
 * send_data_packet): take s_nonce_mutex, call nonce_counter_next, and on
 * failure release the mutex and fail closed without touching *out. The
 * control plane doesn't need a full 12-byte AEAD nonce, just the 48-bit
 * counter nonce_counter_extract pulls out of it.
 */
int control_seq_next(uint64_t* out) {
    uint8_t nonce[BRAMBLE_NONCE_SIZE];
    xSemaphoreTake(s_nonce_mutex, portMAX_DELAY);
    int nonce_ret = nonce_counter_next(nonce);
    if (nonce_ret != 0) {
        xSemaphoreGive(s_nonce_mutex);
        return nonce_ret;
    }
    *out = nonce_counter_extract(nonce);
    xSemaphoreGive(s_nonce_mutex);
    return 0;
}

/*
 * ws 1.3b infra: control-plane replay check, fed only after a MAC verify
 * passes so signer_addr/seq are authenticated. Separate table from the
 * data-plane s_replay (see s_control_replay above).
 */
bool control_replay_ok(uint32_t signer_addr, uint64_t seq) {
    return replay_check_and_add(&s_control_replay, signer_addr, seq, now_ms()) == REPLAY_ACCEPT;
}

/* ── Packet handlers ────────────────────────────────────────────────── */

void handle_beacon(const uint8_t* data, uint8_t len, int16_t rssi, int8_t snr) {
    /* Mandatory-provisioning (Task 2): an unprovisioned node has no beacon key
     * (mesh_rederive_beacon_key zeroes it), so it cannot authenticate a beacon.
     * Drop before any verify/effect: accepting one would mean trusting an HMAC
     * over an all-zero key (a forgery). Fail closed, accept nothing. */
    if (!network_key_is_provisioned()) {
        return;
    }

    bramble_beacon_t beacon;
    if (bramble_beacon_deserialize(&beacon, data, len) != ESP_OK) {
        ESP_LOGW(TAG, "Invalid beacon (len=%u)", len);
        return;
    }

    /* Ignore our own beacons */
    if (beacon.src_addr == s_identity->address)
        return;

    /* Verify beacon HMAC authenticity using shared beacon key */
    if (!beacon_verify_hmac(&beacon, s_beacon_key, sizeof(s_beacon_key))) {
        ESP_LOGW(TAG, "Beacon from %08" PRIX32 " failed HMAC verification, discarding",
                 beacon.src_addr);
        return;
    }

    /* ws 1.3b: replay check on the authenticated signer (beacon.src_addr
     * is HMAC-covered, so an attacker cannot dodge the window by mutating
     * it). Checked immediately after HMAC verify and strictly before every
     * effect below: address-collision handling, neighbor_update, name
     * store, and timesync_handle_sync. Gating timesync closes the part of
     * NEW-SEC-4 where a replayed beacon re-feeds stale network_time; the
     * bootstrap-quorum race (1.3c) is closed separately by the bounded
     * per-boot grace in identity_store_quorum_eligible. */
    uint64_t beacon_seq = ((uint64_t)beacon.seq[0] << 40) | ((uint64_t)beacon.seq[1] << 32) |
                          ((uint64_t)beacon.seq[2] << 24) | ((uint64_t)beacon.seq[3] << 16) |
                          ((uint64_t)beacon.seq[4] << 8) | (uint64_t)beacon.seq[5];
    if (!control_replay_ok(beacon.src_addr, beacon_seq)) {
        ESP_LOGW(TAG, "Beacon replay src=%08" PRIX32, beacon.src_addr);
        return;
    }

    /* Check for address collision: different pubkey_hash but same address */
    if (identity_check_collision(s_identity, beacon.src_addr, beacon.pubkey_hash)) {
        ESP_LOGE(TAG, "ADDRESS COLLISION with %08" PRIX32 ", regenerating identity!",
                 beacon.src_addr);
        /* Regenerate keypair and persist to NVS */
        if (identity_generate_and_save(s_identity) != 0) {
            /* Entropy not ready (pre-RF window, SEC-L1): identity_generate_and_save
             * refused to persist and left s_identity fully untouched (see
             * crypto_generate_identity). Do NOT report a new identity that was
             * never actually generated. */
            ESP_LOGW(TAG, "identity regeneration deferred: entropy not ready");
            return;
        }
        ESP_LOGW(TAG, "New identity: %08" PRIX32, s_identity->address);
        /* Announce the regenerated identity promptly (Phase 2): new
         * address + keys mean the old attestation no longer describes
         * this node. Budget-gated like every attestation send. */
        attempt_identity_attestation(now_ms());
        /* Notify webapp */
        cJSON* params = cJSON_CreateObject();
        char addr_buf[12];
        snprintf(addr_buf, sizeof(addr_buf), "%08" PRIX32, s_identity->address);
        cJSON_AddStringToObject(params, "new_address", addr_buf);
        cJSON_AddStringToObject(params, "reason", "address_collision");
        rpc_notify("bramble.onIdentityChange", params);
        cJSON_Delete(params);
        return;
    }

    /* Update neighbor table: track if this is a new neighbor */
    uint32_t t = now_ms();
    int old_count = neighbor_count(&s_neighbors);
    int idx =
        neighbor_update(&s_neighbors, beacon.src_addr, (int8_t)rssi, snr, beacon.pubkey_hash, t);
    int new_count = neighbor_count(&s_neighbors);
    bool is_new_peer = (new_count > old_count);

    /* Store peer name if present */
    if (idx >= 0 && beacon.name_len > 0) {
        memcpy(s_neighbors.entries[idx].name, beacon.name, beacon.name_len);
        s_neighbors.entries[idx].name[beacon.name_len] = '\0';
    } else if (idx >= 0) {
        s_neighbors.entries[idx].name[0] = '\0';
    }

    /* Feed timesync from beacon: requires corroboration from multiple sources */
    if (beacon.network_time != 0 && beacon.time_confidence != 0xFFFF) {
        /* ws 1.3c: only established neighbors count toward the pre-commit
         * corroboration quorum (NEW-SEC-4 anti-Sybil lever). Computed after
         * neighbor_update above so the current beacon's tenure (beacon_count,
         * first_seen_ms) is reflected before the established check.
         *
         * Phase 4 identity gate on top: a PINNED peer always corroborates
         * (a fabricated source address cannot be pinned post-rebind: it has
         * no deriving Ed key); an UNPINNED peer corroborates only within the
         * bounded per-boot bootstrap grace (QUORUM_BOOTSTRAP_GRACE_MS) so a
         * fresh mesh still converges, and NEVER after it (NEW-SEC-4 1.3c
         * bootstrap-quorum race closed). Full semantics + tests:
         * identity_store_quorum_eligible (identity_store.h). Runs on the
         * same task as handle_identity_attestation, so no locking. */
        bool established = neighbor_is_established(&s_neighbors, beacon.src_addr, t);
        bool quorum_ok =
            identity_store_quorum_eligible(&s_identity_pins, beacon.src_addr, established, t);
        timesync_handle_sync(&s_timesync, (int64_t)beacon.network_time,
                             (uint8_t)beacon.time_confidence, beacon.src_addr, quorum_ok, t);
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_shared.beacon_rx_count++;
    s_shared.last_rx_rssi = rssi;
    s_shared.last_rx_snr = snr;
    s_shared.neighbors = s_neighbors;
    xSemaphoreGive(s_state_mutex);

    if (idx >= 0) {
        ESP_LOGI(TAG, "Neighbor %08" PRIX32 " RSSI:%d SNR:%d (total: %d)%s", beacon.src_addr, rssi,
                 snr, neighbor_count(&s_neighbors), is_new_peer ? " [NEW]" : "");

#ifdef CONFIG_BRAMBLE_BOARD_TDECK_PLUS
        /* Play peer join tone for new neighbors */
        if (is_new_peer && audio_is_available()) {
            audio_play_tone(AUDIO_TONE_PEER_JOIN);
        }
#endif

        /* Mailbox: flush any stored messages for this newly-seen neighbor */
        if (s_mailbox_enabled) {
            mailbox_flush_for(beacon.src_addr);
        }
    }

    /* Sybil detection: check if multiple neighbors cluster at suspiciously similar RSSI.
     * Log-only for now; detection algorithm needs field validation before dropping beacons. */
    {
        int nc = neighbor_count(&s_neighbors);
        if (nc >= 3) {
            int8_t rssi_vals[MAX_NEIGHBORS];
            for (int i = 0; i < nc && i < MAX_NEIGHBORS; i++) {
                rssi_vals[i] = s_neighbors.entries[i].rssi;
            }
            if (sybil_check_rssi_cluster(rssi_vals, nc)) {
                ESP_LOGW(TAG,
                         "SYBIL WARNING: beacon from %08" PRIX32
                         ", %d neighbors with suspiciously similar RSSI (latest RSSI:%d)",
                         beacon.src_addr, nc, rssi);
            }
        }
    }

    /* Notify any RPC clients that the neighbor table changed */
    rpc_notify("bramble.onNeighborChange", NULL);
}

/*
 * Per-node identity Phase 3 (Part B): receive, pin, and flood-relay an
 * identity attestation. Verification ORDER is the security design:
 *
 *   1. exact-length deserialize;
 *   2. ident_relay_verify: the CHEAP network-key MAC, checked before
 *      anything else. Fail = drop: no relay, no pinning, no Ed25519
 *      verify ever runs. Keyless frames die at the first hop, so an
 *      outsider can neither get spam flooded nor grind this node's CPU
 *      with Ed25519 verifies;
 *   3. control_replay_ok on (src_addr, seq), both MAC-covered, so a
 *      captured attestation cannot be re-injected (packet_id is NOT
 *      MAC-covered, so the dispatch s_dedup gate alone would not stop a
 *      replay with a rewritten packet_id; this does);
 *   4. flood dedup (s_flood_dedup, packet_id ^ src_addr, the same
 *      src-qualified key the DATA flood uses);
 *   5. DELIVER to the identity module regardless of the relay decision:
 *      identity_store_handle_attestation runs the one receive-side
 *      Ed25519 verify and TOFU-pins (see identity_store.h);
 *   6. RELAY exactly like the broadcast DATA flood: channel_flood_decide
 *      (hop-limit floor, duplicate/own-echo suppression, airtime budget)
 *      + the shared jittered schedule_flood_relay queue. The frame
 *      rebroadcasts UNMODIFIED except the hop_limit decrement (the MAC
 *      excludes the header, so pass-through is valid; seq is never
 *      re-drawn by relays).
 *
 * Residual (accepted): relays do NOT Ed25519-verify, so a MAC-valid
 * frame with a garbage sig (a keyed insider misbehaving) still floods,
 * bounded by the airtime budget; every RECEIVER rejects it at the
 * Ed25519 check and counts it (identity_store's sig_failures).
 */
void handle_identity_attestation(const uint8_t* data, uint8_t len) {
    bramble_identity_attestation_t att;
    if (bramble_identity_attestation_deserialize(&att, data, len) != ESP_OK) {
        ESP_LOGW(TAG, "Invalid identity attestation (len=%u)", len);
        return;
    }

    if (!ident_relay_verify(&att)) {
        ESP_LOGW(TAG, "Identity attestation auth failed src=%08" PRIX32 ", drop", att.src_addr);
        return;
    }

    uint64_t att_seq = ((uint64_t)att.seq[0] << 40) | ((uint64_t)att.seq[1] << 32) |
                       ((uint64_t)att.seq[2] << 24) | ((uint64_t)att.seq[3] << 16) |
                       ((uint64_t)att.seq[4] << 8) | (uint64_t)att.seq[5];
    if (!control_replay_ok(att.src_addr, att_seq)) {
        ESP_LOGW(TAG, "Identity attestation replay src=%08" PRIX32, att.src_addr);
        return;
    }

    uint32_t flood_key = att.header.packet_id ^ att.src_addr;
    bool is_dup = dedup_check_and_add(&s_flood_dedup, flood_key, now_ms());

    /* Trust-anchor campaign (P2): the wall-clock epoch for the endorsement
     * expiry check. Use network time ONLY when timesync is confident (the same
     * fail-closed gate handle_data uses for deferred replay); otherwise pass 0
     * so the store does not enforce expiry against an untrusted clock. v1 certs
     * are permanent (UINT64_MAX) so this never fires live, but the pin gate is
     * ready for expiring certs the frozen wire format allows. */
    int64_t net_time_ms = timesync_get_network_time(&s_timesync, now_ms());
    /* Only pass a positive, confident epoch; otherwise 0 (the "unsynced"
     * sentinel the store treats as "do not enforce expiry"). The > 0 guard
     * stops a non-positive int64 from casting to a huge uint64 that would
     * spuriously expire a future non-permanent cert. */
    uint64_t epoch_ms = (timesync_is_confident(&s_timesync, now_ms()) && net_time_ms > 0)
                            ? (uint64_t)net_time_ms
                            : 0;

    /* Deliver locally regardless of the relay decision below. */
    identity_pin_result_t pin = identity_store_handle_attestation(
        &s_identity_pins, &att, s_identity->address, now_ms(), epoch_ms);
    switch (pin) {
    case IDENTITY_PIN_NEW:
        ESP_LOGI(TAG, "Identity pinned: %08" PRIX32 " (%d pinned)", att.src_addr,
                 identity_store_count(&s_identity_pins));
        /* Persist the new binding so it (and any later verified bit on it)
         * survives reboot. A REFRESHED re-attestation only bumps the RAM LRU,
         * so it is deliberately NOT persisted. */
        mesh_pin_store_save();
        break;
    case IDENTITY_PIN_CONFLICT:
        /* Impersonation detected: a network-key holder attested this
         * address under DIFFERENT keys than the pinned binding. First
         * seen wins; the original binding survives. */
        ESP_LOGW(TAG,
                 "IDENTITY CONFLICT for %08" PRIX32 ": attestation with different keys REFUSED"
                 " (conflicts=%" PRIu32 ")",
                 att.src_addr, s_identity_pins.conflicts);
        break;
    case IDENTITY_PIN_BAD_SIG:
        ESP_LOGW(TAG,
                 "Identity attestation Ed25519 sig invalid src=%08" PRIX32 " (keyed garbage,"
                 " sig_failures=%" PRIu32 ")",
                 att.src_addr, s_identity_pins.sig_failures);
        break;
    case IDENTITY_PIN_ADDR_MISMATCH:
        /* Phase 4 address<->key binding: a keyed member attested an
         * address its own Ed25519 key does not derive to. Impersonation
         * attempt (or a badly broken sender), refused on first contact. */
        ESP_LOGW(TAG,
                 "IDENTITY ADDR MISMATCH: %08" PRIX32 " claimed without the deriving key,"
                 " REFUSED (addr_mismatches=%" PRIu32 ")",
                 att.src_addr, s_identity_pins.addr_mismatches);
        break;
    case IDENTITY_PIN_UNENDORSED:
        /* Trust-anchor gate (P2): this node is anchored and the attestation
         * carried no cert (or one not signed by our anchor for this key).
         * NOT pinned; the frame was still relayed (endorsement gates pinning
         * only, never liveness). */
        ESP_LOGW(TAG,
                 "Identity attestation UNENDORSED src=%08" PRIX32 " (no valid anchor cert,"
                 " unendorsed=%" PRIu32 ")",
                 att.src_addr, s_identity_pins.unendorsed);
        break;
    case IDENTITY_PIN_EXPIRED:
        /* Anchored + a valid but EXPIRED cert (not_after past the synced
         * clock). v1 certs are permanent so this is not expected live. */
        ESP_LOGW(TAG, "Identity attestation EXPIRED cert src=%08" PRIX32 " (expired=%" PRIu32 ")",
                 att.src_addr, s_identity_pins.expired);
        break;
    case IDENTITY_PIN_REFRESHED:
    case IDENTITY_PIN_SELF:
    default:
        break;
    }

    /* M2 TOFU-session teardown (identity-campaign follow-up): whenever this
     * attestation left a TRUSTED pinned binding for att.src_addr (NEW,
     * REFRESHED, or CONFLICT - the first-seen binding survives a CONFLICT and
     * is authoritative), drop any ESTABLISHED DM session whose cached peer
     * X25519 key disagrees with that pin. Such a session was a first-contact
     * TOFU handshake pointed at an impostor: the attestation is self-signed,
     * address-bound, and on an anchored node ALSO anchor-endorsed, so the pin
     * is authoritative and the stale session is dropped (recovered by a fresh,
     * now pin-continuity-checked handshake). This closes "a TOFU DM with a
     * Sybil that never endorses gets torn down the instant the real endorsed
     * peer pins." Fail-safe: it only ever DROPS; it never touches a
     * key-MATCHING (healthy) session or a non-ACTIVE handshaking slot. The
     * lookup+compare+teardown run as ONE critical section under s_dm_mutex
     * because process_ke_init/resp on the handshake worker mutate the same
     * slot; logging is deferred until after the lock is released, matching the
     * s_dm_mutex convention elsewhere in this file. */
    if (pin == IDENTITY_PIN_NEW || pin == IDENTITY_PIN_REFRESHED || pin == IDENTITY_PIN_CONFLICT) {
        const identity_pin_t* pinned = identity_store_lookup(&s_identity_pins, att.src_addr);
        if (pinned) {
            bool torn_down = false;
            /* Task 7: this is the ONE event where the identity key genuinely
             * changed (a CONFLICT re-binds the pin, or a first pin lands under
             * a key an already-verified session did not expect), so it is also
             * the one place the verified bit is cleared, not just the session
             * torn down. dm_verified_should_clear folds "was this session
             * actually verified" into the decision, so a never-verified
             * session's teardown does not spuriously touch the pin. */
            bool verified_cleared = false;
            xSemaphoreTake(s_dm_mutex, portMAX_DELAY);
            dm_session_t* sess = dm_lookup(s_dm_table, att.src_addr);
            if (sess && dm_pin_disagrees(sess, pinned->x25519_pub)) {
                if (dm_verified_should_clear(sess, pinned->x25519_pub))
                    verified_cleared = true;
                dm_session_teardown(s_dm_table, att.src_addr);
                torn_down = true;
            }
            DM_MUTEX_GIVE();
            if (torn_down)
                ESP_LOGW(TAG,
                         "DM session torn down: pinned identity for %08" PRIX32
                         " disagrees with the TOFU session key",
                         att.src_addr);
            if (verified_cleared) {
                /* Clear the persisted verified bit (the identity SAS the user
                 * compared out of band no longer matches this peer's actual
                 * key) and persist immediately, same save path as a new pin. */
                identity_store_clear_verified(&s_identity_pins, att.src_addr);
                /* RAM-only warning flag (Task 7.5): this IS the genuine
                 * key-change site, unlike a deliberate user un-verify
                 * (identity_store_clear_verified alone, Task 9), so it is the
                 * one place that sets key_changed. No extra save: it never
                 * persists. */
                identity_store_mark_key_changed(&s_identity_pins, att.src_addr);
                mesh_pin_store_save();
                /* Re-verify-needed signal: Tasks 8-9 own the UI surface (chat
                 * banner / device list badge) for this; mesh_get_peer_verification
                 * (Task 7.5) is now that sink's data source, this greppable log
                 * line stays as a diagnostics trail. */
                ESP_LOGW(TAG,
                         "DM RE-VERIFY NEEDED: %08" PRIX32
                         "'s identity key changed, prior SAS verification revoked",
                         att.src_addr);
            }
        }
    }

    /* Relay through the SAME engine as the broadcast DATA flood
     * (handle_data): channel_flood_decide + schedule_flood_relay, on the
     * BROADCAST budget lane. Own-echo folds into is_duplicate exactly like
     * the DATA flood's is_own_echo. */
    bool is_own_echo = (att.src_addr == s_identity->address);
    bool budget_permits = tx_gate_check(len, TX_KIND_DATA_BROADCAST);
    channel_flood_decision_t flood = channel_flood_decide(
        att.header.hop_limit, is_dup || is_own_echo, budget_permits, esp_random());
    if (flood.should_relay) {
        uint8_t relay_buf[IDENTITY_ATTESTATION_SIZE];
        memcpy(relay_buf, data, len);
        bramble_header_t relay_hdr = att.header;
        relay_hdr.hop_limit = flood.new_hop_limit;
        bramble_header_serialize(&relay_hdr, relay_buf, HEADER_SIZE);
        ESP_LOGI(TAG, "Identity attestation relay from %08" PRIX32 " hop_limit %u->%u",
                 att.src_addr, att.header.hop_limit, flood.new_hop_limit);
        schedule_flood_relay(relay_buf, len, flood.jitter_ms, flood_key, TX_KIND_DATA_BROADCAST);
    } else if (!budget_permits) {
        ESP_LOGD(TAG, "Identity attestation relay denied by airtime budget, src=%08" PRIX32,
                 att.src_addr);
    }
}

/* ── Adaptive beacon interval controller ────────────────────────────── */

/**
 * Record a churn event (neighbor count change) for adaptive beacon policy.
 */
void record_churn_event(uint32_t t, uint8_t neighbor_count) {
    s_churn_history[s_churn_history_idx].timestamp = t;
    s_churn_history[s_churn_history_idx].neighbor_count = neighbor_count;
    s_churn_history_idx = (s_churn_history_idx + 1) % MAX_CHURN_HISTORY;
}

/**
 * Compute adaptive beacon interval based on current mesh conditions.
 * Returns new interval in milliseconds.
 */
/* Emulator only: the beacon base interval, overridable via EMU_BEACON_INTERVAL_MS
 * on the linux target so a headless scenario can beacon far more often than the
 * production 60s. Neighbor discovery in a short scenario otherwise hinges on a
 * SINGLE beacon per node landing in the window (the next is 60s out); if those
 * two lone beacons collide on the half-duplex ether or one is dropped, neither
 * node ever learns the other and the DM falls back to a broadcast, which is the
 * historical ~1/3 nondeterminism this rig showed. A few-second interval gives
 * many independent discovery chances so a neighbor is learned every run.
 * NEIGHBOR_EXPIRY_MS is 600s, so a short interval never churns the table.
 * Returns 0 when no override applies (production, or the env unset), so the
 * caller leaves the real 60s policy -- including any NVS-loaded value -- alone.
 * The real esp32s3 firmware never even compiles the getenv: the whole body is
 * behind the linux guard. */
uint32_t emu_beacon_interval_override_ms(void) {
#ifdef CONFIG_IDF_TARGET_LINUX
    const char* v = getenv("EMU_BEACON_INTERVAL_MS");
    if (v && *v) {
        unsigned long n = strtoul(v, NULL, 10);
        if (n > 0)
            return (uint32_t)n;
    }
#endif
    return 0;
}

uint32_t compute_adaptive_beacon_interval(uint32_t t, uint8_t neighbor_count) {
    uint8_t churn =
        beacon_churn_count(s_churn_history, MAX_CHURN_HISTORY, t, s_beacon_policy.churn_window_ms);

    beacon_interval_decision_t d = beacon_interval_decide(
        s_beacon_policy.enabled, s_beacon_policy.mode == BEACON_MODE_ADAPTIVE,
        s_beacon_policy.base_interval_ms, s_beacon_policy.min_interval_ms,
        s_beacon_policy.max_interval_ms, s_beacon_policy.dense_threshold,
        s_beacon_policy.churn_threshold, neighbor_count, churn);

    if (!d.adaptive_active) {
        s_beacon_status.in_backoff = false;
        return d.interval_ms;
    }

    s_beacon_status.churn_events = churn;
    s_beacon_status.neighbor_count = neighbor_count;

    beacon_policy_mode_t prev_mode = s_beacon_status.active_mode;
    s_beacon_status.active_mode = BEACON_MODE_ADAPTIVE;
    s_beacon_status.in_backoff = d.in_backoff ? true : false;
    s_beacon_status.current_interval_ms = d.interval_ms;

    if (prev_mode != s_beacon_status.active_mode ||
        (s_beacon_status.last_transition_ms == 0 && s_beacon_policy.enabled)) {
        s_beacon_status.last_transition_ms = t;
        ESP_LOGI(TAG, "Beacon policy: neighbors=%u churn=%u interval=%lums %s", neighbor_count,
                 churn, (unsigned long)d.interval_ms,
                 d.in_backoff
                     ? "DENSE"
                     : (d.interval_ms < s_beacon_policy.base_interval_ms ? "CHURN" : "STABLE"));
    }

    return d.interval_ms;
}

int mesh_set_beacon_policy(const beacon_policy_config_t* config) {
    if (!config)
        return -1;

    /* Validate config */
    if (config->min_interval_ms < 10000 || config->max_interval_ms > 300000) {
        ESP_LOGE(TAG, "Invalid beacon interval range");
        return -1;
    }
    if (config->min_interval_ms > config->max_interval_ms) {
        ESP_LOGE(TAG, "Min interval must be <= max interval");
        return -1;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_beacon_policy = *config;
    xSemaphoreGive(s_state_mutex);

    /* Persist to NVS */
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS_BACKPRESSURE, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for beacon policy");
        return -1;
    }

    nvs_set_u8(nvs, "enabled", config->enabled ? 1 : 0);
    nvs_set_u8(nvs, "mode", (uint8_t)config->mode);
    nvs_set_u32(nvs, "base_ms", config->base_interval_ms);
    nvs_set_u32(nvs, "min_ms", config->min_interval_ms);
    nvs_set_u32(nvs, "max_ms", config->max_interval_ms);
    nvs_set_u8(nvs, "dense_th", config->dense_threshold);
    nvs_set_u8(nvs, "churn_th", config->churn_threshold);
    nvs_set_u32(nvs, "churn_win", config->churn_window_ms);

    esp_err_t err = nvs_commit(nvs);
    nvs_close(nvs);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to persist beacon policy to NVS");
        return -1;
    }

    ESP_LOGI(TAG, "Beacon policy updated: enabled=%d mode=%d base=%lums", config->enabled,
             config->mode, (unsigned long)config->base_interval_ms);
    return 0;
}

void mesh_get_beacon_policy(beacon_policy_config_t* config) {
    if (!config)
        return;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    *config = s_beacon_policy;
    xSemaphoreGive(s_state_mutex);
}

void mesh_get_beacon_status(beacon_policy_status_t* status) {
    if (!status)
        return;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    *status = s_beacon_status;
    xSemaphoreGive(s_state_mutex);
}

void mesh_beacon_policy_load_config(void) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS_BACKPRESSURE, NVS_READONLY, &nvs) != ESP_OK) {
        /* No saved config, use defaults */
        return;
    }

    uint8_t enabled = 0, mode = 0, dense_th = 10, churn_th = 3;
    uint32_t base_ms = 60000, min_ms = 30000, max_ms = 120000, churn_win = 60000;

    nvs_get_u8(nvs, "enabled", &enabled);
    nvs_get_u8(nvs, "mode", &mode);
    nvs_get_u32(nvs, "base_ms", &base_ms);
    nvs_get_u32(nvs, "min_ms", &min_ms);
    nvs_get_u32(nvs, "max_ms", &max_ms);
    nvs_get_u8(nvs, "dense_th", &dense_th);
    nvs_get_u8(nvs, "churn_th", &churn_th);
    nvs_get_u32(nvs, "churn_win", &churn_win);
    nvs_close(nvs);

    s_beacon_policy.enabled = (enabled != 0);
    s_beacon_policy.mode = (beacon_policy_mode_t)mode;
    s_beacon_policy.base_interval_ms = base_ms;
    s_beacon_policy.min_interval_ms = min_ms;
    s_beacon_policy.max_interval_ms = max_ms;
    s_beacon_policy.dense_threshold = dense_th;
    s_beacon_policy.churn_threshold = churn_th;
    s_beacon_policy.churn_window_ms = churn_win;

    ESP_LOGI(TAG, "Loaded beacon policy: enabled=%d mode=%d base=%lums", enabled, mode,
             (unsigned long)base_ms);
}

void mesh_rederive_beacon_key(void) {
    /* SEC-H2: derive the beacon HMAC subkey from the current network key with
     * domain-separation label "bramble-beacon-v2". Called at init AND after a
     * runtime setNetworkKey so provisioning takes effect for beacons without a
     * reboot.
     *
     * Mandatory-provisioning (Task 2): the public-PSK fallback is GONE. When
     * unprovisioned there is no beacon key -- zero it so a stale key can never
     * be reused, and the node neither beacons (send_beacon is gated) nor
     * accepts beacons (handle_beacon is gated). Do NOT derive from
     * BRAMBLE_PUBLIC_CHANNEL_PSK: that would re-introduce a control-plane
     * fallback key. */
    if (network_key_is_provisioned()) {
        uint8_t net_key[32];
        network_key_get(net_key);
        const char* salt = "bramble-beacon-v2";
        crypto_hkdf_sha256((const uint8_t*)salt, strlen(salt), net_key, sizeof(net_key), NULL, 0,
                           s_beacon_key, sizeof(s_beacon_key));
        ESP_LOGI(TAG, "Beacon HMAC key derived from the provisioned network key");
    } else {
        memset(s_beacon_key, 0, sizeof(s_beacon_key));
        ESP_LOGW(TAG, "unprovisioned: no beacon key (node inert until provisioned)");
    }
}

void mesh_trigger_attestation(void) {
    /* Re-announce with the current identity + cert. Budget-gated like every
     * attestation (attempt_identity_attestation applies the same TX gate as
     * the periodic path), so a burst of setEndorsement calls cannot flood the
     * air. Inert until provisioned (send_identity_attestation gates on the
     * network key). */
    attempt_identity_attestation(now_ms());
}
