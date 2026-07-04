#include "unity/unity.h"
#include "../components/bramble_probe/bramble_probe.c"

static bramble_probe_state_t state;
static uint8_t sent_data[256];
static uint16_t sent_len;

static void test_send_fn(const uint8_t* data, uint16_t len, void* ctx) {
    (void)ctx;
    if (len > sizeof(sent_data))
        len = sizeof(sent_data);
    memcpy(sent_data, data, len);
    sent_len = len;
}

void setUp(void) {
    memset(&state, 0, sizeof(state));
    memset(sent_data, 0, sizeof(sent_data));
    sent_len = 0;
    bramble_probe_init(&state, 0xAABBCCDD, test_send_fn, NULL);
}

void tearDown(void) {}

void test_v2_no_self_responder(void) {
    bramble_probe_send(&state, PROBE_FLAG_INCLUDE_RSSI, 1000);
    uint32_t probe_id = state.result.probe_id;

    bramble_probe_ack_t ack = {0};
    ack.version = 1;
    ack.type = BRAMBLE_TYPE_BROADCAST_ACK;
    ack.flags = PROBE_FLAG_INCLUDE_RSSI;
    ack.hop_count = 1;
    ack.src_addr = 0xAABBCCDD; /* self */
    ack.probe_id = probe_id;
    ack.rssi = -40;

    bramble_probe_handle_ack(&state, (const uint8_t*)&ack, 14, 1200);

    const probe_result_t* result = bramble_probe_get_result(&state);
    TEST_ASSERT_EQUAL_UINT16(0, result->response_count);
}

void test_v2_duplicate_responder_updates_row_not_count(void) {
    bramble_probe_send(&state, PROBE_FLAG_INCLUDE_RSSI, 1000);
    uint32_t probe_id = state.result.probe_id;

    bramble_probe_ack_t ack1 = {0};
    ack1.version = 1;
    ack1.type = BRAMBLE_TYPE_BROADCAST_ACK;
    ack1.flags = PROBE_FLAG_INCLUDE_RSSI;
    ack1.hop_count = 1;
    ack1.src_addr = 0x11223344;
    ack1.probe_id = probe_id;
    ack1.rssi = -90;

    bramble_probe_ack_t ack2 = ack1;
    ack2.rssi = -60; /* better */

    bramble_probe_handle_ack(&state, (const uint8_t*)&ack1, 14, 1500);
    bramble_probe_handle_ack(&state, (const uint8_t*)&ack2, 14, 1800);

    const probe_result_t* result = bramble_probe_get_result(&state);
    TEST_ASSERT_EQUAL_UINT16(1, result->response_count);
    TEST_ASSERT_EQUAL_HEX32(0x11223344, result->responses[0].responder_addr);
    TEST_ASSERT_EQUAL_INT8(-60, result->responses[0].rssi);
    TEST_ASSERT_EQUAL_UINT32(800, result->responses[0].latency_ms);
}

void test_v2_completion_window_is_explicitly_bounded(void) {
    bramble_probe_send(&state, 0, 1000);
    TEST_ASSERT_TRUE(state.collecting);
    TEST_ASSERT_FALSE(state.result.complete);

    bramble_probe_tick(&state, 1000 + PROBE_COLLECTION_WINDOW_MS);
    TEST_ASSERT_FALSE(state.collecting);
    TEST_ASSERT_TRUE(state.result.complete);

    /* ACKs after completion must be ignored */
    bramble_probe_ack_t ack = {0};
    ack.version = 1;
    ack.type = BRAMBLE_TYPE_BROADCAST_ACK;
    ack.flags = 0;
    ack.hop_count = 1;
    ack.src_addr = 0x55667788;
    ack.probe_id = state.result.probe_id;

    bramble_probe_handle_ack(&state, (const uint8_t*)&ack, 13,
                             1000 + PROBE_COLLECTION_WINDOW_MS + 500);

    const probe_result_t* result = bramble_probe_get_result(&state);
    TEST_ASSERT_EQUAL_UINT16(0, result->response_count);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_v2_no_self_responder);
    RUN_TEST(test_v2_duplicate_responder_updates_row_not_count);
    RUN_TEST(test_v2_completion_window_is_explicitly_bounded);
    return UNITY_END();
}
