#include "unity.h"
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

/* channel_flood.c, discovery.c, routing.c and dedup.c are compiled as
 * separate CMake sources (see test/CMakeLists.txt), not #included here: both
 * discovery.c and routing_auth.c define a static ct_eq helper, and this file
 * already pulls in routing_auth.c by #include, so text-including discovery.c
 * too (test_channel_flood.c's pattern, which never also needs routing_auth.c)
 * would collide on that symbol name in the same translation unit. */

/*
 * Flooding F1 Task 1: unicast DATA floods (relay + deliver) behind the
 * s_flood_transport toggle. mesh_task.c is 3800+ ESP-IDF-only lines and is
 * never #include'd by a host test (see test_data_auth.c's identical
 * rationale), so this mirrors mesh_process_rx_packet's PKT_TYPE_DATA
 * dispatch + handle_data's flood-relay gate using the REAL component
 * functions in the REAL order: data_auth_verify first, then (only for a
 * frame that is flood-eligible) dedup_check_and_add and channel_flood_decide
 * -- the exact same channel_flood_decide the broadcast flood already uses,
 * per the F1 plan's "no second flood implementation" constraint. This
 * harness contains no independent relay/suppression logic of its own.
 */

#define SELF 0xAAAAAAAAu
#define OTHER_DEST 0xCCCCCCCCu
#define ORIGIN 0x0A0A0A0Au

void setUp(void) { network_key_clear(); }
void tearDown(void) { network_key_clear(); }

static bramble_header_t make_unicast_header(uint32_t dest, uint32_t pkt_id, uint8_t hop_limit) {
    bramble_header_t h = {0};
    h.version = BRAMBLE_VERSION;
    h.type = PKT_TYPE_DATA;
    h.flags = FLAG_ENCRYPT;
    h.hop_limit = hop_limit;
    h.dest_addr = dest;
    h.packet_id = pkt_id;
    return h;
}

typedef struct {
    bool auth_ok;
    bool relayed;
    bool delivered;
} dispatch_result_t;

/*
 * Mirrors main/mesh_task.c exactly:
 *   - mesh_process_rx_packet's PKT_TYPE_DATA case: data_auth_verify BEFORE
 *     anything else; a failing frame is dropped outright (no relay, no
 *     deliver, no dedup lookup).
 *   - a dest == self frame is DATA_RX_DELIVER: handle_data delivers it and
 *     never enters the flood-relay block (dest_is_self excludes it from
 *     handle_data's flood_eligible condition).
 *   - a dest == someone-else frame is DATA_RX_FORWARD: reactive (toggle off)
 *     calls forward_data_packet (not modeled here, out of scope for this
 *     harness -- see test_forwarding.c for that path); flood (toggle on)
 *     enters the SAME relay block broadcast DATA uses, keyed on
 *     packet_id ^ src_addr in s_flood_dedup, gated by channel_flood_decide.
 */
static dispatch_result_t dispatch_unicast(dedup_buffer_t* flood_dedup, bool flood_transport_on,
                                          const bramble_header_t* h, uint32_t src_addr,
                                          const uint8_t hmac[8], bool budget_permits,
                                          uint32_t random_value, uint32_t now_ms) {
    dispatch_result_t r = {0};

    r.auth_ok = data_auth_verify(h, src_addr, hmac);
    if (!r.auth_ok) {
        return r;
    }

    bool dest_is_broadcast = (h->dest_addr == 0xFFFFFFFF);
    bool dest_is_self = (h->dest_addr == SELF);

    if (dest_is_self) {
        r.delivered = true;
        return r;
    }

    bool flood_eligible = dest_is_broadcast || (flood_transport_on && !dest_is_self);
    if (!flood_eligible) {
        return r; /* reactive route-lookup forward, not modeled by this harness */
    }

    uint32_t flood_key = h->packet_id ^ src_addr;
    bool is_dup = dedup_check_and_add(flood_dedup, flood_key, now_ms);
    bool is_own_echo = (src_addr == SELF);

    channel_flood_decision_t d =
        channel_flood_decide(h->hop_limit, is_dup || is_own_echo, budget_permits, random_value);
    r.relayed = d.should_relay;
    return r;
}

/* --- Toggle ON: a valid unicast-for-someone-else frame floods --- */

void test_flood_on_relays_valid_unicast_frame(void) {
    dedup_buffer_t dd;
    dedup_init(&dd);
    bramble_header_t h = make_unicast_header(OTHER_DEST, 0x1234, 8);
    uint8_t hmac[8];
    data_auth_sign(&h, ORIGIN, hmac);

    dispatch_result_t r = dispatch_unicast(&dd, true, &h, ORIGIN, hmac, true, 42, 1000);
    TEST_ASSERT_TRUE(r.auth_ok);
    TEST_ASSERT_TRUE(r.relayed);
    TEST_ASSERT_FALSE(r.delivered);
}

/* --- Toggle OFF: reactive behavior is unchanged (no relay attempted here;
 * the real dispatch would call forward_data_packet instead) --- */

void test_flood_off_never_relays(void) {
    dedup_buffer_t dd;
    dedup_init(&dd);
    bramble_header_t h = make_unicast_header(OTHER_DEST, 0x1234, 8);
    uint8_t hmac[8];
    data_auth_sign(&h, ORIGIN, hmac);

    dispatch_result_t r = dispatch_unicast(&dd, false, &h, ORIGIN, hmac, true, 42, 1000);
    TEST_ASSERT_TRUE(r.auth_ok);
    TEST_ASSERT_FALSE(r.relayed);
    TEST_ASSERT_FALSE(r.delivered);
}

/* --- dest == self always delivers, regardless of the toggle, and never
 * relays (no rebroadcast needed once you are the destination) --- */

void test_flood_on_dest_self_delivers_not_relays(void) {
    dedup_buffer_t dd;
    dedup_init(&dd);
    bramble_header_t h = make_unicast_header(SELF, 0x1234, 8);
    uint8_t hmac[8];
    data_auth_sign(&h, ORIGIN, hmac);

    dispatch_result_t r = dispatch_unicast(&dd, true, &h, ORIGIN, hmac, true, 42, 1000);
    TEST_ASSERT_TRUE(r.auth_ok);
    TEST_ASSERT_TRUE(r.delivered);
    TEST_ASSERT_FALSE(r.relayed);
}

void test_flood_off_dest_self_still_delivers(void) {
    dedup_buffer_t dd;
    dedup_init(&dd);
    bramble_header_t h = make_unicast_header(SELF, 0x1234, 8);
    uint8_t hmac[8];
    data_auth_sign(&h, ORIGIN, hmac);

    dispatch_result_t r = dispatch_unicast(&dd, false, &h, ORIGIN, hmac, true, 42, 1000);
    TEST_ASSERT_TRUE(r.delivered);
    TEST_ASSERT_FALSE(r.relayed);
}

/* --- Authenticated flood: a bad auth_hmac is NEVER relayed, even with the
 * toggle on, ample hop budget, and free airtime. This is the F1 plan's
 * "authenticated flood" global constraint: keyless traffic never
 * propagates. --- */

void test_flood_on_bad_auth_hmac_never_relays(void) {
    dedup_buffer_t dd;
    dedup_init(&dd);
    bramble_header_t h = make_unicast_header(OTHER_DEST, 0x1234, 8);
    uint8_t bad_hmac[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    dispatch_result_t r = dispatch_unicast(&dd, true, &h, ORIGIN, bad_hmac, true, 42, 1000);
    TEST_ASSERT_FALSE(r.auth_ok);
    TEST_ASSERT_FALSE(r.relayed);
    TEST_ASSERT_FALSE(r.delivered);
}

void test_flood_on_forged_src_never_relays(void) {
    /* Same attack shape as test_data_auth.c's keyless-DATA regression: an
     * attacker claims src_addr = ORIGIN but cannot produce ORIGIN's HMAC
     * without the network key. */
    dedup_buffer_t dd;
    dedup_init(&dd);
    bramble_header_t h = make_unicast_header(OTHER_DEST, 0x1234, 8);
    uint8_t hmac_for_attacker[8];
    data_auth_sign(&h, 0x0E0E0E0Eu, hmac_for_attacker); /* signed as ATTACKER, not ORIGIN */

    dispatch_result_t r =
        dispatch_unicast(&dd, true, &h, ORIGIN, hmac_for_attacker, true, 42, 1000);
    TEST_ASSERT_FALSE(r.auth_ok);
    TEST_ASSERT_FALSE(r.relayed);
}

/* --- channel_flood_decide's existing rules apply unchanged to unicast:
 * hop-limit exhaustion and duplicate suppression both still gate the
 * relay, exactly like the broadcast flood (test_channel_flood.c). --- */

void test_flood_on_hop_limit_exhausted_does_not_relay(void) {
    dedup_buffer_t dd;
    dedup_init(&dd);
    bramble_header_t h = make_unicast_header(OTHER_DEST, 0x1234, 1);
    uint8_t hmac[8];
    data_auth_sign(&h, ORIGIN, hmac);

    dispatch_result_t r = dispatch_unicast(&dd, true, &h, ORIGIN, hmac, true, 42, 1000);
    TEST_ASSERT_TRUE(r.auth_ok);
    TEST_ASSERT_FALSE(r.relayed);
}

void test_flood_on_duplicate_does_not_relay_twice(void) {
    dedup_buffer_t dd;
    dedup_init(&dd);
    bramble_header_t h = make_unicast_header(OTHER_DEST, 0x1234, 8);
    uint8_t hmac[8];
    data_auth_sign(&h, ORIGIN, hmac);

    dispatch_result_t r1 = dispatch_unicast(&dd, true, &h, ORIGIN, hmac, true, 42, 1000);
    TEST_ASSERT_TRUE(r1.relayed);

    dispatch_result_t r2 = dispatch_unicast(&dd, true, &h, ORIGIN, hmac, true, 42, 1000);
    TEST_ASSERT_FALSE(r2.relayed);
}

void test_flood_on_budget_denied_does_not_relay(void) {
    dedup_buffer_t dd;
    dedup_init(&dd);
    bramble_header_t h = make_unicast_header(OTHER_DEST, 0x1234, 8);
    uint8_t hmac[8];
    data_auth_sign(&h, ORIGIN, hmac);

    dispatch_result_t r = dispatch_unicast(&dd, true, &h, ORIGIN, hmac, false, 42, 1000);
    TEST_ASSERT_TRUE(r.auth_ok);
    TEST_ASSERT_FALSE(r.relayed);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_flood_on_relays_valid_unicast_frame);
    RUN_TEST(test_flood_off_never_relays);
    RUN_TEST(test_flood_on_dest_self_delivers_not_relays);
    RUN_TEST(test_flood_off_dest_self_still_delivers);
    RUN_TEST(test_flood_on_bad_auth_hmac_never_relays);
    RUN_TEST(test_flood_on_forged_src_never_relays);
    RUN_TEST(test_flood_on_hop_limit_exhausted_does_not_relay);
    RUN_TEST(test_flood_on_duplicate_does_not_relay_twice);
    RUN_TEST(test_flood_on_budget_denied_does_not_relay);
    return UNITY_END();
}
