/**
 * mesh_routing.c: RREQ/RREP/RERR routing, jittered RREQ and flood relay, unicast forwarding.
 *
 * Split out of mesh_task.c (issue #86); pure code motion, no behavior change.
 * Shared state and cross-module entry points come from mesh_internal.h.
 */
#include "mesh_internal.h"

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "nvs_keys.h"
#include "cJSON.h"
#include "rpc_dispatcher.h"

static const char* TAG = "mesh";

/* Forward declarations for intra-module static helpers. */
static void send_rrep(const bramble_rrep_t* rrep);
static void send_rerr(uint32_t broken_dest, uint32_t broken_next_hop);
static void schedule_rreq_forward(const bramble_rreq_t* fwd);

void send_rreq(const bramble_rreq_t* rreq) {
    /* Red-team audit: was buf[64], a hand-counted constant. RREQ_SIZE (30)
     * is unaffected by the ws 1.3b size bumps and always fit, but
     * macro-ized for the same reason as the other TX buffers in this
     * file: a hand-counted constant can't warn you when it stops being
     * big enough. */
    uint8_t buf[RREQ_SIZE];
    if (bramble_rreq_serialize(rreq, buf, sizeof(buf)) == ESP_OK) {
        ESP_LOGI(TAG, "TX RREQ query=%08" PRIX32 " dest=%08" PRIX32, rreq->query_id,
                 rreq->header.dest_addr);
        /* Deny behavior: routing control rides the reserved CRITICAL lane
         * (can also borrow from NORMAL); if even that is exhausted the
         * discovery retry scheduler will try again. Log loudly. */
        if (mesh_tx(buf, HEADER_SIZE + 18, TX_KIND_ROUTING) == TX_GATE_ERR_BUDGET) {
            ESP_LOGW(TAG, "RREQ denied by airtime budget");
        }
    }
}

static void send_rrep(const bramble_rrep_t* rrep) {
    /* Mandatory-provisioning (Task 2): inert when unprovisioned. The RREP was
     * built and rrep_sign'd elsewhere; without the network key that MAC is the
     * all-zero sentinel, so do not transmit. */
    if (!network_key_is_provisioned()) {
        ESP_LOGD(TAG, "unprovisioned: inert, skipping RREP");
        return;
    }
    /* Red-team audit: was buf[64], a hand-counted constant. RREP_SIZE (40
     * as of ws 1.3b) always fit, but macro-ized for the same reason as
     * the other TX buffers in this file. */
    uint8_t buf[RREP_SIZE];
    if (bramble_rrep_serialize(rrep, buf, sizeof(buf)) == ESP_OK) {
        ESP_LOGI(TAG, "TX RREP query=%08" PRIX32 " → next=%08" PRIX32, rrep->query_id,
                 rrep->next_hop);
        /* Pre-existing bug fixed here (found while adding ws 1.3b's seq
         * field): this was HEADER_SIZE + 19 (31 bytes), 3 short of the
         * struct's 22-byte payload (query_id+src_addr+next_hop+hop_count+
         * route_metric+auth_hmac), truncating the last 3 bytes of
         * auth_hmac on every real transmit. RREP_SIZE (the macro, not a
         * hand-counted offset) is what every other RREP size check already
         * uses, so this can't drift again the way HEADER_SIZE+19 did. */
        if (mesh_tx(buf, RREP_SIZE, TX_KIND_ROUTING) == TX_GATE_ERR_BUDGET) {
            ESP_LOGW(TAG, "RREP denied by airtime budget");
        }
    }
}

static void send_rerr(uint32_t broken_dest, uint32_t broken_next_hop) {
    /* Mandatory-provisioning (Task 2): inert when unprovisioned (rerr_sign
     * needs the network key). */
    if (!network_key_is_provisioned()) {
        ESP_LOGD(TAG, "unprovisioned: inert, skipping RERR");
        return;
    }
    /* components/routing/forwarding.c: rerr_build fills version/type/flags/
     * hop_limit/dest_addr/reporter_addr/broken_dest/broken_next_hop
     * identically to the struct literal this replaced. packet_id and seq
     * are this node's own counters (rerr_build leaves them zeroed since it
     * owns no sequencing state), so they're set here same as before. */
    bramble_rerr_t rerr = rerr_build(s_identity->address, broken_dest, broken_next_hop);
    rerr.header.packet_id = next_packet_id();
    /* ws 1.3b: every re-origination draws its own fresh seq (unlike RREP's
     * origin-stable seq, RERR's seq is per-hop, matching reporter_addr).
     * Fail-closed: no seq means no RERR goes out this call; the caller's
     * route-broken detection or forwarding chain simply doesn't propagate
     * this hop, rather than shipping an unfresh/replayable report. */
    uint64_t rerr_seq;
    if (control_seq_next(&rerr_seq) != 0) {
        ESP_LOGE(TAG, "Seq counter unavailable, dropping RERR for broken_dest=%08" PRIX32,
                 broken_dest);
        return;
    }
    rerr.seq[0] = (uint8_t)(rerr_seq >> 40);
    rerr.seq[1] = (uint8_t)(rerr_seq >> 32);
    rerr.seq[2] = (uint8_t)(rerr_seq >> 24);
    rerr.seq[3] = (uint8_t)(rerr_seq >> 16);
    rerr.seq[4] = (uint8_t)(rerr_seq >> 8);
    rerr.seq[5] = (uint8_t)rerr_seq;
    /* SEC-H1 (STAGED): re-signed on every call, including re-origination,
     * since this function builds a fresh struct each time (fresh
     * reporter_addr/packet_id/seq), and reporter_addr/seq are now
     * MAC-covered alongside the origin-stable broken_dest/broken_next_hop
     * (ws 1.3b). */
    rerr_sign(&rerr);
    /* Red-team audit: was buf[64], a hand-counted constant. RERR_SIZE (38
     * as of ws 1.3b) always fit, but macro-ized for the same reason as
     * the other TX buffers in this file. */
    uint8_t buf[RERR_SIZE];
    if (bramble_rerr_serialize(&rerr, buf, sizeof(buf)) == ESP_OK) {
        ESP_LOGI(TAG, "TX RERR broken_dest=%08" PRIX32, broken_dest);
        /* RERR_SIZE (the macro), not a hand-counted offset: RREP's
         * equivalent hand-counted offset drifted 3 bytes short of its
         * struct for years before being caught in ws 1.3b Task 2. */
        if (mesh_tx(buf, RERR_SIZE, TX_KIND_ROUTING) == TX_GATE_ERR_BUDGET) {
            ESP_LOGW(TAG, "RERR denied by airtime budget");
        }
    }
}

/* ── Originator pseudonym helpers for RREQ privacy ─────────────────── */

/* No pseudonym lookup table exists: RREP correlation runs on query_ids in
 * the pending-discovery table, and every attempt (first try or retry) gets a
 * fresh query_id, so its pseudonym is re-derived on demand and never stored.
 * Retries being unlinkable new queries is the intended privacy behavior. */

/* ── End pseudonym helpers ───────────────────────────────────── */

/* ── Jittered RREQ forwarding (DES-3) ────────────────────────── */

/**
 * Queue an RREQ forward with random jitter so same-hop relays do not
 * rebroadcast at the same instant. Falls back to immediate transmission when
 * the queue is full (under a forward storm the jitter no longer matters, and
 * dropping the forward could sever the only path).
 */
static void schedule_rreq_forward(const bramble_rreq_t* fwd) {
    uint32_t jitter = discovery_forward_jitter_ms(esp_random());
    for (int i = 0; i < RREQ_FWD_QUEUE_CAPACITY; i++) {
        if (!s_rreq_fwd_queue[i].used) {
            s_rreq_fwd_queue[i].used = true;
            s_rreq_fwd_queue[i].due_at_ms = now_ms() + jitter;
            s_rreq_fwd_queue[i].rreq = *fwd;
            ESP_LOGD(TAG, "RREQ fwd query=%08" PRIX32 " jittered %" PRIu32 "ms", fwd->query_id,
                     jitter);
            return;
        }
    }
    ESP_LOGW(TAG, "RREQ fwd queue full; forwarding query=%08" PRIX32 " immediately", fwd->query_id);
    send_rreq(fwd);
}

/**
 * Transmit any due jittered RREQ forwards. Called from the mesh task main
 * loop, so forwards stay scheduled rather than blocking packet handling.
 */
void process_rreq_forward_queue(uint32_t t) {
    for (int i = 0; i < RREQ_FWD_QUEUE_CAPACITY; i++) {
        if (s_rreq_fwd_queue[i].used && (int32_t)(t - s_rreq_fwd_queue[i].due_at_ms) >= 0) {
            send_rreq(&s_rreq_fwd_queue[i].rreq);
            s_rreq_fwd_queue[i].used = false;
        }
    }
}

/* ── End jittered RREQ forwarding ──────────────────────────────── */

/* ── Jittered channel-flood relay (Task 5) ──────────────────────── */

/**
 * Queue a broadcast/channel DATA rebroadcast with random jitter, exactly
 * like schedule_rreq_forward: same-hop relays that all decided to flood the
 * same frame should not key up at the same instant. DROPS the relay when the
 * queue is full (issue #87): a full queue means this node is already
 * congested, and the old behaviour of transmitting immediately, without
 * jitter, inverted backpressure exactly there. Placement, the drop, and the
 * drop accounting live in channel_flood_relay_admit so they are unit-
 * testable on the host; see channel_flood.h for the full rationale.
 *
 * buf/len are the ALREADY relay-mutated wire bytes (hop_limit decremented,
 * prev_hop rewritten to this node -- see the caller in handle_data): this
 * function only owns the timing, not the frame content.
 *
 * flood_key = packet_id ^ src_addr (the caller already computed it for the
 * src-qualified flood dedup) is recorded on the queued entry so an overheard
 * duplicate of the SAME frame can find and suppress this pending relay before
 * it fires (Flooding F1; see channel_flood_note_overheard). heard starts at 0
 * -- the copy that triggered this schedule is the FIRST copy, never counted
 * as an overheard one.
 *
 * tx_kind is the airtime lane the relay is sent on: TX_KIND_DATA_BROADCAST
 * for a flooded DATA frame, TX_KIND_ACK for a flooded ACK (Flooding F1
 * Task 2). One queue + one suppression engine serves both; only the lane the
 * final mesh_tx debits differs.
 */
void schedule_flood_relay(const uint8_t* buf, uint8_t len, uint32_t jitter_ms, uint32_t flood_key,
                          tx_kind_t tx_kind) {
    if (channel_flood_relay_admit(s_flood_relay_queue, FLOOD_RELAY_QUEUE_CAPACITY, buf, len,
                                  now_ms() + jitter_ms, flood_key, (uint8_t)tx_kind,
                                  &s_flood_relay_drops)) {
        ESP_LOGD(TAG, "Channel flood relay jittered %" PRIu32 "ms", jitter_ms);
        return;
    }
    /* Congested: yield the channel instead of grabbing it. Counted, not
     * silent, so the field can tell "congestion dropped relays" apart from
     * "broadcasts mysteriously do not arrive" (bramble.getDiagnostics ->
     * flood_relay_drops). */
    ESP_LOGW(TAG, "Flood relay queue full; dropping relay (total drops=%" PRIu32 ")",
             s_flood_relay_drops);
}

uint32_t mesh_get_flood_relay_drops(void) { return s_flood_relay_drops; }

/**
 * Transmit any due jittered flood relays. Called from the mesh task main
 * loop alongside process_rreq_forward_queue, so relays stay scheduled
 * rather than blocking packet handling. The airtime budget gets the final,
 * authoritative say here (mesh_tx -> tx_gate_send): a node that was under
 * budget when it decided to relay but has since spent it (e.g. its own
 * traffic, or other jittered relays firing first) still yields instead of
 * transmitting -- the airtime-aware stop that keeps a saturated node from
 * amplifying a storm.
 */
void process_flood_relay_queue(uint32_t t) {
    for (int i = 0; i < FLOOD_RELAY_QUEUE_CAPACITY; i++) {
        if (s_flood_relay_queue[i].used && (int32_t)(t - s_flood_relay_queue[i].due_at_ms) >= 0) {
            if (mesh_tx(s_flood_relay_queue[i].buf, s_flood_relay_queue[i].len,
                        (tx_kind_t)s_flood_relay_queue[i].tx_kind) == TX_GATE_ERR_BUDGET) {
                ESP_LOGD(TAG, "Jittered flood relay denied by airtime budget");
            }
            s_flood_relay_queue[i].used = false;
        }
    }
}

void handle_rreq(const uint8_t* data, uint8_t len, int16_t rssi, int8_t snr) {
    /* Mandatory-provisioning gate, matching handle_beacon (and every other RX
     * control handler): an unprovisioned node has no mesh key, cannot sign an
     * RREP, and has no business flooding or installing routes. Drop before any
     * effect. Note this is a participation gate, not RREQ authentication: the
     * RREQ itself carries no HMAC (there is no rreq_verify), so a route learned
     * from it is only ever an unauthenticated hint (see the ROUTE_SRC_BREADCRUMB
     * installs below and issue #74). */
    if (!network_key_is_provisioned()) {
        return;
    }

    bramble_rreq_t rreq;
    if (bramble_rreq_deserialize(&rreq, data, len) != ESP_OK) {
        ESP_LOGW(TAG, "Invalid RREQ packet");
        return;
    }

    ESP_LOGI(TAG, "RX RREQ query=%08" PRIX32 " dest=%08" PRIX32 " hops=%u metric=%u", rreq.query_id,
             rreq.header.dest_addr, rreq.hop_count, rreq.metric);

    /* First-arrival dedup: the first flood copy wins. Path quality still
     * arbitrates at route_install time, between RREPs answering different
     * discovery attempts (each attempt floods under a fresh query_id). */
    if (rreq_dedup_check_and_add(&s_rreq_dedup, rreq.query_id, now_ms())) {
        ESP_LOGD(TAG, "Duplicate RREQ query=%08" PRIX32, rreq.query_id);
        return;
    }

    /* Record reverse route (for RREP path back) */
    reverse_route_add(&s_reverse_routes, rreq.query_id, rreq.prev_hop, now_ms());

    /* Is this RREQ for us? */
    if (rreq.header.dest_addr == s_identity->address) {
        ESP_LOGI(TAG, "RREQ is for us; sending RREP");
        bramble_rrep_t rrep = rrep_build_destination(&rreq, s_identity->address);

        /* ws 1.3b: draw the 48-bit origin seq and re-sign to cover it
         * (rrep_build_destination already signed once with seq=0 from the
         * zeroed struct; this re-sign is the one that ships). Fail-closed:
         * no seq means no RREP goes out this round, and the RREQ
         * originator's retry logic will try again later. */
        uint64_t rrep_seq;
        if (control_seq_next(&rrep_seq) != 0) {
            ESP_LOGE(TAG, "Seq counter unavailable, dropping RREP for query=%08" PRIX32,
                     rreq.query_id);
            return;
        }
        rrep.seq[0] = (uint8_t)(rrep_seq >> 40);
        rrep.seq[1] = (uint8_t)(rrep_seq >> 32);
        rrep.seq[2] = (uint8_t)(rrep_seq >> 24);
        rrep.seq[3] = (uint8_t)(rrep_seq >> 16);
        rrep.seq[4] = (uint8_t)(rrep_seq >> 8);
        rrep.seq[5] = (uint8_t)rrep_seq;
        rrep_sign(&rrep);

        /* Route RREP back toward the previous hop */
        reverse_route_t* rev = reverse_route_lookup(&s_reverse_routes, rreq.query_id);
        if (rev) {
            rrep.next_hop = rev->prev_hop;
        }
        send_rrep(&rrep);

        /* Install route to the source via prev_hop. The link penalty
         * subtracts from the higher-is-better path metric.
         *
         * Trust class is ROUTE_SRC_BREADCRUMB, NOT ROUTE_SRC_DISCOVERED
         * (issue #74). prev_hop, hop_count and metric all come from an
         * unauthenticated RREQ (there is no rreq_verify; RREQ has no HMAC
         * field on the wire), so a keyless attacker can forge them. Classing
         * this as DISCOVERED, the most-trusted route class, handed that
         * attacker the strongest route-poisoning primitive in the stack: the
         * route_install trust rules refuse to let anything displace or evict
         * a DISCOVERED entry. As a BREADCRUMB it is exactly what it is: an
         * unauthenticated next-hop hint that a real HMAC-gated DISCOVERED
         * route (learned via the signed RREP this RREQ triggers) always
         * reclaims. No wire change: source is internal routing-table state. */
        uint8_t metric = metric_apply_link_penalty(rreq.metric, (int8_t)rssi, snr);
        route_install(&s_routes, rreq.prev_hop, rreq.prev_hop, rreq.hop_count, metric, ROUTE_ACTIVE,
                      ROUTE_SRC_BREADCRUMB, now_ms());
        return;
    }

    /* Not for us: check whether we already hold a fresh, trustworthy route
     * to the destination (Phase 2 "save reactive routing": intermediate-
     * node RREP; see discovery.h's rrep_build_intermediate/
     * intermediate_rrep_route_usable doc comments for the trust/freshness
     * rules). Answering here short-circuits discovery for this whole
     * subtree instead of needing the flood to reach D itself.
     *
     * Having replied, this node does NOT also forward the RREQ onward:
     * that is the airtime-saving half of the tradeoff (the point of this
     * feature is to cut RREQ flood cost, not just add RREP traffic on top
     * of an unchanged flood), and it is safe because the RREQ is a
     * broadcast: every OTHER neighbor that heard the same RREQ still makes
     * its own independent forward decision, so a subtree this node cannot
     * vouch for still gets flooded through other paths. If this node's
     * cached route turns out to be stale/wrong beyond what
     * intermediate_rrep_route_usable already guards against, the
     * originator's expanding-ring retry (a fresh query_id, see
     * discovery.h) still reaches D normally. */
    route_entry_t* cached_route = route_lookup(&s_routes, rreq.header.dest_addr);
    if (cached_route && intermediate_rrep_route_usable(cached_route, now_ms())) {
        ESP_LOGI(TAG, "Intermediate RREP for dest=%08" PRIX32 " via cached route (hops=%u)",
                 rreq.header.dest_addr, cached_route->hop_count);
        bramble_rrep_t rrep =
            rrep_build_intermediate(&rreq, cached_route, s_identity->address, (int8_t)rssi, snr);

        /* Same seq draw + re-sign convention as the "RREQ is for us"
         * branch above: this node is a fresh RREP signer (answering on D's
         * behalf), so it needs its own origin sequence number, not D's. */
        uint64_t rrep_seq;
        if (control_seq_next(&rrep_seq) != 0) {
            ESP_LOGE(TAG,
                     "Seq counter unavailable, dropping intermediate RREP for query=%08" PRIX32,
                     rreq.query_id);
            return;
        }
        rrep.seq[0] = (uint8_t)(rrep_seq >> 40);
        rrep.seq[1] = (uint8_t)(rrep_seq >> 32);
        rrep.seq[2] = (uint8_t)(rrep_seq >> 24);
        rrep.seq[3] = (uint8_t)(rrep_seq >> 16);
        rrep.seq[4] = (uint8_t)(rrep_seq >> 8);
        rrep.seq[5] = (uint8_t)rrep_seq;
        rrep_sign(&rrep);

        send_rrep(&rrep);

        /* Same as the "RREQ is for us" branch: install a route to the
         * RREQ's ultimate source via prev_hop, since this node is now
         * answering on D's behalf and should be just as reachable from the
         * source as the real destination would have been. Same trust-class
         * reasoning too: ROUTE_SRC_BREADCRUMB, not DISCOVERED, because the
         * prev_hop/hop_count/metric come from an unauthenticated RREQ (issue
         * #74). */
        uint8_t src_metric = metric_apply_link_penalty(rreq.metric, (int8_t)rssi, snr);
        route_install(&s_routes, rreq.prev_hop, rreq.prev_hop, rreq.hop_count, src_metric,
                      ROUTE_ACTIVE, ROUTE_SRC_BREADCRUMB, now_ms());
        return;
    }

    /* Not for us, and no usable cached route: schedule a jittered forward
     * while the hop budget lasts. The > 1 bound makes hop_limit N mean
     * N-hop reach exactly (a relay receiving 1 does not forward), matching
     * the spec and the simulator. */
    if (rreq.header.hop_limit > 1) {
        if (!rreq_fwd_allow(&s_rreq_fwd_rl, now_ms())) {
            ESP_LOGW(TAG, "Forwarded RREQ rate limited (query=%08" PRIX32 ")", rreq.query_id);
        } else {
            bramble_rreq_t fwd = rreq_forward(&rreq, s_identity->address, (int8_t)rssi, snr);
            schedule_rreq_forward(&fwd);
        }
    }
}

void handle_rrep(const uint8_t* data, uint8_t len, int16_t rssi, int8_t snr) {
    bramble_rrep_t rrep;
    if (bramble_rrep_deserialize(&rrep, data, len) != ESP_OK) {
        ESP_LOGW(TAG, "Invalid RREP packet");
        return;
    }

    /* SEC-H1 (STAGED, see network_key.h): reject before installing any
     * route from this RREP. Covers query_id/src_addr/hop_count/route_metric
     * only, so a legitimate relay's next_hop/header.dest_addr rewrite
     * (rrep_forward) still verifies. */
    if (!rrep_verify(&rrep)) {
        ESP_LOGW(TAG, "RREP auth failed query=%08" PRIX32 " src=%08" PRIX32, rrep.query_id,
                 rrep.src_addr);
        return;
    }

    ESP_LOGI(TAG, "RX RREP query=%08" PRIX32 " src=%08" PRIX32 " hops=%u", rrep.query_id,
             rrep.src_addr, rrep.hop_count);

    /* ws 1.3b: replay check on the authenticated signer (rrep.src_addr is
     * MAC-covered, so an attacker cannot dodge the window by mutating it).
     * Checked after rrep_verify and strictly before route_install, so a
     * replayed RREP never resurrects a stale route. */
    uint64_t rrep_seq = ((uint64_t)rrep.seq[0] << 40) | ((uint64_t)rrep.seq[1] << 32) |
                        ((uint64_t)rrep.seq[2] << 24) | ((uint64_t)rrep.seq[3] << 16) |
                        ((uint64_t)rrep.seq[4] << 8) | (uint64_t)rrep.seq[5];
    if (!control_replay_ok(rrep.src_addr, rrep_seq)) {
        ESP_LOGW(TAG, "RREP replay query=%08" PRIX32 " src=%08" PRIX32, rrep.query_id,
                 rrep.src_addr);
        return;
    }

    /* The route-install and deliver/forward/drop decision lives in
     * rrep_rx_decide, a pure host-testable function in components/routing.
     * The link penalty subtracts from the higher-is-better path metric. */
    uint8_t metric = metric_apply_link_penalty(rrep.route_metric, (int8_t)rssi, snr);
    rrep_rx_decision_t d =
        rrep_rx_decide(&rrep, s_identity->address, metric, &s_pending_disc, &s_reverse_routes);

    if (d.install_route) {
        route_install(&s_routes, d.route_dest, d.route_next_hop, d.route_hops, d.route_metric,
                      ROUTE_ACTIVE, ROUTE_SRC_DISCOVERED, now_ms());
    }

    switch (d.action) {
    case RREP_RX_DELIVER:
        /* This RREP is for us: we originated the RREQ. */
        ESP_LOGI(TAG, "Route discovered to %08" PRIX32 " (hops=%u, metric=%u)", d.deliver_dest,
                 d.route_hops, d.route_metric);
        discovery_remove(&s_pending_disc, d.deliver_dest);

        /* Flush queued messages waiting for this route */
        flush_queued_messages(d.deliver_dest);
        break;
    case RREP_RX_FORWARD: {
        /* Not for us: forward the RREP toward the originator via the reverse route. */
        bramble_rrep_t fwd = rrep_forward(&rrep, d.forward_to, s_identity->address);
        send_rrep(&fwd);
        break;
    }
    case RREP_RX_DROP:
    default:
        ESP_LOGW(TAG, "No reverse route for RREP query=%08" PRIX32, rrep.query_id);
        break;
    }
}

void rerr_fastfail_notify(uint32_t packet_id, const char* reason, void* ctx) {
    (void)ctx;

    cJSON* params = cJSON_CreateObject();
    if (!params) {
        return;
    }

    char pkt_buf[12];
    snprintf(pkt_buf, sizeof(pkt_buf), "%08" PRIX32, packet_id);
    cJSON_AddStringToObject(params, "packet_id", pkt_buf);
    cJSON_AddStringToObject(params, "status", "failed");
    if (reason) {
        cJSON_AddStringToObject(params, "reason", reason);
    }

    rpc_notify("bramble.onAck", params);
    cJSON_Delete(params);
}

void handle_rerr(const uint8_t* data, uint8_t len) {
    bramble_rerr_t rerr;
    if (bramble_rerr_deserialize(&rerr, data, len) != ESP_OK) {
        ESP_LOGW(TAG, "Invalid RERR packet");
        return;
    }

    /* SEC-H1 (STAGED, see network_key.h): verify before ANY route
     * teardown. An unauthenticated RERR must never break a route: reject
     * before route_lookup/route_marked_broken/fastfail run at all. */
    if (!rerr_verify(&rerr)) {
        ESP_LOGW(TAG, "RERR auth failed dest=%08" PRIX32 " broken_hop=%08" PRIX32, rerr.broken_dest,
                 rerr.broken_next_hop);
        return;
    }

    ESP_LOGW(TAG, "RX RERR: dest=%08" PRIX32 " broken_hop=%08" PRIX32, rerr.broken_dest,
             rerr.broken_next_hop);

    /* ws 1.3b: replay check on the authenticated (reporter_addr, seq) pair
     * (both MAC-covered as of this change, so an attacker cannot dodge the
     * window by mutating either). Checked after rerr_verify and strictly
     * before any teardown effect (route_marked_broken, forwarding,
     * failfast), so a replayed RERR never re-tears-down a live route. */
    uint64_t rerr_seq = ((uint64_t)rerr.seq[0] << 40) | ((uint64_t)rerr.seq[1] << 32) |
                        ((uint64_t)rerr.seq[2] << 24) | ((uint64_t)rerr.seq[3] << 16) |
                        ((uint64_t)rerr.seq[4] << 8) | (uint64_t)rerr.seq[5];
    if (!control_replay_ok(rerr.reporter_addr, rerr_seq)) {
        ESP_LOGW(TAG, "RERR replay reporter=%08" PRIX32 " dest=%08" PRIX32, rerr.reporter_addr,
                 rerr.broken_dest);
        return;
    }

    /* Invalidate route if it uses the broken next hop. components/routing/
     * forwarding.c: rerr_handle does the route_lookup + state/fail_count
     * mutation (identical to the inline logic this replaced) and reports
     * back whether it actually marked a route broken, since only mesh_task
     * needs that to decide on re-origination and logging. */
    bool route_marked_broken = rerr_handle(&s_routes, &rerr);
    if (route_marked_broken) {
        ESP_LOGW(TAG, "Route to %08" PRIX32 " marked BROKEN", rerr.broken_dest);

        /* Forward RERR if hop limit allows */
        if (rerr.header.hop_limit > 1) {
            send_rerr(rerr.broken_dest, rerr.broken_next_hop);
        }
    }

    /* Fail fast for pending packets to the destination, even on forwarded RERRs */
    size_t failed = rerr_ack_failfast_for_dest(&s_pending_acks, rerr.broken_dest, "route_broken",
                                               rerr_fastfail_notify, NULL);
    if (failed > 0) {
        ESP_LOGW(TAG, "RERR fast-failed %u pending ACK(s) for dest %08" PRIX32 "%s",
                 (unsigned)failed, rerr.broken_dest,
                 route_marked_broken ? "" : " (forwarded RERR/no local next-hop match)");
    }
}

void forward_data_packet(const uint8_t* data, uint8_t len, const bramble_header_t* header) {
    /* components/routing/forwarding.c: forward_data() owns the route-lookup
     * plus hop-limit-decrement decision (the same function gosim's bridge.c
     * already calls, and test_forwarding.c already exercises). Task 2 (ws
     * 1.4): mesh_task keeps only its own side effects around the decision:
     * mailbox-store-on-no-route, RERR-on-no-route, the actual TX, and stats.
     * Two behavioral deltas came along for the ride, both resolved by
     * adopting the tested/shipped-by-gosim behavior rather than silently
     * keeping the untested one (see task-2-report.md for the full list):
     *   - a STALE route used to forward is now promoted to ACTIVE with a
     *     refreshed last_confirmed (forward_data_packet never did this);
     *   - route last_used/use_count are now bumped at decision time
     *     (inside forward_data()) rather than only after a successful
     *     mesh_tx, so a budget-denied forward still counts as "used". */
    uint8_t hop_limit = header->hop_limit;
    forward_result_t fwd = forward_data(&s_routes, header->dest_addr, &hop_limit, now_ms());

    if (!fwd.should_send) {
        if (!fwd.route_error) {
            /* Hop limit already exhausted: silent drop, no mailbox/RERR,
             * matching the pre-refactor behavior exactly. */
            ESP_LOGD(TAG, "Data packet hop limit reached, dropping");
            return;
        }

        /* No usable route (unknown dest or ROUTE_BROKEN). */
        /* Extract src_addr from the data packet body (offset
         * BRAMBLE_DATA_SRC_ADDR_OFFSET). */
        uint32_t fwd_src_addr = 0;
        if (len >= BRAMBLE_DATA_SRC_ADDR_OFFSET + 4) {
            memcpy(&fwd_src_addr, data + BRAMBLE_DATA_SRC_ADDR_OFFSET, 4);
        }
        /* If mailbox enabled, store for later delivery instead of dropping */
        if (s_mailbox_enabled &&
            mesh_mailbox_store(fwd_src_addr, header->dest_addr, data, len, header->packet_id)) {
            ESP_LOGI(TAG, "No route to %08" PRIX32 ": stored in mailbox", header->dest_addr);
        } else {
            ESP_LOGW(TAG, "No route to forward data for %08" PRIX32, header->dest_addr);
            send_rerr(header->dest_addr, s_identity->address);
        }
        return;
    }

    /* Rebuild header with the hop limit forward_data() already decremented */
    uint8_t buf[BRAMBLE_MAX_PACKET_SIZE];
    memcpy(buf, data, len);

    bramble_header_t fwd_hdr = *header;
    fwd_hdr.hop_limit = hop_limit;
    bramble_header_serialize(&fwd_hdr, buf, HEADER_SIZE);

    /* Wire v4: overwrite prev_hop with OUR OWN address before rebroadcast,
     * mirroring RREP's forwarder-address rewrite (#119). This is what lets
     * the next hop learn a route back to this DATA's originator via US,
     * closing the reverse-route gap that made multi-hop delivery
     * confirmations die at the first relay. Relay-mutable/MAC-excluded, so
     * this rewrite never touches anything under the AEAD tag. */
    if (len >= BRAMBLE_DATA_PREV_HOP_OFFSET + 4) {
        memcpy(buf + BRAMBLE_DATA_PREV_HOP_OFFSET, &s_identity->address, 4);
    }

    ESP_LOGI(TAG, "Forwarding data to %08" PRIX32 " via %08" PRIX32, header->dest_addr,
             fwd.next_hop);
    /* Deny behavior: relayed traffic is dropped when the NORMAL lane is
     * exhausted; the originator's ACK-driven retries cover recovery. */
    if (mesh_tx(buf, len, TX_KIND_FORWARD) == TX_GATE_ERR_BUDGET) {
        ESP_LOGW(TAG, "Forward denied by airtime budget for %08" PRIX32, header->dest_addr);
    }
}
