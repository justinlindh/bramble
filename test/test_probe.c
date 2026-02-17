#include "unity/unity.h"
#include "../components/bramble_probe/bramble_probe.c"

/* Test send callback captures */
static uint8_t sent_data[256];
static uint16_t sent_len;
static int send_count;

static void test_send_fn(const uint8_t *data, uint16_t len, void *ctx) {
    (void)ctx;
    if (len > 256) len = 256;
    memcpy(sent_data, data, len);
    sent_len = len;
    send_count++;
}

static bramble_probe_state_t state;

void setUp(void) {
    memset(&state, 0, sizeof(state));
    memset(sent_data, 0, sizeof(sent_data));
    sent_len = 0;
    send_count = 0;
    bramble_probe_init(&state, 0xAABBCCDD, test_send_fn, NULL);
}

void tearDown(void) {}

void test_probe_packet_creation(void) {
    int ret = bramble_probe_send(&state, PROBE_FLAG_INCLUDE_RSSI, 1000);
    TEST_ASSERT_EQUAL_INT(0, ret);
    TEST_ASSERT_EQUAL_INT(1, send_count);
    TEST_ASSERT_EQUAL_UINT16(sizeof(bramble_probe_packet_t), sent_len);

    bramble_probe_packet_t *pkt = (bramble_probe_packet_t *)sent_data;
    TEST_ASSERT_EQUAL_UINT8(1, pkt->version);
    TEST_ASSERT_EQUAL_UINT8(BRAMBLE_TYPE_BROADCAST_PROBE, pkt->type);
    TEST_ASSERT_EQUAL_UINT8(PROBE_FLAG_INCLUDE_RSSI, pkt->flags);
    TEST_ASSERT_EQUAL_HEX32(0xAABBCCDD, pkt->src_addr);
    TEST_ASSERT_TRUE(pkt->probe_id != 0);

    /* Should be collecting */
    TEST_ASSERT_TRUE(state.collecting);
    TEST_ASSERT_EQUAL_HEX32(pkt->probe_id, state.result.probe_id);
}

void test_rate_limiting(void) {
    /* Use all 3 tokens */
    TEST_ASSERT_EQUAL_INT(0, bramble_probe_send(&state, 0, 1000));
    state.collecting = false; /* reset for next send */
    TEST_ASSERT_EQUAL_INT(0, bramble_probe_send(&state, 0, 2000));
    state.collecting = false;
    TEST_ASSERT_EQUAL_INT(0, bramble_probe_send(&state, 0, 3000));
    state.collecting = false;

    /* 4th should fail */
    TEST_ASSERT_EQUAL_INT(-1, bramble_probe_send(&state, 0, 4000));
    TEST_ASSERT_FALSE(bramble_probe_can_send(&state, 4000));

    /* After refill interval, should work again */
    TEST_ASSERT_TRUE(bramble_probe_can_send(&state, 4000 + PROBE_RATE_LIMIT_REFILL_MS));
    TEST_ASSERT_EQUAL_INT(0, bramble_probe_send(&state, 0, 4000 + PROBE_RATE_LIMIT_REFILL_MS));
}

void test_rate_limit_remaining_sec(void) {
    /* Exhaust tokens */
    bramble_probe_send(&state, 0, 1000); state.collecting = false;
    bramble_probe_send(&state, 0, 1000); state.collecting = false;
    bramble_probe_send(&state, 0, 1000); state.collecting = false;

    uint32_t remaining = bramble_probe_get_rate_limit_remaining_sec(&state, 1000);
    TEST_ASSERT_TRUE(remaining > 0);
    TEST_ASSERT_TRUE(remaining <= 60);

    /* Should be 0 when tokens available */
    bramble_probe_init(&state, 0xAABBCCDD, test_send_fn, NULL);
    TEST_ASSERT_EQUAL_UINT32(0, bramble_probe_get_rate_limit_remaining_sec(&state, 1000));
}

void test_ack_handling(void) {
    /* Send probe first */
    bramble_probe_send(&state, PROBE_FLAG_INCLUDE_RSSI, 1000);
    uint32_t probe_id = state.result.probe_id;

    /* Simulate receiving an ACK */
    bramble_probe_ack_t ack;
    memset(&ack, 0, sizeof(ack));
    ack.version = 1;
    ack.type = BRAMBLE_TYPE_BROADCAST_ACK;
    ack.flags = PROBE_FLAG_INCLUDE_RSSI;
    ack.hop_count = 1;
    ack.src_addr = 0x11223344;
    ack.probe_id = probe_id;
    ack.rssi = -65;

    bramble_probe_handle_ack(&state, (uint8_t *)&ack, 14, 2000);

    const probe_result_t *result = bramble_probe_get_result(&state);
    TEST_ASSERT_EQUAL_UINT16(1, result->response_count);
    TEST_ASSERT_EQUAL_HEX32(0x11223344, result->responses[0].responder_addr);
    TEST_ASSERT_EQUAL_INT8(-65, result->responses[0].rssi);
    TEST_ASSERT_TRUE(result->responses[0].has_rssi);
    TEST_ASSERT_EQUAL_UINT32(1000, result->responses[0].latency_ms);
}

void test_dedup(void) {
    /* Create a probe packet from another node */
    bramble_probe_packet_t pkt;
    pkt.version = 1;
    pkt.type = BRAMBLE_TYPE_BROADCAST_PROBE;
    pkt.flags = 0;
    pkt.hop_limit = 3;
    pkt.src_addr = 0x11111111;
    pkt.probe_id = 0xDEADBEEF;

    /* First time: should queue an ACK */
    bramble_probe_handle_probe(&state, (uint8_t *)&pkt, sizeof(pkt), -70, 50000);
    TEST_ASSERT_TRUE(state.pending_ack.active);

    /* Tick to send the ACK */
    bramble_probe_tick(&state, 50000 + PROBE_ACK_JITTER_MAX_MS + 1);
    int first_send_count = send_count;

    /* Reset pending ack state, advance past cooldown */
    uint32_t time_after_cooldown = 50000 + PROBE_ACK_JITTER_MAX_MS + 1 + PROBE_ACK_COOLDOWN_MS + 1;

    /* Same probe ID again: should be deduped */
    bramble_probe_handle_probe(&state, (uint8_t *)&pkt, sizeof(pkt), -70, time_after_cooldown);
    TEST_ASSERT_FALSE(state.pending_ack.active);
    TEST_ASSERT_EQUAL_INT(first_send_count, send_count);
}

void test_collection_window_expiry(void) {
    bramble_probe_send(&state, 0, 1000);
    TEST_ASSERT_TRUE(state.collecting);
    TEST_ASSERT_FALSE(state.result.complete);

    /* Before window expires */
    bramble_probe_tick(&state, 1000 + PROBE_COLLECTION_WINDOW_MS - 1);
    TEST_ASSERT_TRUE(state.collecting);

    /* After window expires */
    bramble_probe_tick(&state, 1000 + PROBE_COLLECTION_WINDOW_MS);
    TEST_ASSERT_FALSE(state.collecting);
    TEST_ASSERT_TRUE(state.result.complete);
}

void test_silent_probe_no_ack(void) {
    bramble_probe_packet_t pkt;
    pkt.version = 1;
    pkt.type = BRAMBLE_TYPE_BROADCAST_PROBE;
    pkt.flags = PROBE_FLAG_SILENT;
    pkt.hop_limit = 3;
    pkt.src_addr = 0x22222222;
    pkt.probe_id = 0xCAFEBABE;

    bramble_probe_handle_probe(&state, (uint8_t *)&pkt, sizeof(pkt), -70, 50000);
    TEST_ASSERT_FALSE(state.pending_ack.active);
}

void test_own_probe_ignored(void) {
    bramble_probe_packet_t pkt;
    pkt.version = 1;
    pkt.type = BRAMBLE_TYPE_BROADCAST_PROBE;
    pkt.flags = 0;
    pkt.hop_limit = 3;
    pkt.src_addr = 0xAABBCCDD;  /* same as our addr */
    pkt.probe_id = 0x12345678;

    bramble_probe_handle_probe(&state, (uint8_t *)&pkt, sizeof(pkt), -70, 50000);
    TEST_ASSERT_FALSE(state.pending_ack.active);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_probe_packet_creation);
    RUN_TEST(test_rate_limiting);
    RUN_TEST(test_rate_limit_remaining_sec);
    RUN_TEST(test_ack_handling);
    RUN_TEST(test_dedup);
    RUN_TEST(test_collection_window_expiry);
    RUN_TEST(test_silent_probe_no_ack);
    RUN_TEST(test_own_probe_ignored);
    return UNITY_END();
}
