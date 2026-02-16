#include "unity.h"
#include "../components/timesync/anti_replay.c"

void setUp(void) {}
void tearDown(void) {}

void test_fresh_packet_accepted(void) {
    anti_replay_cache_t cache;
    anti_replay_init(&cache);
    TEST_ASSERT_TRUE(anti_replay_check(&cache, 42, 10000, 10000));
}

void test_duplicate_rejected(void) {
    anti_replay_cache_t cache;
    anti_replay_init(&cache);
    anti_replay_add(&cache, 42, 10000);
    TEST_ASSERT_FALSE(anti_replay_check(&cache, 42, 10000, 10000));
}

void test_expired_packet_rejected(void) {
    anti_replay_cache_t cache;
    anti_replay_init(&cache);
    // Packet timestamp is 50s old (>30s window)
    TEST_ASSERT_FALSE(anti_replay_check(&cache, 99, 10000, 60001));
}

void test_purge_removes_old(void) {
    anti_replay_cache_t cache;
    anti_replay_init(&cache);
    anti_replay_add(&cache, 1, 10000);
    anti_replay_add(&cache, 2, 20000);
    anti_replay_add(&cache, 3, 50000);
    TEST_ASSERT_EQUAL_INT(3, cache.count);
    // Purge at time 55000: entries at 10000 and 20000 are >30s old
    anti_replay_purge(&cache, 55000);
    TEST_ASSERT_EQUAL_INT(1, cache.count);
    TEST_ASSERT_EQUAL_UINT32(3, cache.entries[0].packet_id);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_fresh_packet_accepted);
    RUN_TEST(test_duplicate_rejected);
    RUN_TEST(test_expired_packet_rejected);
    RUN_TEST(test_purge_removes_old);
    return UNITY_END();
}
