/*
 * 3-node integration test: A(0) ↔ B(1) ↔ C(2)
 * A and C cannot hear each other directly.
 * Tests full RREQ→RREP→data forwarding flow.
 */
#include "unity.h"
#include "esp_stubs.h"

/* Include implementations directly */
#include "../components/location/include/location.h"
#include "../components/routing/routing.c"
#include "../components/routing/discovery.c"
#include "../components/routing/forwarding.c"
#include "../components/packet/packet.c"
#include "../components/radio/radio_mock.c"

/* Node addresses */
#define ADDR_A 0x0A000001
#define ADDR_B 0x0B000002
#define ADDR_C 0x0C000003

/* Per-node state */
typedef struct {
    uint32_t addr;
    routing_table_t routes;
    reverse_route_table_t reverse_routes;
    rreq_dedup_t rreq_dedup;
    pending_discovery_table_t pending;
} node_state_t;

static mock_radio_t radio;
static node_state_t nodes[3];

void setUp(void) {
    /* Init radio: A↔B, B↔C, no A↔C */
    mock_radio_init(&radio, 3);
    mock_radio_connect(&radio, 0, 1, -60, 10);  /* A↔B: good link */
    mock_radio_connect(&radio, 1, 2, -70, 8);   /* B↔C: decent link */
    /* A↔C: not connected (default) */

    /* Init node state */
    nodes[0].addr = ADDR_A;
    nodes[1].addr = ADDR_B;
    nodes[2].addr = ADDR_C;
    for (int i = 0; i < 3; i++) {
        route_init(&nodes[i].routes);
        reverse_route_init(&nodes[i].reverse_routes);
        rreq_dedup_init(&nodes[i].rreq_dedup);
        discovery_init(&nodes[i].pending);
    }
}

void tearDown(void) {}

/*
 * Full route discovery and data forwarding test.
 */
void test_three_node_route_discovery(void) {
    uint32_t now = 1000;
    uint32_t query_id = 0x12345678;

    /* Step 1: A builds RREQ for C */
    bramble_rreq_t rreq_a = rreq_build_originator(ADDR_A, ADDR_C, query_id, ADDR_A,
                                                  discovery_hop_limit_for_attempt(1));
    TEST_ASSERT_EQUAL(PKT_TYPE_RREQ, rreq_a.header.type);
    TEST_ASSERT_EQUAL(ADDR_C, rreq_a.header.dest_addr);
    TEST_ASSERT_EQUAL(0, rreq_a.hop_count);
    TEST_ASSERT_EQUAL(255, rreq_a.metric);

    /* A serializes and broadcasts */
    uint8_t buf[RREQ_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_rreq_serialize(&rreq_a, buf, RREQ_SIZE));
    mock_radio_send(&radio, 0, buf, RREQ_SIZE);

    /* Step 2: B receives RREQ */
    mock_packet_t pkt;
    TEST_ASSERT_TRUE(mock_radio_recv(&radio, 1, &pkt));
    TEST_ASSERT_EQUAL(RREQ_SIZE, pkt.len);

    bramble_rreq_t rreq_at_b;
    TEST_ASSERT_EQUAL(ESP_OK, bramble_rreq_deserialize(&rreq_at_b, pkt.data, pkt.len));

    /* B checks dedup — first time seeing this query */
    TEST_ASSERT_FALSE(rreq_dedup_check_and_add(&nodes[1].rreq_dedup, query_id, now));

    /* B stores reverse route (query_id → prev_hop=A) */
    reverse_route_add(&nodes[1].reverse_routes, query_id, ADDR_A, now);
    reverse_route_t *rr = reverse_route_lookup(&nodes[1].reverse_routes, query_id);
    TEST_ASSERT_NOT_NULL(rr);
    TEST_ASSERT_EQUAL(ADDR_A, rr->prev_hop);

    /* B forwards RREQ */
    bramble_rreq_t rreq_fwd = rreq_forward(&rreq_at_b, ADDR_B, pkt.rssi, pkt.snr);
    TEST_ASSERT_EQUAL(1, rreq_fwd.hop_count);
    TEST_ASSERT_EQUAL(ADDR_B, rreq_fwd.prev_hop);
    TEST_ASSERT_TRUE(rreq_fwd.metric <= 255);  /* penalty applied */

    uint8_t buf2[RREQ_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_rreq_serialize(&rreq_fwd, buf2, RREQ_SIZE));
    mock_radio_send(&radio, 1, buf2, RREQ_SIZE);

    /* A should NOT get it back (dedup would catch it, but also verify radio) */
    /* Actually A CAN hear B, so it will receive it — dedup handles that */

    /* Step 3: C receives RREQ */
    TEST_ASSERT_TRUE(mock_radio_recv(&radio, 2, &pkt));

    bramble_rreq_t rreq_at_c;
    TEST_ASSERT_EQUAL(ESP_OK, bramble_rreq_deserialize(&rreq_at_c, pkt.data, pkt.len));
    TEST_ASSERT_EQUAL(ADDR_C, rreq_at_c.header.dest_addr);

    /* C is the destination — builds RREP */
    bramble_rrep_t rrep_c = rrep_build_destination(&rreq_at_c, ADDR_C);
    TEST_ASSERT_EQUAL(PKT_TYPE_RREP, rrep_c.header.type);
    TEST_ASSERT_EQUAL(ADDR_C, rrep_c.src_addr);
    TEST_ASSERT_EQUAL(ADDR_B, rrep_c.next_hop);  /* unicast back to prev_hop */

    /* C serializes and sends RREP toward B */
    uint8_t rrep_buf[RREP_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_rrep_serialize(&rrep_c, rrep_buf, RREP_SIZE));
    mock_radio_send(&radio, 2, rrep_buf, RREP_SIZE);

    /* Step 4: B receives RREP */
    TEST_ASSERT_TRUE(mock_radio_recv(&radio, 1, &pkt));

    bramble_rrep_t rrep_at_b;
    TEST_ASSERT_EQUAL(ESP_OK, bramble_rrep_deserialize(&rrep_at_b, pkt.data, pkt.len));
    TEST_ASSERT_EQUAL(ADDR_C, rrep_at_b.src_addr);

    /* B installs forward route: dest=C, next_hop=C (direct) */
    route_install(&nodes[1].routes, ADDR_C, ADDR_C, 1, rrep_at_b.route_metric,
                  ROUTE_ACTIVE, now);
    route_entry_t *r_bc = route_lookup(&nodes[1].routes, ADDR_C);
    TEST_ASSERT_NOT_NULL(r_bc);
    TEST_ASSERT_EQUAL(ADDR_C, r_bc->next_hop);

    /* B looks up reverse route to forward RREP back toward A */
    rr = reverse_route_lookup(&nodes[1].reverse_routes, rrep_at_b.query_id);
    TEST_ASSERT_NOT_NULL(rr);
    TEST_ASSERT_EQUAL(ADDR_A, rr->prev_hop);

    bramble_rrep_t rrep_fwd = rrep_forward(&rrep_at_b, ADDR_A);
    TEST_ASSERT_EQUAL(ADDR_A, rrep_fwd.next_hop);
    TEST_ASSERT_EQUAL(ADDR_A, rrep_fwd.header.dest_addr);

    uint8_t rrep_buf2[RREP_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, bramble_rrep_serialize(&rrep_fwd, rrep_buf2, RREP_SIZE));
    mock_radio_send(&radio, 1, rrep_buf2, RREP_SIZE);

    /* Step 5: A receives RREP */
    /* Drain A's queue — it may have the forwarded RREQ first */
    bool got_rrep = false;
    bramble_rrep_t rrep_at_a;
    while (mock_radio_recv(&radio, 0, &pkt)) {
        bramble_header_t hdr;
        bramble_header_deserialize(&hdr, pkt.data, pkt.len);
        if (hdr.type == PKT_TYPE_RREP) {
            bramble_rrep_deserialize(&rrep_at_a, pkt.data, pkt.len);
            got_rrep = true;
        }
    }
    TEST_ASSERT_TRUE(got_rrep);
    TEST_ASSERT_EQUAL(ADDR_C, rrep_at_a.src_addr);

    /* A installs route: dest=C, next_hop=B */
    route_install(&nodes[0].routes, ADDR_C, ADDR_B, rrep_at_a.hop_count,
                  rrep_at_a.route_metric, ROUTE_ACTIVE, now);

    /* Step 6: Verify A's route to C goes via B */
    route_entry_t *r_ac = route_lookup(&nodes[0].routes, ADDR_C);
    TEST_ASSERT_NOT_NULL(r_ac);
    TEST_ASSERT_EQUAL(ADDR_B, r_ac->next_hop);
    TEST_ASSERT_EQUAL(ROUTE_ACTIVE, r_ac->state);

    /* Step 7: A forwards data toward C */
    uint8_t hop_limit = 4;
    forward_result_t fwd = forward_data(&nodes[0].routes, ADDR_C, &hop_limit, now);
    TEST_ASSERT_TRUE(fwd.should_send);
    TEST_ASSERT_FALSE(fwd.route_error);
    TEST_ASSERT_EQUAL(ADDR_B, fwd.next_hop);
    TEST_ASSERT_EQUAL(3, hop_limit);  /* decremented */

    /* B can forward toward C */
    hop_limit = 3;
    fwd = forward_data(&nodes[1].routes, ADDR_C, &hop_limit, now);
    TEST_ASSERT_TRUE(fwd.should_send);
    TEST_ASSERT_EQUAL(ADDR_C, fwd.next_hop);
}

void test_rreq_dedup_prevents_loops(void) {
    uint32_t now = 2000;
    uint32_t query_id = 0xDEADBEEF;

    /* First check: not seen */
    TEST_ASSERT_FALSE(rreq_dedup_check_and_add(&nodes[0].rreq_dedup, query_id, now));
    /* Second check: already seen */
    TEST_ASSERT_TRUE(rreq_dedup_check_and_add(&nodes[0].rreq_dedup, query_id, now));
}

void test_route_error_on_missing_route(void) {
    uint8_t hop_limit = 4;
    forward_result_t fwd = forward_data(&nodes[0].routes, 0xDEAD0000, &hop_limit, 1000);
    TEST_ASSERT_FALSE(fwd.should_send);
    TEST_ASSERT_TRUE(fwd.route_error);
}

void test_rerr_breaks_route(void) {
    uint32_t now = 3000;

    /* Install a route */
    route_install(&nodes[0].routes, ADDR_C, ADDR_B, 2, 200, ROUTE_ACTIVE, now);

    /* Build and handle RERR */
    bramble_rerr_t rerr = rerr_build(ADDR_B, ADDR_C, ADDR_C);
    rerr_handle(&nodes[0].routes, &rerr);

    /* Route should not be broken — RERR says broken_next_hop=C but A's next_hop=B */
    route_entry_t *r = route_lookup(&nodes[0].routes, ADDR_C);
    TEST_ASSERT_EQUAL(ROUTE_ACTIVE, r->state);

    /* Now RERR with matching next_hop */
    bramble_rerr_t rerr2 = rerr_build(ADDR_B, ADDR_C, ADDR_B);
    rerr_handle(&nodes[0].routes, &rerr2);
    r = route_lookup(&nodes[0].routes, ADDR_C);
    TEST_ASSERT_EQUAL(ROUTE_BROKEN, r->state);
}

void test_location_tx_uses_dedicated_packet_type(void) {
    /* Coarse/binary payload shape, intentionally not JSON text. */
    uint8_t payload[LOCATION_COARSE_SIZE] = {0x11, 0x22, 0x33, 0x44, 0x55};
    int payload_len = LOCATION_COARSE_SIZE;

    uint8_t packet[HEADER_SIZE + 4 + LOCATION_FULL_SIZE] = {0};
    bramble_header_t hdr = {
        .version = BRAMBLE_VERSION,
        .type = PKT_TYPE_LOCATION,
        .flags = (uint8_t)(LOCATION_TIER_COARSE << FLAG_TIER_SHIFT),
        .hop_limit = 3,
        .dest_addr = ADDR_B,
        .packet_id = 0x01020304,
    };
    TEST_ASSERT_EQUAL(ESP_OK, bramble_header_serialize(&hdr, packet, sizeof(packet)));
    uint32_t src_addr = ADDR_A;
    memcpy(packet + HEADER_SIZE, &src_addr, 4);
    memcpy(packet + HEADER_SIZE + 4, payload, (size_t)payload_len);

    bramble_header_t out_hdr;
    TEST_ASSERT_EQUAL(ESP_OK, bramble_header_deserialize(&out_hdr, packet, HEADER_SIZE + 4 + payload_len));
    TEST_ASSERT_EQUAL(PKT_TYPE_LOCATION, out_hdr.type);
    TEST_ASSERT_NOT_EQUAL('{', packet[HEADER_SIZE + 4]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_three_node_route_discovery);
    RUN_TEST(test_rreq_dedup_prevents_loops);
    RUN_TEST(test_route_error_on_missing_route);
    RUN_TEST(test_rerr_breaks_route);
    RUN_TEST(test_location_tx_uses_dedicated_packet_type);
    return UNITY_END();
}
