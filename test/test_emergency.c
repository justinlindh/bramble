#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "emergency.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { \
    printf("  %-50s", #name); \
    tests_run++; \
    name(); \
    tests_passed++; \
    printf("PASS\n"); \
} while(0)

static void test_emergency_activate_and_cancel(void) {
    emergency_manager_t mgr;
    emergency_init(&mgr);

    assert(mgr.state == EMERGENCY_STATE_INACTIVE);
    assert(!emergency_is_active(&mgr));

    int rc = emergency_activate(&mgr, 474000000, -1224000000, 100, 85, "HELP", 1000);
    assert(rc == 0);
    assert(emergency_is_active(&mgr));
    assert(mgr.beacon.latitude_e7 == 474000000);
    assert(mgr.beacon.msg_len == 4);
    assert(memcmp(mgr.beacon.short_msg, "HELP", 4) == 0);

    rc = emergency_cancel(&mgr, 5000);
    assert(rc == 0);
    assert(!emergency_is_active(&mgr));
    assert(mgr.state == EMERGENCY_STATE_COOLDOWN);
}

static void test_emergency_auto_timeout(void) {
    emergency_manager_t mgr;
    emergency_init(&mgr);

    emergency_activate(&mgr, 0, 0, 0, 50, NULL, 1000);
    assert(emergency_is_active(&mgr));

    /* Just before timeout */
    uint32_t almost = 1000 + (uint32_t)(EMERGENCY_AUTO_TIMEOUT_MS - 1);
    emergency_tick(&mgr, almost);
    assert(emergency_is_active(&mgr));

    /* At/after timeout */
    uint32_t after = 1000 + (uint32_t)EMERGENCY_AUTO_TIMEOUT_MS;
    emergency_tick(&mgr, after);
    assert(!emergency_is_active(&mgr));
    assert(mgr.state == EMERGENCY_STATE_INACTIVE);
}

static void test_emergency_cooldown(void) {
    emergency_manager_t mgr;
    emergency_init(&mgr);

    emergency_activate(&mgr, 0, 0, 0, 50, NULL, 1000);
    emergency_cancel(&mgr, 5000);
    assert(mgr.state == EMERGENCY_STATE_COOLDOWN);

    /* Before cooldown expires */
    emergency_tick(&mgr, 5000 + EMERGENCY_COOLDOWN_MS - 1);
    assert(mgr.state == EMERGENCY_STATE_COOLDOWN);

    /* After cooldown */
    emergency_tick(&mgr, 5000 + EMERGENCY_COOLDOWN_MS);
    assert(mgr.state == EMERGENCY_STATE_INACTIVE);
}

static void test_emergency_rate_limit(void) {
    emergency_manager_t mgr;
    emergency_init(&mgr);

    emergency_activate(&mgr, 0, 0, 0, 50, NULL, 1000);
    emergency_cancel(&mgr, 2000);
    emergency_tick(&mgr, 2000 + EMERGENCY_COOLDOWN_MS);  /* clear cooldown */
    assert(mgr.state == EMERGENCY_STATE_INACTIVE);

    /* Try to activate again within 1h — should be rejected */
    int rc = emergency_activate(&mgr, 0, 0, 0, 50, NULL, 1000 + EMERGENCY_MIN_ACTIVATION_MS - 1);
    assert(rc == -2);

    /* After 1h — should succeed */
    rc = emergency_activate(&mgr, 0, 0, 0, 50, NULL, 1000 + EMERGENCY_MIN_ACTIVATION_MS);
    assert(rc == 0);
}

static void test_emergency_beacon_serialize_roundtrip(void) {
    /* With message */
    emergency_beacon_t b = {0};
    b.src_addr = 0xDEADBEEF;
    b.latitude_e7 = 474123456;
    b.longitude_e7 = -1224567890;
    b.altitude_m = -50;
    b.battery_pct = 73;
    memcpy(b.short_msg, "SOS HELP ME", 11);
    b.msg_len = 11;

    uint8_t buf[EMERGENCY_BEACON_MAX_SIZE];
    int len = emergency_beacon_serialize(&b, buf, sizeof(buf));
    assert(len == EMERGENCY_BEACON_MIN_SIZE + 11);

    emergency_beacon_t b2;
    int rc = emergency_beacon_deserialize(buf, len, &b2);
    assert(rc == 0);
    assert(b2.src_addr == b.src_addr);
    assert(b2.latitude_e7 == b.latitude_e7);
    assert(b2.longitude_e7 == b.longitude_e7);
    assert(b2.altitude_m == b.altitude_m);
    assert(b2.battery_pct == b.battery_pct);
    assert(b2.msg_len == 11);
    assert(memcmp(b2.short_msg, "SOS HELP ME", 11) == 0);

    /* Without message */
    emergency_beacon_t b3 = {0};
    b3.src_addr = 42;
    b3.msg_len = 0;
    len = emergency_beacon_serialize(&b3, buf, sizeof(buf));
    assert(len == EMERGENCY_BEACON_MIN_SIZE);

    emergency_beacon_t b4;
    rc = emergency_beacon_deserialize(buf, len, &b4);
    assert(rc == 0);
    assert(b4.src_addr == 42);
    assert(b4.msg_len == 0);
}

static void test_emergency_should_beacon(void) {
    emergency_manager_t mgr;
    emergency_init(&mgr);

    /* Not active — no beacon */
    assert(!emergency_should_beacon(&mgr, 1000));

    emergency_activate(&mgr, 0, 0, 0, 50, NULL, 1000);

    /* First beacon — immediate */
    assert(emergency_should_beacon(&mgr, 1000));

    /* Simulate beacon sent */
    mgr.last_beacon_ms = 1000;

    /* Too early */
    assert(!emergency_should_beacon(&mgr, 1000 + EMERGENCY_BEACON_INTERVAL_MS - 1));

    /* On time */
    assert(emergency_should_beacon(&mgr, 1000 + EMERGENCY_BEACON_INTERVAL_MS));
}

static void test_emergency_track_received(void) {
    emergency_manager_t mgr;
    emergency_init(&mgr);

    assert(emergency_get_active_count(&mgr) == 0);

    emergency_beacon_t b1 = {.src_addr = 100};
    emergency_beacon_t b2 = {.src_addr = 200};

    emergency_record_received(&mgr, &b1, 1000);
    assert(emergency_get_active_count(&mgr) == 1);

    emergency_record_received(&mgr, &b2, 2000);
    assert(emergency_get_active_count(&mgr) == 2);

    /* Duplicate update */
    emergency_record_received(&mgr, &b1, 3000);
    assert(emergency_get_active_count(&mgr) == 2);
    assert(mgr.known_count == 2);
}

static void test_emergency_cancel_received(void) {
    emergency_manager_t mgr;
    emergency_init(&mgr);

    emergency_beacon_t b1 = {.src_addr = 100};
    emergency_beacon_t b2 = {.src_addr = 200};

    emergency_record_received(&mgr, &b1, 1000);
    emergency_record_received(&mgr, &b2, 2000);
    assert(emergency_get_active_count(&mgr) == 2);

    int rc = emergency_record_cancel(&mgr, 100);
    assert(rc == 0);
    assert(emergency_get_active_count(&mgr) == 1);

    /* Cancel unknown — should return -1 */
    rc = emergency_record_cancel(&mgr, 999);
    assert(rc == -1);
}

int main(void) {
    printf("Emergency beacon tests:\n");

    TEST(test_emergency_activate_and_cancel);
    TEST(test_emergency_auto_timeout);
    TEST(test_emergency_cooldown);
    TEST(test_emergency_rate_limit);
    TEST(test_emergency_beacon_serialize_roundtrip);
    TEST(test_emergency_should_beacon);
    TEST(test_emergency_track_received);
    TEST(test_emergency_cancel_received);

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
