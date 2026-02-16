#include "unity.h"
#include "../components/security/dummy_traffic.c"
#include "../components/packet/packet.c"

void setUp(void) { srand(12345); }
void tearDown(void) {}

void test_init_disabled(void) {
    dummy_traffic_ctx_t ctx;
    dummy_traffic_init(&ctx);
    TEST_ASSERT_FALSE(dummy_traffic_is_enabled(&ctx));
    TEST_ASSERT_EQUAL_UINT32(0, dummy_traffic_get_count(&ctx));
}

void test_enable_schedules_first_send(void) {
    dummy_traffic_ctx_t ctx;
    dummy_traffic_init(&ctx);
    dummy_traffic_enable(&ctx, true, 1000);
    TEST_ASSERT_TRUE(dummy_traffic_is_enabled(&ctx));
    // next_send_time should be in the future
    TEST_ASSERT_TRUE(ctx.next_send_time > 1000);
    TEST_ASSERT_TRUE(ctx.next_send_time <= 1000 + DUMMY_TRAFFIC_MAX_INTERVAL_MS);
}

void test_should_send_before_interval_false(void) {
    dummy_traffic_ctx_t ctx;
    dummy_traffic_init(&ctx);
    dummy_traffic_enable(&ctx, true, 1000);
    size_t size;
    // Right after enable, before next_send_time
    TEST_ASSERT_FALSE(dummy_traffic_should_send(&ctx, 1001, 10000, &size));
}

void test_should_send_after_interval_true(void) {
    dummy_traffic_ctx_t ctx;
    dummy_traffic_init(&ctx);
    dummy_traffic_enable(&ctx, true, 1000);
    size_t size;
    // Jump well past any possible interval
    bool sent = dummy_traffic_should_send(&ctx, 1000 + DUMMY_TRAFFIC_MAX_INTERVAL_MS + 1, 10000, &size);
    TEST_ASSERT_TRUE(sent);
    TEST_ASSERT_TRUE(size >= DUMMY_TRAFFIC_MIN_SIZE);
    TEST_ASSERT_TRUE(size <= DUMMY_TRAFFIC_MAX_SIZE);
}

void test_record_send_tracks_airtime(void) {
    dummy_traffic_ctx_t ctx;
    dummy_traffic_init(&ctx);
    dummy_traffic_enable(&ctx, true, 1000);
    dummy_traffic_record_send(&ctx, 50, 2000);
    TEST_ASSERT_EQUAL_UINT32(1, dummy_traffic_get_count(&ctx));
    TEST_ASSERT_EQUAL_UINT32(50, ctx.airtime_used_ms);
}

void test_airtime_budget_exceeded(void) {
    dummy_traffic_ctx_t ctx;
    dummy_traffic_init(&ctx);
    dummy_traffic_enable(&ctx, true, 1000);

    // Use up a lot of airtime
    ctx.airtime_used_ms = 500;  // Already used 500ms

    size_t size;
    // Budget remaining is 100ms, total = 600ms, 2% = 12ms, we already used 500 > 12
    bool sent = dummy_traffic_should_send(&ctx, 1000 + DUMMY_TRAFFIC_MAX_INTERVAL_MS + 1, 100, &size);
    TEST_ASSERT_FALSE(sent);
}

void test_build_packet_valid_header(void) {
    uint8_t pkt[64];
    TEST_ASSERT_EQUAL(0, dummy_traffic_build_packet(pkt, sizeof(pkt), 0xAABBCCDD));

    bramble_header_t hdr;
    TEST_ASSERT_EQUAL(ESP_OK, bramble_header_deserialize(&hdr, pkt, sizeof(pkt)));
    TEST_ASSERT_EQUAL(BRAMBLE_VERSION, hdr.version);
    TEST_ASSERT_EQUAL(PKT_TYPE_DATA, hdr.type);
    TEST_ASSERT_TRUE(hdr.flags & FLAG_ENCRYPT);
    TEST_ASSERT_EQUAL(1, hdr.hop_limit);
    TEST_ASSERT_NOT_EQUAL(0x00000000, hdr.dest_addr);
    TEST_ASSERT_NOT_EQUAL(0xFFFFFFFF, hdr.dest_addr);

    // Check src_addr is written after header
    uint32_t src;
    memcpy(&src, pkt + HEADER_SIZE, 4);
    TEST_ASSERT_EQUAL_UINT32(0xAABBCCDD, src);
}

void test_disable_stops_generating(void) {
    dummy_traffic_ctx_t ctx;
    dummy_traffic_init(&ctx);
    dummy_traffic_enable(&ctx, true, 1000);
    dummy_traffic_enable(&ctx, false, 2000);
    TEST_ASSERT_FALSE(dummy_traffic_is_enabled(&ctx));

    size_t size;
    TEST_ASSERT_FALSE(dummy_traffic_should_send(&ctx, 100000, 10000, &size));
}

void test_random_intervals_vary(void) {
    // Run two enable cycles, check that intervals differ (probabilistic but very likely)
    dummy_traffic_ctx_t ctx;
    srand(1);
    dummy_traffic_init(&ctx);
    dummy_traffic_enable(&ctx, true, 0);
    uint32_t interval1 = ctx.next_send_time;

    srand(99999);
    dummy_traffic_init(&ctx);
    dummy_traffic_enable(&ctx, true, 0);
    uint32_t interval2 = ctx.next_send_time;

    // With different seeds, intervals should differ
    TEST_ASSERT_NOT_EQUAL(interval1, interval2);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_init_disabled);
    RUN_TEST(test_enable_schedules_first_send);
    RUN_TEST(test_should_send_before_interval_false);
    RUN_TEST(test_should_send_after_interval_true);
    RUN_TEST(test_record_send_tracks_airtime);
    RUN_TEST(test_airtime_budget_exceeded);
    RUN_TEST(test_build_packet_valid_header);
    RUN_TEST(test_disable_stops_generating);
    RUN_TEST(test_random_intervals_vary);
    return UNITY_END();
}
