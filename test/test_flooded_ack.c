#include "unity.h"
#include "esp_stubs.h"
#include "crypto.h"
#include "network_key.h"
#include "routing_auth.h"
#include "packet.h"
#include "dedup.h"
#include "channel_flood.h"
#include "reliability.h"

#include "../components/crypto/crypto_host.c"
#include "../components/network_key/network_key.c"
#include "../components/packet/packet.c"
#include "../components/routing_auth/routing_auth.c"

#include <string.h>

/*
 * Flooding F1 Task 2: flooded-ACK gives sender-confirmation with NO routes.
 *
 * main/mesh_task.c is ESP-IDF-only and never host-compiled (see
 * test_unicast_flood.c / test_data_auth.c for the same rationale), so this
 * harness mirrors handle_ack's flood-relay branch and the sender's consume
 * path using the REAL component functions in the REAL order:
 *   - ack_verify (network-key MAC) BEFORE any relay or consume effect, so a
 *     bad-MAC ACK is dropped and never rebroadcast (the "never act on
 *     unauthenticated wire bytes" rule the DATA flood applies via
 *     data_auth_verify);
 *   - for an ACK NOT addressed to this node: channel_flood_decide (the exact
 *     same engine the broadcast/unicast DATA flood uses) gates the
 *     rebroadcast on hop-limit, the is_own_echo duplicate guard, and the
 *     airtime budget;
 *   - for an ACK addressed to this node (the original sender):
 *     pending_ack_remove marks the pending message delivered-confirmed.
 * The harness contains no independent relay/verify logic of its own; it wires
 * the real functions and observes their outputs. A full wire serialize/
 * deserialize round trip is exercised so the big-endian ack.src_addr on the
 * wire (packet.c) is read back exactly as handle_ack and the dispatch-gate
 * suppression key recompute do.
 */

#define SENDER 0xAAAAAAAAu      /* the original DATA sender (ACK dest_addr) */
#define DEST 0xDDDDDDDDu        /* the DATA destination that originates the ACK */
#define RELAY 0xCCCCCCCCu       /* an intermediate flood relay */
#define DATA_PKT_ID 0x0BADF00Du /* the delivered DATA's packet_id == ack_packet_id */

void setUp(void) { network_key_clear(); }
void tearDown(void) { network_key_clear(); }

static void set_ack_seq(bramble_ack_t* a, uint64_t seq) {
    a->seq[0] = (uint8_t)(seq >> 40);
    a->seq[1] = (uint8_t)(seq >> 32);
    a->seq[2] = (uint8_t)(seq >> 24);
    a->seq[3] = (uint8_t)(seq >> 16);
    a->seq[4] = (uint8_t)(seq >> 8);
    a->seq[5] = (uint8_t)seq;
}

/* Build the ACK the destination (DEST) originates for the sender (SENDER),
 * bound to the delivered DATA's packet_id via ack_packet_id, then sign it. */
static bramble_ack_t make_signed_ack(uint32_t src_addr, uint32_t dest_addr, uint32_t ack_packet_id,
                                     uint8_t hop_limit) {
    bramble_ack_t a = {0};
    a.header.version = BRAMBLE_VERSION;
    a.header.type = PKT_TYPE_ACK;
    a.header.hop_limit = hop_limit;
    a.header.dest_addr = dest_addr;
    a.header.packet_id = 0x11112222; /* the ACK's OWN packet_id (flood dedup basis) */
    a.src_addr = src_addr;
    a.ack_packet_id = ack_packet_id;
    a.ack_flags = 0;
    a.rssi_at_dest = -50;
    a.hop_count = 1;
    a.relay_path[0] = src_addr;
    set_ack_seq(&a, 0x0102030405);
    ack_sign(&a);
    return a;
}

typedef struct {
    bool verified;
    bool relayed;
    bool consumed;
} ack_dispatch_result_t;

/*
 * Mirrors main/mesh_task.c's handle_ack (flood path) exactly:
 *   ack_verify -> (dest == self) consume via pending_ack_remove
 *              -> (dest != self, flood on) channel_flood_decide relay.
 * `buf`/`len` are the real serialized wire bytes; deserialization here goes
 * through the same bramble_ack_deserialize the firmware runs.
 */
static ack_dispatch_result_t dispatch_ack(const uint8_t* buf, uint16_t len, uint32_t self_addr,
                                          bool flood_transport_on, pending_ack_table_t* pending,
                                          bool budget_permits, uint32_t random_value) {
    ack_dispatch_result_t r = {0};
    bramble_ack_t ack;
    if (bramble_ack_deserialize(&ack, buf, len) != ESP_OK) {
        return r;
    }

    r.verified = ack_verify(&ack);
    if (!r.verified) {
        return r; /* bad MAC: dropped before ANY relay or consume effect */
    }

    if (ack.header.dest_addr == self_addr) {
        /* Original sender: correlate the flooded ACK to a pending message by
         * ack_packet_id and mark it delivered-confirmed. */
        r.consumed = pending_ack_remove(pending, ack.ack_packet_id);
        return r;
    }

    if (flood_transport_on) {
        bool is_own_echo = (ack.src_addr == self_addr);
        channel_flood_decision_t d =
            channel_flood_decide(ack.header.hop_limit, is_own_echo, budget_permits, random_value);
        r.relayed = d.should_relay;
    }
    /* flood OFF: reactive forward_ack (route lookup), not modeled here. */
    return r;
}

/* --- A valid flooded ACK not addressed to the relay IS flood-relayable --- */
void test_flood_ack_relay_valid(void) {
    bramble_ack_t a = make_signed_ack(DEST, SENDER, DATA_PKT_ID, 8);
    uint8_t buf[ACK_MAX_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_ack_serialize(&a, buf, sizeof(buf)));
    size_t len = bramble_ack_wire_size(&a);

    pending_ack_table_t pending;
    pending_ack_init(&pending);
    ack_dispatch_result_t r = dispatch_ack(buf, (uint16_t)len, RELAY, true, &pending, true, 42);
    TEST_ASSERT_TRUE(r.verified);
    TEST_ASSERT_TRUE(r.relayed);
    TEST_ASSERT_FALSE(r.consumed);
}

/* --- A bad-MAC ACK is NEVER rebroadcast, even with the toggle on, ample hop
 * budget, and free airtime. Non-vacuous: identical inputs with a good MAC
 * (test_flood_ack_relay_valid) DO relay; only the MAC differs. --- */
void test_flood_ack_bad_mac_never_relays(void) {
    bramble_ack_t a = make_signed_ack(DEST, SENDER, DATA_PKT_ID, 8);
    uint8_t buf[ACK_MAX_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_ack_serialize(&a, buf, sizeof(buf)));
    size_t len = bramble_ack_wire_size(&a);
    /* Corrupt the auth_hmac on the wire (fixed offset HEADER_SIZE + 11, see
     * packet.c). This is the only mutation vs. the valid case above. */
    buf[HEADER_SIZE + 11] ^= 0xFF;

    pending_ack_table_t pending;
    pending_ack_init(&pending);
    ack_dispatch_result_t r = dispatch_ack(buf, (uint16_t)len, RELAY, true, &pending, true, 42);
    TEST_ASSERT_FALSE(r.verified);
    TEST_ASSERT_FALSE(r.relayed);
    TEST_ASSERT_FALSE(r.consumed);
}

/* --- A keyless attacker who signs with the WRONG network key cannot get an
 * ACK flooded: ack_verify fails under the real (cleared/public) key. --- */
void test_flood_ack_wrong_key_never_relays(void) {
    uint8_t attacker_key[32];
    crypto_random(attacker_key, 32);
    network_key_set_provisioned(attacker_key);
    bramble_ack_t a = make_signed_ack(DEST, SENDER, DATA_PKT_ID, 8);
    uint8_t buf[ACK_MAX_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_ack_serialize(&a, buf, sizeof(buf)));
    size_t len = bramble_ack_wire_size(&a);

    network_key_clear(); /* relay does not hold the attacker's key */
    pending_ack_table_t pending;
    pending_ack_init(&pending);
    ack_dispatch_result_t r = dispatch_ack(buf, (uint16_t)len, RELAY, true, &pending, true, 42);
    TEST_ASSERT_FALSE(r.verified);
    TEST_ASSERT_FALSE(r.relayed);
}

/* --- A node hearing its OWN originated ACK echoed back does not rebroadcast
 * it (is_own_echo), exactly like the DATA flood's own-echo guard. --- */
void test_flood_ack_own_echo_not_relayed(void) {
    bramble_ack_t a = make_signed_ack(DEST, SENDER, DATA_PKT_ID, 8);
    uint8_t buf[ACK_MAX_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_ack_serialize(&a, buf, sizeof(buf)));
    size_t len = bramble_ack_wire_size(&a);

    pending_ack_table_t pending;
    pending_ack_init(&pending);
    /* self == DEST (the ACK's src): this node originated the ACK. */
    ack_dispatch_result_t r = dispatch_ack(buf, (uint16_t)len, DEST, true, &pending, true, 42);
    TEST_ASSERT_TRUE(r.verified);
    TEST_ASSERT_FALSE(r.relayed);
}

/* --- channel_flood_decide's existing gates apply to the flooded ACK: a
 * hop-exhausted ACK and a budget-denied ACK both do not relay. --- */
void test_flood_ack_hop_exhausted_not_relayed(void) {
    bramble_ack_t a = make_signed_ack(DEST, SENDER, DATA_PKT_ID, 1);
    uint8_t buf[ACK_MAX_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_ack_serialize(&a, buf, sizeof(buf)));
    size_t len = bramble_ack_wire_size(&a);

    pending_ack_table_t pending;
    pending_ack_init(&pending);
    ack_dispatch_result_t r = dispatch_ack(buf, (uint16_t)len, RELAY, true, &pending, true, 42);
    TEST_ASSERT_TRUE(r.verified);
    TEST_ASSERT_FALSE(r.relayed);
}

void test_flood_ack_budget_denied_not_relayed(void) {
    bramble_ack_t a = make_signed_ack(DEST, SENDER, DATA_PKT_ID, 8);
    uint8_t buf[ACK_MAX_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_ack_serialize(&a, buf, sizeof(buf)));
    size_t len = bramble_ack_wire_size(&a);

    pending_ack_table_t pending;
    pending_ack_init(&pending);
    ack_dispatch_result_t r = dispatch_ack(buf, (uint16_t)len, RELAY, true, &pending, false, 42);
    TEST_ASSERT_TRUE(r.verified);
    TEST_ASSERT_FALSE(r.relayed);
}

/* --- The correlation: the original sender, hearing a flooded ACK whose
 * ack_packet_id matches a pending message, marks it confirmed. NO route table
 * is consulted anywhere on this path. --- */
void test_flood_ack_sender_consumes_marks_confirmed(void) {
    pending_ack_table_t pending;
    pending_ack_init(&pending);
    uint8_t dummy[4] = {1, 2, 3, 4};
    /* The sender registered a pending confirmation for DATA_PKT_ID to DEST. */
    TEST_ASSERT_EQUAL(0, pending_ack_add(&pending, DATA_PKT_ID, DEST, MSG_TIER_NORMAL, dummy,
                                         sizeof(dummy), 1000));

    bramble_ack_t a = make_signed_ack(DEST, SENDER, DATA_PKT_ID, 8);
    uint8_t buf[ACK_MAX_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_ack_serialize(&a, buf, sizeof(buf)));
    size_t len = bramble_ack_wire_size(&a);

    /* self == SENDER: the flooded ACK is addressed to us; consume it. */
    ack_dispatch_result_t r = dispatch_ack(buf, (uint16_t)len, SENDER, true, &pending, true, 42);
    TEST_ASSERT_TRUE(r.verified);
    TEST_ASSERT_TRUE(r.consumed); /* pending message marked delivered-confirmed */
    TEST_ASSERT_FALSE(r.relayed);

    /* Idempotence: a second (duplicate flood copy) ACK does not re-confirm. */
    ack_dispatch_result_t r2 = dispatch_ack(buf, (uint16_t)len, SENDER, true, &pending, true, 42);
    TEST_ASSERT_TRUE(r2.verified);
    TEST_ASSERT_FALSE(r2.consumed);
}

/* --- Non-vacuous consume: a valid flooded ACK for us whose ack_packet_id
 * matches NO pending message does not falsely confirm anything. --- */
void test_flood_ack_sender_no_match_no_confirm(void) {
    pending_ack_table_t pending;
    pending_ack_init(&pending);
    uint8_t dummy[4] = {1, 2, 3, 4};
    TEST_ASSERT_EQUAL(0, pending_ack_add(&pending, 0xFEEDFACEu, DEST, MSG_TIER_NORMAL, dummy,
                                         sizeof(dummy), 1000));

    bramble_ack_t a = make_signed_ack(DEST, SENDER, DATA_PKT_ID, 8); /* different ack_packet_id */
    uint8_t buf[ACK_MAX_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_ack_serialize(&a, buf, sizeof(buf)));
    size_t len = bramble_ack_wire_size(&a);

    ack_dispatch_result_t r = dispatch_ack(buf, (uint16_t)len, SENDER, true, &pending, true, 42);
    TEST_ASSERT_TRUE(r.verified);
    TEST_ASSERT_FALSE(r.consumed);
}

/* --- The dispatch-gate suppression key is recomputed from the wire in
 * mesh_task.c as header.packet_id ^ (big-endian ack.src_addr). Prove that
 * wire recompute matches the host-order key handle_ack schedules with, so the
 * overheard-copy suppression can actually find the pending relay. --- */
void test_flood_ack_wire_suppression_key_matches(void) {
    bramble_ack_t a = make_signed_ack(DEST, SENDER, DATA_PKT_ID, 8);
    uint8_t buf[ACK_MAX_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_ack_serialize(&a, buf, sizeof(buf)));

    /* handle_ack (schedule side): host-order struct fields. */
    uint32_t schedule_key = a.header.packet_id ^ a.src_addr;

    /* dispatch gate (overheard side): src_addr read big-endian off the wire
     * at HEADER_SIZE, packet_id from the parsed header. */
    uint32_t wire_src = ((uint32_t)buf[HEADER_SIZE] << 24) |
                        ((uint32_t)buf[HEADER_SIZE + 1] << 16) |
                        ((uint32_t)buf[HEADER_SIZE + 2] << 8) | (uint32_t)buf[HEADER_SIZE + 3];
    bramble_header_t hdr;
    TEST_ASSERT_EQUAL(ESP_OK, bramble_header_deserialize(&hdr, buf, ACK_MAX_SIZE));
    uint32_t dispatch_key = hdr.packet_id ^ wire_src;

    TEST_ASSERT_EQUAL_UINT32(schedule_key, dispatch_key);
    TEST_ASSERT_EQUAL_UINT32(DEST, wire_src); /* sanity: big-endian read is correct */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_flood_ack_relay_valid);
    RUN_TEST(test_flood_ack_bad_mac_never_relays);
    RUN_TEST(test_flood_ack_wrong_key_never_relays);
    RUN_TEST(test_flood_ack_own_echo_not_relayed);
    RUN_TEST(test_flood_ack_hop_exhausted_not_relayed);
    RUN_TEST(test_flood_ack_budget_denied_not_relayed);
    RUN_TEST(test_flood_ack_sender_consumes_marks_confirmed);
    RUN_TEST(test_flood_ack_sender_no_match_no_confirm);
    RUN_TEST(test_flood_ack_wire_suppression_key_matches);
    return UNITY_END();
}
