/*
 * Identity attestation relay gate + delivery order (Phase 3, Part B).
 *
 * main/mesh_task.c is ESP-IDF-only and never host-compiled (see
 * test_flooded_ack.c / test_unicast_flood.c for the same rationale), so
 * this harness mirrors handle_identity_attestation with the REAL
 * component functions in the REAL order:
 *
 *   deserialize (exact length)
 *   -> ident_relay_verify        (cheap network-key MAC, FIRST)
 *   -> replay_check_and_add      (src_addr-scoped seq, the control window)
 *   -> dedup_check_and_add       (flood dedup, packet_id ^ src_addr)
 *   -> identity_store_handle_attestation (deliver: the ONLY Ed25519
 *                                 verify; runs regardless of relay result)
 *   -> channel_flood_decide      (the shared flood engine's decision)
 *   -> relay bytes: UNMODIFIED except the hop_limit decrement.
 *
 * The harness contains no verify/relay logic of its own; it wires the
 * real functions and observes outputs. Key security assertions:
 *   - bad MAC: dropped, NOT relayed, NOT pinned, and the Ed25519 verify
 *     is never even reached (the good-MAC control right before proves
 *     the same frame otherwise pins + relays);
 *   - replayed seq with a REWRITTEN packet_id (packet_id is not
 *     MAC-covered, so dedup alone cannot stop this) is dropped;
 *   - a good frame is delivered + relayed byte-identical except the
 *     hop_limit byte;
 *   - relays do not Ed-verify: a MAC-valid frame with a garbage sig
 *     still RELAYS (bounded keyed-insider residual) but is NOT pinned.
 */
#include "unity.h"
#include "esp_stubs.h"
#include "crypto.h"
#include "network_key.h"
#include "routing_auth.h"
#include "test_net_key.h"
#include "packet.h"
#include "dedup.h"
#include "replay_window.h"
#include "channel_flood.h"
#include "identity_store.h"

#include "../components/crypto/crypto_host.c"
#include "../components/network_key/network_key.c"
#include "../components/packet/packet.c"
#include "../components/routing_auth/routing_auth.c"
#include "../components/identity/identity_store.c"
/* identity.c (identity_endorsement_verify, called by identity_store.c since
 * trust-anchor P2) is a SEPARATE source in CMakeLists, not #included here: its
 * static put_be64/get_be64 would collide with packet.c's in this TU. */
#include "identity.h"

#include <string.h>

#define SELF_ADDR 0xCCCC0001u

/* Mandatory-provisioning (Task 2): provision the shared fixed key so the
 * relay-gate MAC path runs against a PROVISIONED node. */
void setUp(void) { bramble_test_provision_net_key(); }
void tearDown(void) { network_key_clear(); }

/* One receiving node's relay-relevant state, mirroring mesh_task.c's
 * s_control_replay / s_flood_dedup / s_identity_pins. */
typedef struct {
    replay_table_t control_replay;
    dedup_buffer_t flood_dedup;
    identity_store_t pins;
} node_state_t;

static void node_init(node_state_t* n) {
    replay_table_init(&n->control_replay);
    dedup_init(&n->flood_dedup);
    identity_store_init(&n->pins, 0);
}

typedef struct {
    bool mac_ok;
    bool replay_ok;
    bool relayed;
    identity_pin_result_t pin;
    uint8_t relay_buf[IDENTITY_ATTESTATION_SIZE];
} dispatch_result_t;

/* Mirrors main/mesh_task.c's handle_identity_attestation exactly (order
 * and all); budget_permits/random_value are explicit inputs the same way
 * test_flooded_ack.c passes them. */
static dispatch_result_t dispatch_attestation(node_state_t* n, const uint8_t* buf, uint16_t len,
                                              uint32_t self_addr, uint32_t now_ms,
                                              bool budget_permits, uint32_t random_value) {
    dispatch_result_t r;
    memset(&r, 0, sizeof(r));
    r.pin = IDENTITY_PIN_SELF; /* sentinel: never reached the store */

    bramble_identity_attestation_t att;
    if (bramble_identity_attestation_deserialize(&att, buf, len) != ESP_OK)
        return r;

    r.mac_ok = ident_relay_verify(&att) != 0;
    if (!r.mac_ok)
        return r; /* bad MAC: no replay-table touch, no pin, no relay */

    uint64_t seq = ((uint64_t)att.seq[0] << 40) | ((uint64_t)att.seq[1] << 32) |
                   ((uint64_t)att.seq[2] << 24) | ((uint64_t)att.seq[3] << 16) |
                   ((uint64_t)att.seq[4] << 8) | (uint64_t)att.seq[5];
    r.replay_ok =
        replay_check_and_add(&n->control_replay, att.src_addr, seq, now_ms) == REPLAY_ACCEPT;
    if (!r.replay_ok)
        return r; /* replayed: dropped before pin and relay */

    uint32_t flood_key = att.header.packet_id ^ att.src_addr;
    bool is_dup = dedup_check_and_add(&n->flood_dedup, flood_key, now_ms);

    /* Deliver regardless of the relay decision. These nodes set no anchor, so
     * the endorsement gate is skipped and epoch_ms is irrelevant (pass 0). */
    r.pin = identity_store_handle_attestation(&n->pins, &att, self_addr, now_ms, 0);

    bool is_own_echo = (att.src_addr == self_addr);
    channel_flood_decision_t flood = channel_flood_decide(
        att.header.hop_limit, is_dup || is_own_echo, budget_permits, random_value);
    r.relayed = flood.should_relay;
    if (flood.should_relay) {
        memcpy(r.relay_buf, buf, len);
        bramble_header_t relay_hdr = att.header;
        relay_hdr.hop_limit = flood.new_hop_limit;
        bramble_header_serialize(&relay_hdr, r.relay_buf, HEADER_SIZE);
    }
    return r;
}

/* Origination exactly as send_identity_attestation does it: canonical
 * message -> Ed25519 sign -> seq -> ident_relay_sign -> serialize.
 * Post-rebind, src_addr IS derive(ed_pub) (any other claim is refused by
 * the store's addr check, pinned in test_identity_store.c); the honest
 * origin address is therefore derived here and returned. */
static uint32_t originate(uint8_t out[IDENTITY_ATTESTATION_SIZE], uint64_t seq, uint32_t packet_id,
                          uint8_t hop_limit, uint8_t ed_pub_out[32]) {
    bramble_identity_attestation_t att;
    memset(&att, 0, sizeof(att));
    att.header.version = BRAMBLE_VERSION;
    att.header.type = PKT_TYPE_IDENTITY_ATTESTATION;
    att.header.hop_limit = hop_limit;
    att.header.dest_addr = 0xFFFFFFFFu;
    att.header.packet_id = packet_id;
    for (int i = 0; i < 32; i++)
        att.x25519_pub[i] = (uint8_t)(0x40 + i);
    uint8_t sk[BRAMBLE_ED25519_SECKEY_SIZE];
    TEST_ASSERT_EQUAL(0, crypto_ed25519_keypair(att.ed25519_pub, sk));
    att.src_addr = crypto_derive_address(att.ed25519_pub);
    if (ed_pub_out)
        memcpy(ed_pub_out, att.ed25519_pub, 32);
    uint8_t msg[IDENTITY_ATTESTATION_MSG_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_identity_attestation_signed_msg(&att, msg, sizeof(msg)));
    TEST_ASSERT_EQUAL(0, crypto_ed25519_sign(sk, msg, sizeof(msg), att.sig));
    att.seq[0] = (uint8_t)(seq >> 40);
    att.seq[1] = (uint8_t)(seq >> 32);
    att.seq[2] = (uint8_t)(seq >> 24);
    att.seq[3] = (uint8_t)(seq >> 16);
    att.seq[4] = (uint8_t)(seq >> 8);
    att.seq[5] = (uint8_t)seq;
    ident_relay_sign(&att);
    TEST_ASSERT_EQUAL(ESP_OK,
                      bramble_identity_attestation_serialize(&att, out, IDENTITY_ATTESTATION_SIZE));
    return att.src_addr;
}

/* Good frame: delivered (pinned) AND relayed, byte-identical except the
 * hop_limit byte (offset 3), which decremented by exactly one. */
static void test_good_frame_delivered_and_relayed_unmodified(void) {
    node_state_t n;
    node_init(&n);
    uint8_t frame[IDENTITY_ATTESTATION_SIZE];
    uint8_t ed_pub[32];
    uint32_t origin = originate(frame, 0x0102030405u, 0x11112222u, 8, ed_pub);

    dispatch_result_t r =
        dispatch_attestation(&n, frame, sizeof(frame), SELF_ADDR, 1000, true, 12345);
    TEST_ASSERT_TRUE(r.mac_ok);
    TEST_ASSERT_TRUE(r.replay_ok);
    TEST_ASSERT_EQUAL(IDENTITY_PIN_NEW, r.pin);
    TEST_ASSERT_TRUE(r.relayed);

    /* Pinned the frame's own keys. */
    const identity_pin_t* e = identity_store_lookup(&n.pins, origin);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ed_pub, e->ed25519_pub, 32);

    /* Relay bytes: hop_limit (offset 3) decremented, everything else
     * byte-identical, including the MAC and seq (never re-drawn). */
    TEST_ASSERT_EQUAL_HEX8(7, r.relay_buf[3]);
    frame[3] = 7; /* normalize the one legitimately mutated byte */
    TEST_ASSERT_EQUAL_HEX8_ARRAY(frame, r.relay_buf, IDENTITY_ATTESTATION_SIZE);
}

/* Bad MAC: dropped at the FIRST gate. Not relayed, not pinned, replay
 * window untouched. Non-vacuous: the same frame with a good MAC (control
 * above and re-check here) pins + relays. */
static void test_bad_mac_dropped_not_relayed_not_pinned(void) {
    node_state_t n;
    node_init(&n);
    uint8_t frame[IDENTITY_ATTESTATION_SIZE];
    uint32_t origin = originate(frame, 0x0102030405u, 0x11112222u, 8, NULL);

    /* Control: good copy on a fresh node pins + relays. */
    node_state_t control;
    node_init(&control);
    dispatch_result_t ok =
        dispatch_attestation(&control, frame, sizeof(frame), SELF_ADDR, 1000, true, 12345);
    TEST_ASSERT_TRUE(ok.relayed);
    TEST_ASSERT_EQUAL(IDENTITY_PIN_NEW, ok.pin);

    /* Flip one MAC byte (offset 144): keyless forgery. */
    frame[144] ^= 0x01;
    dispatch_result_t r =
        dispatch_attestation(&n, frame, sizeof(frame), SELF_ADDR, 1000, true, 12345);
    TEST_ASSERT_FALSE(r.mac_ok);
    TEST_ASSERT_FALSE(r.relayed);
    TEST_ASSERT_NULL(identity_store_lookup(&n.pins, origin));
    /* The replay window was never fed: the SAME (src, seq) still passes
     * later when the genuine frame arrives (a bad-MAC frame must not be
     * able to pre-burn a victim's seq). */
    frame[144] ^= 0x01; /* restore */
    dispatch_result_t r2 =
        dispatch_attestation(&n, frame, sizeof(frame), SELF_ADDR, 2000, true, 12345);
    TEST_ASSERT_TRUE(r2.replay_ok);
    TEST_ASSERT_EQUAL(IDENTITY_PIN_NEW, r2.pin);
}

/* Replay: the exact frame re-injected with a REWRITTEN packet_id
 * (header is not MAC-covered, so this dodges the flood dedup key) is
 * still dropped by the seq window, before pin and relay. */
static void test_replayed_seq_dropped_despite_fresh_packet_id(void) {
    node_state_t n;
    node_init(&n);
    uint8_t frame[IDENTITY_ATTESTATION_SIZE];
    originate(frame, 0x0102030405u, 0x11112222u, 8, NULL);

    dispatch_result_t first =
        dispatch_attestation(&n, frame, sizeof(frame), SELF_ADDR, 1000, true, 12345);
    TEST_ASSERT_TRUE(first.relayed); /* control */

    /* Attacker rewrites packet_id (bytes 8..11) to dodge dedup and
     * re-injects the otherwise identical frame. */
    frame[8] ^= 0xFF;
    dispatch_result_t r =
        dispatch_attestation(&n, frame, sizeof(frame), SELF_ADDR, 2000, true, 12345);
    TEST_ASSERT_TRUE(r.mac_ok);     /* MAC still valid (header excluded)... */
    TEST_ASSERT_FALSE(r.replay_ok); /* ...but the seq window kills it */
    TEST_ASSERT_FALSE(r.relayed);
    /* And the pin state did not churn: still exactly one entry. */
    TEST_ASSERT_EQUAL(1, identity_store_count(&n.pins));
}

/* Duplicate (same packet_id, e.g. heard via two relays): flood dedup
 * suppresses the re-relay; a FRESH origination (new seq + new packet_id)
 * from the same node relays again and refreshes the pin. */
static void test_duplicate_suppressed_fresh_origination_relays(void) {
    node_state_t n;
    node_init(&n);
    uint8_t frame[IDENTITY_ATTESTATION_SIZE];
    uint32_t origin = originate(frame, 0x0102030405u, 0x11112222u, 8, NULL);

    dispatch_result_t first =
        dispatch_attestation(&n, frame, sizeof(frame), SELF_ADDR, 1000, true, 12345);
    TEST_ASSERT_TRUE(first.relayed);

    /* Note: an exact duplicate is caught by the replay window here (same
     * seq) even before flood dedup; in firmware the dispatch s_dedup gate
     * catches it earlier still. Either way: no second relay. */
    dispatch_result_t dup =
        dispatch_attestation(&n, frame, sizeof(frame), SELF_ADDR, 1100, true, 12345);
    TEST_ASSERT_FALSE(dup.relayed);

    /* The next CADENCE send is a new origination: same identity/keys,
     * fresh seq + packet_id. It relays and refreshes (not re-pins). */
    uint8_t frame2[IDENTITY_ATTESTATION_SIZE];
    bramble_identity_attestation_t att;
    TEST_ASSERT_EQUAL(ESP_OK, bramble_identity_attestation_deserialize(&att, frame, sizeof(frame)));
    att.header.packet_id = 0x33334444u;
    att.seq[5] = 0x06; /* next counter draw */
    ident_relay_sign(&att);
    TEST_ASSERT_EQUAL(ESP_OK, bramble_identity_attestation_serialize(&att, frame2, sizeof(frame2)));
    dispatch_result_t next =
        dispatch_attestation(&n, frame2, sizeof(frame2), SELF_ADDR, 2000, true, 12345);
    TEST_ASSERT_TRUE(next.relayed);
    TEST_ASSERT_EQUAL(IDENTITY_PIN_REFRESHED, next.pin);
    const identity_pin_t* e = identity_store_lookup(&n.pins, origin);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT32(2000, e->last_confirmed_ms);
}

/* Relays never Ed-verify (documented residual): a MAC-valid frame whose
 * Ed25519 sig is garbage still RELAYS (keyed-insider noise, bounded by
 * budget) but is NOT pinned, and the receiver counts it. */
static void test_mac_valid_sig_invalid_relays_but_never_pins(void) {
    node_state_t n;
    node_init(&n);
    bramble_identity_attestation_t att;
    uint8_t frame[IDENTITY_ATTESTATION_SIZE];
    uint32_t origin = originate(frame, 0x0102030405u, 0x11112222u, 8, NULL);
    TEST_ASSERT_EQUAL(ESP_OK, bramble_identity_attestation_deserialize(&att, frame, sizeof(frame)));

    /* Keyed insider: garbage sig, then a VALID relay MAC over it. */
    memset(att.sig, 0xEE, sizeof(att.sig));
    ident_relay_sign(&att);
    TEST_ASSERT_EQUAL(ESP_OK, bramble_identity_attestation_serialize(&att, frame, sizeof(frame)));

    dispatch_result_t r =
        dispatch_attestation(&n, frame, sizeof(frame), SELF_ADDR, 1000, true, 12345);
    TEST_ASSERT_TRUE(r.mac_ok);
    TEST_ASSERT_TRUE(r.relayed); /* relays check the MAC only */
    TEST_ASSERT_EQUAL(IDENTITY_PIN_BAD_SIG, r.pin);
    TEST_ASSERT_NULL(identity_store_lookup(&n.pins, origin));
    TEST_ASSERT_EQUAL_UINT32(1, n.pins.sig_failures);
}

/* Own echo: a node hearing its own attestation flooded back neither
 * re-relays it nor pins itself. */
static void test_own_echo_not_relayed_not_pinned(void) {
    node_state_t n;
    node_init(&n);
    uint8_t frame[IDENTITY_ATTESTATION_SIZE];
    uint32_t origin = originate(frame, 0x0102030405u, 0x11112222u, 7, NULL);

    dispatch_result_t r =
        dispatch_attestation(&n, frame, sizeof(frame), /*self=*/origin, 1000, true, 12345);
    TEST_ASSERT_TRUE(r.mac_ok);
    TEST_ASSERT_EQUAL(IDENTITY_PIN_SELF, r.pin);
    TEST_ASSERT_FALSE(r.relayed);
    TEST_ASSERT_NULL(identity_store_lookup(&n.pins, origin));
}

/* Budget-denied and hop-limit-exhausted frames still DELIVER (pin), just
 * do not relay: delivery is unconditional on the relay decision. */
static void test_no_budget_or_exhausted_hops_still_delivers(void) {
    node_state_t n;
    node_init(&n);
    uint8_t frame[IDENTITY_ATTESTATION_SIZE];
    originate(frame, 0x0102030405u, 0x11112222u, 8, NULL);

    dispatch_result_t r =
        dispatch_attestation(&n, frame, sizeof(frame), SELF_ADDR, 1000, /*budget=*/false, 12345);
    TEST_ASSERT_FALSE(r.relayed);
    TEST_ASSERT_EQUAL(IDENTITY_PIN_NEW, r.pin);

    node_state_t n2;
    node_init(&n2);
    uint8_t frame2[IDENTITY_ATTESTATION_SIZE];
    originate(frame2, 0x0102030405u, 0x2222u, /*hop_limit=*/1, NULL);
    dispatch_result_t r2 =
        dispatch_attestation(&n2, frame2, sizeof(frame2), SELF_ADDR, 1000, true, 12345);
    TEST_ASSERT_FALSE(r2.relayed); /* hop budget exhausted at this hop */
    TEST_ASSERT_EQUAL(IDENTITY_PIN_NEW, r2.pin);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_good_frame_delivered_and_relayed_unmodified);
    RUN_TEST(test_bad_mac_dropped_not_relayed_not_pinned);
    RUN_TEST(test_replayed_seq_dropped_despite_fresh_packet_id);
    RUN_TEST(test_duplicate_suppressed_fresh_origination_relays);
    RUN_TEST(test_mac_valid_sig_invalid_relays_but_never_pins);
    RUN_TEST(test_own_echo_not_relayed_not_pinned);
    RUN_TEST(test_no_budget_or_exhausted_hops_still_delivers);
    return UNITY_END();
}
