#include "unity.h"
#include "esp_stubs.h"
#include "crypto.h"
#include "network_key.h"
#include "routing_auth.h"
#include "routing.h"
#include "channel_flood.h"
#include "packet.h"
#include "reliability.h"

#include "../components/crypto/crypto_host.c"
#include "../components/network_key/network_key.c"
#include "../components/packet/packet.c"
#include "../components/routing_auth/routing_auth.c"

#include <string.h>

/*
 * Flooding F1 Task 3: send-side flood origination + confirmation/retry
 * lifecycle.
 *
 * main/mesh_task.c is ESP-IDF-only and never host-compiled (see
 * test_unicast_flood.c / test_flooded_ack.c for the same rationale), so this
 * harness mirrors mesh_send_message's flood-vs-reactive origination gate and
 * the pending_ack_tick retry loop using the REAL component functions in the
 * REAL order:
 *   - origination decision: under flood there is NO route discovery, the DATA
 *     is built (data_auth_sign, network-key MAC) and a pending-confirmation is
 *     registered (pending_ack_add, tier from msg_tier_for_send); under reactive
 *     with no route, discovery is initiated and NOTHING is flooded.
 *   - retry: a flood-originated pending entry retries by RE-FLOODING the SAME
 *     stored frame (byte-identical, same packet_id) on backoff, bounded by the
 *     tier max_attempts, then FAILED. This mirrors main/mesh_task.c's ACK retry
 *     tick exactly (attempt >= max_attempts -> FAILED; else re-transmit the
 *     stored frame, bump attempt, back off).
 *   - confirmation: a flooded ACK whose ack_packet_id matches clears the
 *     pending entry (pending_ack_remove) = DELIVERED, stopping retries. (The
 *     ACK's own authenticated flood-relay path is covered by test_flooded_ack.c;
 *     here we exercise the sender's consume + lifecycle side.)
 *
 * The harness contains no independent reliability/auth logic of its own.
 */

#define SELF 0xAAAAAAAAu
#define DEST 0xDDDDDDDDu

void setUp(void) { network_key_clear(); }
void tearDown(void) { network_key_clear(); }

/* Build the exact flood DATA an originator floods: header (dest = D, hop_limit
 * = ROUTE_HOP_LIMIT_MAX = the flood hop budget), src_addr, then the network-key
 * auth_hmac over the masked header + src_addr (data_auth_sign). We do not model
 * the AEAD ciphertext here (that is test_channel_msg.c / test_dm_session.c);
 * the send-side lifecycle only cares about the authenticated envelope that
 * relays flood and the destination re-ACKs. Returns the wire length. */
static uint16_t build_flood_data(uint8_t* buf, uint32_t dest, uint32_t src, uint32_t pkt_id) {
    bramble_header_t h = {0};
    h.version = BRAMBLE_VERSION;
    h.type = PKT_TYPE_DATA;
    h.flags = FLAG_ENCRYPT | FLAG_CHANNEL;
    h.hop_limit = ROUTE_HOP_LIMIT_MAX;
    h.dest_addr = dest;
    h.packet_id = pkt_id;
    bramble_header_serialize(&h, buf, HEADER_SIZE);
    memcpy(buf + BRAMBLE_DATA_SRC_ADDR_OFFSET, &src, 4);
    memcpy(buf + BRAMBLE_DATA_PREV_HOP_OFFSET, &src, 4); /* originator: prev_hop = self */
    data_auth_sign(&h, src, buf + BRAMBLE_DATA_AUTH_HMAC_OFFSET);
    return (
        uint16_t)(BRAMBLE_DATA_NONCE_OFFSET); /* prefix through auth_hmac; enough for this test */
}

/* Origination decision, mirroring mesh_send_message's Task 3 gate exactly:
 * under flood there is no route/discovery; the caller floods and registers a
 * pending confirmation. Under reactive with no route, discovery is initiated
 * and nothing is flooded/registered. */
typedef struct {
    bool discovery_initiated;
    bool flooded;
    bool pending_registered;
} originate_result_t;

static originate_result_t originate(pending_ack_table_t* pending, bool flood_transport,
                                    bool have_route, const uint8_t* frame, uint16_t frame_len,
                                    uint32_t pkt_id, uint32_t dest, bool is_key_exchange,
                                    uint32_t now_ms) {
    originate_result_t r = {0};

    if (!flood_transport) {
        /* Reactive: no route -> discovery + queue, no flood (mesh_send_message
         * returns before mesh_send_channel). */
        if (!have_route) {
            r.discovery_initiated = true;
            return r;
        }
        /* have a route: falls through to a routed send (not modeled here). */
    }

    /* Flood transport (or reactive-with-route): originate the DATA. Under flood
     * this is a single budget-gated broadcast; a pending-confirmation is
     * registered at the message's tier so the retry tick can re-flood it. */
    r.flooded = flood_transport;
    uint8_t tier = msg_tier_for_send(is_key_exchange);
    r.pending_registered =
        (pending_ack_add(pending, pkt_id, dest, tier, frame, frame_len, now_ms) >= 0);
    return r;
}

/* One retry tick, mirroring main/mesh_task.c's ACK retry loop (the 1s tick):
 * for the entry matching pkt_id, if it is due, either FAIL it (attempts
 * exhausted) or RE-FLOOD the stored frame and back off. Re-flood = re-transmit
 * the byte-identical stored frame (no route lookup); we capture it into
 * out_reflood so the test can assert it is the same packet_id/bytes. Returns
 * true iff a re-flood happened this tick. */
static bool flood_retry_tick(pending_ack_table_t* table, uint32_t pkt_id, uint32_t now_ms,
                             uint8_t* out_reflood, uint16_t* out_len, bool* out_failed) {
    for (int i = 0; i < MAX_PENDING_ACKS; i++) {
        pending_ack_t* pa = &table->entries[i];
        if (!pa->active || pa->packet_id != pkt_id)
            continue;
        if (now_ms < pa->next_retry_ms)
            return false;
        if (pa->attempt >= pa->max_attempts) {
            pa->active = false; /* FAILED */
            if (out_failed)
                *out_failed = true;
            return false;
        }
        /* Re-flood: re-transmit the stored broadcast frame verbatim. */
        if (out_reflood)
            memcpy(out_reflood, pa->packet_data, pa->packet_len);
        if (out_len)
            *out_len = pa->packet_len;
        pa->attempt++;
        /* Backoff mirrors mesh_task.c (base << attempt); jitter omitted so the
         * test is deterministic. */
        pa->next_retry_ms = now_ms + (tier_base_delay_ms(pa->tier) << pa->attempt);
        return true;
    }
    return false;
}

/* --- Under flood transport, origination floods immediately with NO discovery
 * and registers a pending confirmation. The A/B partner below proves the
 * toggle is what changes this. --- */
void test_flood_origination_no_discovery_registers_pending(void) {
    pending_ack_table_t pending;
    pending_ack_init(&pending);

    uint8_t frame[BRAMBLE_DATA_NONCE_OFFSET];
    uint16_t flen = build_flood_data(frame, DEST, SELF, 0xC0FFEE01u);

    /* Empty route table (have_route = false), flood ON. */
    originate_result_t r =
        originate(&pending, true, false, frame, flen, 0xC0FFEE01u, DEST, false, 1000);

    TEST_ASSERT_FALSE(r.discovery_initiated); /* NO RREQ under flood */
    TEST_ASSERT_TRUE(r.flooded);              /* flooded immediately */
    TEST_ASSERT_TRUE(r.pending_registered);   /* pending confirmation registered */

    /* The registered frame is the exact authenticated flood DATA (dest = D,
     * network-key MAC verifies), i.e. what relays will flood. */
    bramble_header_t h;
    TEST_ASSERT_EQUAL(ESP_OK, bramble_header_deserialize(&h, pending.entries[0].packet_data,
                                                         pending.entries[0].packet_len));
    TEST_ASSERT_EQUAL_UINT32(DEST, h.dest_addr);
    TEST_ASSERT_EQUAL_UINT8(ROUTE_HOP_LIMIT_MAX, h.hop_limit);
    TEST_ASSERT_TRUE(
        data_auth_verify(&h, SELF, pending.entries[0].packet_data + BRAMBLE_DATA_AUTH_HMAC_OFFSET));
}

/* --- A/B baseline: with flood OFF and no route, origination initiates
 * discovery and floods NOTHING (the reactive path is unchanged). --- */
void test_reactive_origination_discovers_no_flood(void) {
    pending_ack_table_t pending;
    pending_ack_init(&pending);

    uint8_t frame[BRAMBLE_DATA_NONCE_OFFSET];
    uint16_t flen = build_flood_data(frame, DEST, SELF, 0xC0FFEE02u);

    originate_result_t r =
        originate(&pending, false, false, frame, flen, 0xC0FFEE02u, DEST, false, 1000);

    TEST_ASSERT_TRUE(r.discovery_initiated);
    TEST_ASSERT_FALSE(r.flooded);
    TEST_ASSERT_FALSE(r.pending_registered);
}

/* --- A flood-originated message with NO ACK retries by RE-FLOODING the same
 * frame, then FAILS after the tier's max_attempts. Every re-flood is the
 * byte-identical stored frame (same packet_id) -- a re-flood, never a routed
 * retransmit. --- */
void test_flood_no_ack_reflood_then_failed(void) {
    pending_ack_table_t pending;
    pending_ack_init(&pending);

    const uint32_t pkt_id = 0xC0FFEE03u;
    uint8_t frame[BRAMBLE_DATA_NONCE_OFFSET];
    uint16_t flen = build_flood_data(frame, DEST, SELF, pkt_id);
    originate(&pending, true, false, frame, flen, pkt_id, DEST, false, 0);

    /* NORMAL tier: 3 max attempts -> exactly 3 re-floods, then FAILED. */
    int reflood_count = 0;
    bool failed = false;
    uint32_t t = 0;
    for (int tick = 0; tick < 50 && !failed; tick++) {
        t += 1000; /* advance 1s per tick, like mesh_task.c's ACK tick */
        uint8_t reflood[BRAMBLE_DATA_NONCE_OFFSET];
        uint16_t rlen = 0;
        if (flood_retry_tick(&pending, pkt_id, t, reflood, &rlen, &failed)) {
            reflood_count++;
            /* Re-flood is the SAME frame: identical bytes, same packet_id. */
            TEST_ASSERT_EQUAL_UINT16(flen, rlen);
            TEST_ASSERT_EQUAL_MEMORY(frame, reflood, flen);
        }
    }

    TEST_ASSERT_EQUAL_INT(tier_max_retries(MSG_TIER_NORMAL), reflood_count); /* 3 re-floods */
    TEST_ASSERT_TRUE(failed);
    TEST_ASSERT_FALSE(pending.entries[0].active); /* FAILED: entry cleared */
}

/* --- A flood-originated message whose flooded ACK arrives is DELIVERED on the
 * first confirmation: pending_ack_remove clears the entry and no re-flood ever
 * happens. --- */
void test_flood_ack_first_confirmation_delivered(void) {
    pending_ack_table_t pending;
    pending_ack_init(&pending);

    const uint32_t pkt_id = 0xC0FFEE04u;
    uint8_t frame[BRAMBLE_DATA_NONCE_OFFSET];
    uint16_t flen = build_flood_data(frame, DEST, SELF, pkt_id);
    originate(&pending, true, false, frame, flen, pkt_id, DEST, false, 0);

    /* The flooded ACK for pkt_id reaches the sender before any retry is due. */
    bool delivered = pending_ack_remove(&pending, pkt_id);
    TEST_ASSERT_TRUE(delivered);                  /* DELIVERED-confirmed */
    TEST_ASSERT_FALSE(pending.entries[0].active); /* entry cleared */

    /* No re-flood ever fires now: ticking well past every backoff is a no-op. */
    uint32_t t = 0;
    bool failed = false;
    for (int tick = 0; tick < 50; tick++) {
        t += 1000;
        TEST_ASSERT_FALSE(flood_retry_tick(&pending, pkt_id, t, NULL, NULL, &failed));
    }
    TEST_ASSERT_FALSE(failed);
}

/* --- A dropped FIRST ACK then a retry eventually DELIVERED: the first
 * confirmation is lost, so the sender re-floods (same packet_id); the
 * re-flood's ACK arrives and confirms before max_attempts, so the message ends
 * DELIVERED, not FAILED. --- */
void test_flood_dropped_first_ack_retry_delivered(void) {
    pending_ack_table_t pending;
    pending_ack_init(&pending);

    const uint32_t pkt_id = 0xC0FFEE05u;
    uint8_t frame[BRAMBLE_DATA_NONCE_OFFSET];
    uint16_t flen = build_flood_data(frame, DEST, SELF, pkt_id);
    originate(&pending, true, false, frame, flen, pkt_id, DEST, false, 0);

    /* First ACK dropped: advance until the first re-flood fires. */
    uint32_t t = 0;
    bool failed = false;
    int reflood_count = 0;
    for (int tick = 0; tick < 10 && reflood_count == 0; tick++) {
        t += 1000;
        uint8_t reflood[BRAMBLE_DATA_NONCE_OFFSET];
        uint16_t rlen = 0;
        if (flood_retry_tick(&pending, pkt_id, t, reflood, &rlen, &failed)) {
            reflood_count++;
            TEST_ASSERT_EQUAL_MEMORY(frame, reflood, flen); /* re-flood = same frame */
        }
    }
    TEST_ASSERT_EQUAL_INT(1, reflood_count); /* re-flooded after the dropped ACK */
    TEST_ASSERT_TRUE(pending.entries[0].active);

    /* The re-flood's ACK now arrives (before max_attempts): DELIVERED. */
    bool delivered = pending_ack_remove(&pending, pkt_id);
    TEST_ASSERT_TRUE(delivered);
    TEST_ASSERT_FALSE(pending.entries[0].active);
    TEST_ASSERT_FALSE(failed); /* never reached FAILED */
}

/* --- KE stays CRITICAL tier under flood: a key-exchange origination registers
 * with the CRITICAL retry budget (8), so the handshake gets more re-flood
 * chances than a NORMAL message (3). This is the "KE stays critical-tier"
 * requirement, on the flood origination path. --- */
void test_flood_ke_origination_critical_tier(void) {
    pending_ack_table_t pending;
    pending_ack_init(&pending);

    const uint32_t pkt_id = 0xC0FFEE06u;
    uint8_t frame[BRAMBLE_DATA_NONCE_OFFSET];
    uint16_t flen = build_flood_data(frame, DEST, SELF, pkt_id);
    /* is_key_exchange = true: the APP_TYPE_KE handshake transport. */
    originate_result_t r = originate(&pending, true, false, frame, flen, pkt_id, DEST, true, 0);
    TEST_ASSERT_TRUE(r.flooded);
    TEST_ASSERT_TRUE(r.pending_registered);
    TEST_ASSERT_EQUAL_UINT8(MSG_TIER_CRITICAL, pending.entries[0].tier);
    TEST_ASSERT_EQUAL_UINT8(tier_max_retries(MSG_TIER_CRITICAL), pending.entries[0].max_attempts);

    /* Drive it to exhaustion: CRITICAL re-floods 8 times before FAILED, more
     * chances than NORMAL's 3. Jump time to each entry's due point (the
     * exponential backoff reaches minutes) rather than stepping a fixed 1s. */
    bool failed = false;
    int reflood_count = 0;
    for (int guard = 0; guard < 100 && pending.entries[0].active; guard++) {
        uint32_t t = pending.entries[0].next_retry_ms;
        if (flood_retry_tick(&pending, pkt_id, t, NULL, NULL, &failed))
            reflood_count++;
    }
    TEST_ASSERT_EQUAL_INT(tier_max_retries(MSG_TIER_CRITICAL), reflood_count); /* 8 re-floods */
    TEST_ASSERT_TRUE(failed);
}

/* --- Flooding F1 finalize: the flood origination hop_limit is the CONFIGURED
 * operator-settable value, not a constant. flood_origination_hop_limit is the
 * exact selector send_data_packet / send_dm_packet / send_ack call, so testing
 * it proves the originators stamp the configured hop budget. --- */
void test_flood_origination_uses_configured_hop_limit(void) {
    /* Under flood transport the originator stamps the configured value... */
    TEST_ASSERT_EQUAL_UINT8(20, flood_origination_hop_limit(true, 20));
    TEST_ASSERT_EQUAL_UINT8(11, flood_origination_hop_limit(true, 11));
    /* ...and the default (8) reproduces the pre-change constant exactly. */
    TEST_ASSERT_EQUAL_UINT8(FLOOD_HOP_LIMIT_DEFAULT, flood_origination_hop_limit(true, 8));
    TEST_ASSERT_EQUAL_UINT8(ROUTE_HOP_LIMIT_MAX, flood_origination_hop_limit(true, 8));
}

/* --- Reactive origination is UNAFFECTED by the flood hop limit: it always
 * stamps ROUTE_HOP_LIMIT_MAX regardless of the configured flood value, proving
 * the two hop budgets are independent (the reactive path is untouched). --- */
void test_reactive_origination_ignores_flood_hop_limit(void) {
    TEST_ASSERT_EQUAL_UINT8(ROUTE_HOP_LIMIT_MAX, flood_origination_hop_limit(false, 20));
    TEST_ASSERT_EQUAL_UINT8(ROUTE_HOP_LIMIT_MAX, flood_origination_hop_limit(false, 1));
    TEST_ASSERT_EQUAL_UINT8(ROUTE_HOP_LIMIT_MAX, flood_origination_hop_limit(false, 8));
}

/* --- The configured value is clamped to [MIN, CEIL] so a stale NVS / bad RPC
 * value can never originate an out-of-range hop_limit. --- */
void test_flood_hop_limit_clamped_to_range(void) {
    TEST_ASSERT_EQUAL_UINT8(FLOOD_HOP_LIMIT_MIN, flood_hop_limit_clamp(0));
    TEST_ASSERT_EQUAL_UINT8(FLOOD_HOP_LIMIT_CEIL, flood_hop_limit_clamp(9999));
    TEST_ASSERT_EQUAL_UINT8(FLOOD_HOP_LIMIT_CEIL, flood_hop_limit_clamp(FLOOD_HOP_LIMIT_CEIL + 1));
    TEST_ASSERT_EQUAL_UINT8(1, flood_hop_limit_clamp(1));
    TEST_ASSERT_EQUAL_UINT8(32, flood_hop_limit_clamp(32));
    /* Origination applies the same clamp: an over-range config never escapes. */
    TEST_ASSERT_EQUAL_UINT8(FLOOD_HOP_LIMIT_CEIL, flood_origination_hop_limit(true, 100));
    TEST_ASSERT_EQUAL_UINT8(FLOOD_HOP_LIMIT_MIN, flood_origination_hop_limit(true, 0));
}

/* --- A flood DATA built with the configured hop limit carries it on the wire:
 * the serialized header's hop_limit is exactly the configured budget, which is
 * what relays decrement and what sets the flood's reach. --- */
void test_flood_data_wire_carries_configured_hop_limit(void) {
    uint8_t frame[BRAMBLE_DATA_NONCE_OFFSET];
    bramble_header_t h = {0};
    h.version = BRAMBLE_VERSION;
    h.type = PKT_TYPE_DATA;
    h.flags = FLAG_ENCRYPT | FLAG_CHANNEL;
    h.hop_limit = flood_origination_hop_limit(true, 17); /* operator set 17 */
    h.dest_addr = DEST;
    h.packet_id = 0xC0FFEE07u;
    bramble_header_serialize(&h, frame, HEADER_SIZE);

    bramble_header_t got;
    TEST_ASSERT_EQUAL(ESP_OK, bramble_header_deserialize(&got, frame, HEADER_SIZE));
    TEST_ASSERT_EQUAL_UINT8(17, got.hop_limit); /* configured value on the wire */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_flood_origination_no_discovery_registers_pending);
    RUN_TEST(test_reactive_origination_discovers_no_flood);
    RUN_TEST(test_flood_no_ack_reflood_then_failed);
    RUN_TEST(test_flood_ack_first_confirmation_delivered);
    RUN_TEST(test_flood_dropped_first_ack_retry_delivered);
    RUN_TEST(test_flood_ke_origination_critical_tier);
    RUN_TEST(test_flood_origination_uses_configured_hop_limit);
    RUN_TEST(test_reactive_origination_ignores_flood_hop_limit);
    RUN_TEST(test_flood_hop_limit_clamped_to_range);
    RUN_TEST(test_flood_data_wire_carries_configured_hop_limit);
    return UNITY_END();
}
