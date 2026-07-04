#include "unity.h"
#include "esp_stubs.h"
#include "crypto.h"
#include "network_key.h"
#include "routing_auth.h"
#include "packet.h"
#include "dedup.h"
#include "channel_flood.h"

#include "../components/crypto/crypto_host.c"
#include "../components/network_key/network_key.c"
#include "../components/packet/packet.c"
#include "../components/routing_auth/routing_auth.c"

#include <string.h>

/*
 * Flooding F1 hardening (final whole-branch review finding): the rebroadcast-
 * suppression overheard-copy counter must act ONLY on AUTHENTICATED duplicate
 * frames. main/mesh_task.c inserts the s_dedup key on the FIRST copy of a
 * flooded frame BEFORE the network-key MAC is verified, so a keyless party can
 * replay a garbage-MAC duplicate carrying a matching plaintext packet_id +
 * src_addr, land in the dispatch-gate duplicate branch, and bump `heard` to
 * FLOOD_SUPPRESS_AFTER -- cancelling a legitimate node's genuine pending relay
 * and punching a targeted coverage hole in a sparse mesh.
 *
 * The fix gates BOTH channel_flood_note_overheard calls (DATA via
 * data_auth_verify, flooded ACK via bramble_ack_deserialize + ack_verify) on
 * the copy's network-key MAC verifying first. mesh_task.c is 3800+ ESP-IDF-only
 * lines and is never host-compiled (see test_unicast_flood.c / test_flooded_
 * ack.c for the same rationale), so this harness mirrors the two dispatch-gate
 * suppression branches using the REAL component functions in the REAL order:
 * verify the MAC, and ONLY on success call the real channel_flood_note_
 * overheard against a real pending_flood_relay queue. The non-vacuous pair for
 * each frame type flips exactly one auth byte between the valid and forged
 * copy (the test_unicast_flood.c / test_flooded_ack.c pattern), proving the
 * ONLY thing separating "counts + suppresses" from "ignored" is MAC validity.
 */

#define SELF 0xAAAAAAAAu
#define OTHER_DEST 0xCCCCCCCCu
#define ORIGIN 0x0A0A0A0Au
#define DATA_PKT_ID 0x1234u

/* the flooded-ACK origin (its ACK src_addr) and destination sender */
#define ACK_DEST 0xDDDDDDDDu
#define ACK_SENDER 0xBBBBBBBBu
#define ACK_RELAY 0xCCCCCCCCu
#define ACK_DATA_PKT_ID 0x0BADF00Du

void setUp(void) { network_key_clear(); }
void tearDown(void) { network_key_clear(); }

/* Seed the pending flood relay queue with ONE genuine, still-jittering relay
 * for flood_key (heard == 0, used == true) -- exactly what schedule_flood_relay
 * records after channel_flood_decide queues a rebroadcast. This is the relay a
 * forged overheard copy must NOT be able to cancel. */
static void seed_pending_relay(pending_flood_relay_t* queue, uint32_t flood_key) {
    memset(queue, 0, sizeof(pending_flood_relay_t) * FLOOD_RELAY_QUEUE_CAPACITY);
    queue[0].used = true;
    queue[0].flood_key = flood_key;
    queue[0].heard = 0;
}

/* -------------------------- DATA flood suppression -------------------------- */

static bramble_header_t make_data_header(uint32_t dest, uint32_t pkt_id, uint8_t hop_limit) {
    bramble_header_t h = {0};
    h.version = BRAMBLE_VERSION;
    h.type = PKT_TYPE_DATA;
    h.flags = FLAG_ENCRYPT;
    h.hop_limit = hop_limit;
    h.dest_addr = dest;
    h.packet_id = pkt_id;
    return h;
}

/* Mirrors main/mesh_task.c's dispatch-gate DATA suppression branch exactly:
 * data_auth_verify on the overheard copy FIRST; only a genuine copy reaches
 * channel_flood_note_overheard. Returns true iff this copy cancelled the
 * pending relay (heard reached FLOOD_SUPPRESS_AFTER). */
static bool data_overheard(pending_flood_relay_t* queue, const bramble_header_t* h,
                           uint32_t src_addr, const uint8_t hmac[8]) {
    if (!data_auth_verify(h, src_addr, hmac)) {
        return false;
    }
    uint32_t flood_key = h->packet_id ^ src_addr;
    return channel_flood_note_overheard(queue, FLOOD_RELAY_QUEUE_CAPACITY, flood_key);
}

/* A forged (bad-MAC) duplicate DATA copy must NOT count toward suppression and
 * must NOT cancel the genuine pending relay. */
void test_data_forged_dup_does_not_count(void) {
    pending_flood_relay_t queue[FLOOD_RELAY_QUEUE_CAPACITY];
    seed_pending_relay(queue, DATA_PKT_ID ^ ORIGIN);

    bramble_header_t h = make_data_header(OTHER_DEST, DATA_PKT_ID, 8);
    uint8_t good[8];
    data_auth_sign(&h, ORIGIN, good);
    uint8_t bad[8];
    memcpy(bad, good, 8);
    bad[0] ^= 0xFF; /* the ONLY difference vs. the valid copy below */

    bool suppressed = data_overheard(queue, &h, ORIGIN, bad);
    TEST_ASSERT_FALSE(suppressed);
    TEST_ASSERT_TRUE(queue[0].used);            /* relay still pending */
    TEST_ASSERT_EQUAL_UINT8(0, queue[0].heard); /* not counted */
}

/* A single flipped auth byte is the only difference: the VALID duplicate DOES
 * count, and FLOOD_SUPPRESS_AFTER valid copies cancel the pending relay. */
void test_data_valid_dup_counts_and_suppresses(void) {
    pending_flood_relay_t queue[FLOOD_RELAY_QUEUE_CAPACITY];
    seed_pending_relay(queue, DATA_PKT_ID ^ ORIGIN);

    bramble_header_t h = make_data_header(OTHER_DEST, DATA_PKT_ID, 8);
    uint8_t good[8];
    data_auth_sign(&h, ORIGIN, good);

    /* First valid overheard copy: counted (heard 0 -> 1), not yet cancelled. */
    bool s1 = data_overheard(queue, &h, ORIGIN, good);
    TEST_ASSERT_FALSE(s1);
    TEST_ASSERT_TRUE(queue[0].used);
    TEST_ASSERT_EQUAL_UINT8(1, queue[0].heard);

    /* Second valid overheard copy reaches FLOOD_SUPPRESS_AFTER (== 2): cancel. */
    bool s2 = data_overheard(queue, &h, ORIGIN, good);
    TEST_ASSERT_TRUE(s2);
    TEST_ASSERT_FALSE(queue[0].used); /* genuine relay cancelled */
}

/* Regression combining both: a forged copy interleaved with the genuine flood
 * cannot advance suppression -- two forged copies leave the relay live, and it
 * still takes two GENUINE copies to cancel. Proves a keyless party cannot
 * cancel a legitimate node's relay even by flooding many forged duplicates. */
void test_data_forged_copies_never_reach_threshold(void) {
    pending_flood_relay_t queue[FLOOD_RELAY_QUEUE_CAPACITY];
    seed_pending_relay(queue, DATA_PKT_ID ^ ORIGIN);

    bramble_header_t h = make_data_header(OTHER_DEST, DATA_PKT_ID, 8);
    uint8_t good[8];
    data_auth_sign(&h, ORIGIN, good);
    uint8_t bad[8];
    memcpy(bad, good, 8);
    bad[7] ^= 0x01;

    /* Any number of forged copies: never counted, relay stays live. */
    TEST_ASSERT_FALSE(data_overheard(queue, &h, ORIGIN, bad));
    TEST_ASSERT_FALSE(data_overheard(queue, &h, ORIGIN, bad));
    TEST_ASSERT_FALSE(data_overheard(queue, &h, ORIGIN, bad));
    TEST_ASSERT_TRUE(queue[0].used);
    TEST_ASSERT_EQUAL_UINT8(0, queue[0].heard);

    /* The genuine flood still cancels on its own two copies, unaffected. */
    TEST_ASSERT_FALSE(data_overheard(queue, &h, ORIGIN, good));
    TEST_ASSERT_TRUE(data_overheard(queue, &h, ORIGIN, good));
    TEST_ASSERT_FALSE(queue[0].used);
}

/* -------------------------- flooded ACK suppression ------------------------- */

static void set_ack_seq(bramble_ack_t* a, uint64_t seq) {
    a->seq[0] = (uint8_t)(seq >> 40);
    a->seq[1] = (uint8_t)(seq >> 32);
    a->seq[2] = (uint8_t)(seq >> 24);
    a->seq[3] = (uint8_t)(seq >> 16);
    a->seq[4] = (uint8_t)(seq >> 8);
    a->seq[5] = (uint8_t)seq;
}

static bramble_ack_t make_signed_ack(uint32_t src_addr, uint32_t dest_addr, uint32_t ack_packet_id,
                                     uint8_t hop_limit) {
    bramble_ack_t a = {0};
    a.header.version = BRAMBLE_VERSION;
    a.header.type = PKT_TYPE_ACK;
    a.header.hop_limit = hop_limit;
    a.header.dest_addr = dest_addr;
    a.header.packet_id = 0x11112222;
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

/* Mirrors main/mesh_task.c's dispatch-gate flooded-ACK suppression branch
 * exactly: bramble_ack_deserialize + ack_verify FIRST; only a genuine copy
 * reaches channel_flood_note_overheard, keyed the same way (packet_id ^
 * src_addr). */
static bool ack_overheard(pending_flood_relay_t* queue, const uint8_t* buf, uint16_t len) {
    bramble_ack_t ack;
    if (bramble_ack_deserialize(&ack, buf, len) != ESP_OK) {
        return false;
    }
    if (!ack_verify(&ack)) {
        return false;
    }
    uint32_t flood_key = ack.header.packet_id ^ ack.src_addr;
    return channel_flood_note_overheard(queue, FLOOD_RELAY_QUEUE_CAPACITY, flood_key);
}

/* A forged (bad-MAC) duplicate flooded-ACK copy must NOT count toward
 * suppression and must NOT cancel the genuine pending ACK relay. */
void test_ack_forged_dup_does_not_count(void) {
    bramble_ack_t a = make_signed_ack(ACK_DEST, ACK_SENDER, ACK_DATA_PKT_ID, 8);
    uint8_t buf[ACK_MAX_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_ack_serialize(&a, buf, sizeof(buf)));
    uint16_t len = (uint16_t)bramble_ack_wire_size(&a);

    pending_flood_relay_t queue[FLOOD_RELAY_QUEUE_CAPACITY];
    seed_pending_relay(queue, a.header.packet_id ^ a.src_addr);

    /* Corrupt the auth_hmac on the wire (fixed offset HEADER_SIZE + 11, see
     * packet.c) -- the ONLY difference vs. the valid copy below. */
    buf[HEADER_SIZE + 11] ^= 0xFF;

    bool suppressed = ack_overheard(queue, buf, len);
    TEST_ASSERT_FALSE(suppressed);
    TEST_ASSERT_TRUE(queue[0].used);
    TEST_ASSERT_EQUAL_UINT8(0, queue[0].heard);
}

/* The valid flooded-ACK duplicate DOES count, and FLOOD_SUPPRESS_AFTER valid
 * copies cancel the pending ACK relay. Only the flipped auth byte in the test
 * above separates this from the ignored case. */
void test_ack_valid_dup_counts_and_suppresses(void) {
    bramble_ack_t a = make_signed_ack(ACK_DEST, ACK_SENDER, ACK_DATA_PKT_ID, 8);
    uint8_t buf[ACK_MAX_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_ack_serialize(&a, buf, sizeof(buf)));
    uint16_t len = (uint16_t)bramble_ack_wire_size(&a);

    pending_flood_relay_t queue[FLOOD_RELAY_QUEUE_CAPACITY];
    seed_pending_relay(queue, a.header.packet_id ^ a.src_addr);

    bool s1 = ack_overheard(queue, buf, len);
    TEST_ASSERT_FALSE(s1);
    TEST_ASSERT_TRUE(queue[0].used);
    TEST_ASSERT_EQUAL_UINT8(1, queue[0].heard);

    bool s2 = ack_overheard(queue, buf, len);
    TEST_ASSERT_TRUE(s2);
    TEST_ASSERT_FALSE(queue[0].used);
}

/* A keyless attacker signing with the WRONG network key cannot advance ACK
 * suppression either: ack_verify fails under the real (cleared/public) key. */
void test_ack_wrong_key_does_not_count(void) {
    uint8_t attacker_key[32];
    crypto_random(attacker_key, 32);
    network_key_set_provisioned(attacker_key);
    bramble_ack_t a = make_signed_ack(ACK_DEST, ACK_SENDER, ACK_DATA_PKT_ID, 8);
    uint8_t buf[ACK_MAX_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_ack_serialize(&a, buf, sizeof(buf)));
    uint16_t len = (uint16_t)bramble_ack_wire_size(&a);

    network_key_clear(); /* the relay does not hold the attacker's key */
    pending_flood_relay_t queue[FLOOD_RELAY_QUEUE_CAPACITY];
    seed_pending_relay(queue, a.header.packet_id ^ a.src_addr);

    bool suppressed = ack_overheard(queue, buf, len);
    TEST_ASSERT_FALSE(suppressed);
    TEST_ASSERT_TRUE(queue[0].used);
    TEST_ASSERT_EQUAL_UINT8(0, queue[0].heard);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_data_forged_dup_does_not_count);
    RUN_TEST(test_data_valid_dup_counts_and_suppresses);
    RUN_TEST(test_data_forged_copies_never_reach_threshold);
    RUN_TEST(test_ack_forged_dup_does_not_count);
    RUN_TEST(test_ack_valid_dup_counts_and_suppresses);
    RUN_TEST(test_ack_wrong_key_does_not_count);
    return UNITY_END();
}
