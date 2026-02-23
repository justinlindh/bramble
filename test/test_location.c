#include "unity.h"
#include "location.h"
#include <string.h>
#include <math.h>

static location_manager_t mgr;
extern int location_deserialize_for_tier(const uint8_t *buf, size_t len, uint8_t tier, bramble_position_t *pos);

void setUp(void) { location_init(&mgr); }
void tearDown(void) {}

void test_location_serialize_full_roundtrip(void) {
    bramble_position_t pos = {
        .latitude_e7 = 374220000,   /* ~37.422° N */
        .longitude_e7 = -1220840000, /* ~-122.084° W */
        .altitude_m = 42,
        .accuracy_m = 10,
        .speed_kmh = 5,
        .heading_deg2 = 90,
        .timestamp = 1700000000,
        .valid = true
    };
    uint8_t buf[32];
    int n = location_serialize_full(&pos, buf, sizeof(buf));
    TEST_ASSERT_EQUAL(LOCATION_FULL_SIZE, n);

    bramble_position_t out;
    memset(&out, 0, sizeof(out));
    n = location_deserialize_full(buf, LOCATION_FULL_SIZE, &out);
    TEST_ASSERT_EQUAL(LOCATION_FULL_SIZE, n);
    TEST_ASSERT_EQUAL(pos.latitude_e7, out.latitude_e7);
    TEST_ASSERT_EQUAL(pos.longitude_e7, out.longitude_e7);
    TEST_ASSERT_EQUAL(pos.altitude_m, out.altitude_m);
    TEST_ASSERT_EQUAL(pos.accuracy_m, out.accuracy_m);
    TEST_ASSERT_EQUAL(pos.speed_kmh, out.speed_kmh);
    TEST_ASSERT_EQUAL(pos.heading_deg2, out.heading_deg2);
    TEST_ASSERT_EQUAL(pos.timestamp, out.timestamp);
    TEST_ASSERT_TRUE(out.valid);
}

void test_location_serialize_coarse_roundtrip(void) {
    bramble_position_t pos = {
        .latitude_e7 = 374225678,
        .longitude_e7 = -1220841234,
        .timestamp = 0x42,
        .valid = true
    };
    uint8_t buf[8];
    int n = location_serialize_coarse(&pos, buf, sizeof(buf));
    TEST_ASSERT_EQUAL(LOCATION_COARSE_SIZE, n);

    bramble_position_t out;
    n = location_deserialize_coarse(buf, LOCATION_COARSE_SIZE, &out);
    TEST_ASSERT_EQUAL(LOCATION_COARSE_SIZE, n);
    /* Coarse quantization: lat/10000 -> +90000 -> /3 -> *3 -> -90000 -> *10000
     * 374225678/10000 = 37422, +90000=127422, /3=42474, *3=127422, -90000=37422, *10000=374220000
     * lon: -1220841234/10000 = -122084, +180000=57916, /6=9652, *6=57912, -180000=-122088, *10000=-1220880000 */
    TEST_ASSERT_EQUAL(374220000, out.latitude_e7);
    TEST_ASSERT_EQUAL(-1220880000, out.longitude_e7);
    TEST_ASSERT_TRUE(out.valid);
}

void test_location_coarse_precision(void) {
    /* Verify coarse position is within ~1.5km of original */
    bramble_position_t pos = {
        .latitude_e7 = 374229999,
        .longitude_e7 = -1220849999,
        .valid = true
    };
    uint8_t buf[8];
    location_serialize_coarse(&pos, buf, sizeof(buf));
    bramble_position_t out;
    location_deserialize_coarse(buf, LOCATION_COARSE_SIZE, &out);

    /* Max error is 10000 in e7 = 0.001 degrees ~ 111m lat, varies lon */
    double dlat = fabs((pos.latitude_e7 - out.latitude_e7) / 1e7);
    double dlon = fabs((pos.longitude_e7 - out.longitude_e7) / 1e7);
    /* At ~37° lat, 1 degree lon ~ 88km, 1 degree lat ~ 111km */
    double err_m = sqrt(pow(dlat * 111000, 2) + pow(dlon * 88000, 2));
    TEST_ASSERT_TRUE(err_m < 8000.0); /* within ~8km (coarse grid) */
}

void test_location_contact_management(void) {
    TEST_ASSERT_EQUAL(0, location_add_contact(&mgr, 0x1234, LOCATION_TIER_FULL));
    TEST_ASSERT_EQUAL(0, location_add_contact(&mgr, 0x5678, LOCATION_TIER_COARSE));
    TEST_ASSERT_EQUAL(-1, location_add_contact(&mgr, 0x1234, LOCATION_TIER_FULL)); /* dup */
    TEST_ASSERT_EQUAL(2, mgr.contact_count);

    location_contact_t *c = location_find_contact(&mgr, 0x1234);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL(LOCATION_TIER_FULL, c->tier);

    TEST_ASSERT_EQUAL(0, location_remove_contact(&mgr, 0x1234));
    TEST_ASSERT_EQUAL(1, mgr.contact_count);
    TEST_ASSERT_NULL(location_find_contact(&mgr, 0x1234));
    TEST_ASSERT_EQUAL(-1, location_remove_contact(&mgr, 0x1234)); /* not found */
}

void test_location_cache(void) {
    bramble_position_t pos = { .latitude_e7 = 100000000, .longitude_e7 = 200000000, .valid = true };

    TEST_ASSERT_EQUAL(0, location_cache_update(&mgr, 0xAA, &pos, 1000));
    const location_cache_entry_t *e = location_cache_get(&mgr, 0xAA);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL(100000000, e->pos.latitude_e7);

    /* Update existing */
    pos.latitude_e7 = 100010000;
    location_cache_update(&mgr, 0xAA, &pos, 2000);
    e = location_cache_get(&mgr, 0xAA);
    TEST_ASSERT_EQUAL(100010000, e->pos.latitude_e7);
    TEST_ASSERT_EQUAL(1, mgr.cache_count); /* no duplicate */

    /* Purge: not expired yet */
    location_cache_purge(&mgr, 2000 + LOCATION_CACHE_TTL_MS - 1);
    TEST_ASSERT_EQUAL(1, mgr.cache_count);

    /* Purge: expired */
    location_cache_purge(&mgr, 2000 + LOCATION_CACHE_TTL_MS + 1);
    TEST_ASSERT_EQUAL(0, mgr.cache_count);
}

void test_location_should_send_time(void) {
    location_add_contact(&mgr, 0x10, LOCATION_TIER_FULL);
    mgr.my_position.valid = true;

    /* Never sent -> should send */
    TEST_ASSERT_TRUE(location_should_send(&mgr, 0x10, 1000));

    /* Mark as sent */
    location_contact_t *c = location_find_contact(&mgr, 0x10);
    c->last_sent_ms = 1000;

    /* Not enough time elapsed */
    TEST_ASSERT_FALSE(location_should_send(&mgr, 0x10, 1000 + LOCATION_DEFAULT_INTERVAL_MS - 1));

    /* Enough time */
    TEST_ASSERT_TRUE(location_should_send(&mgr, 0x10, 1000 + LOCATION_DEFAULT_INTERVAL_MS));
}

void test_location_policy_defaults(void) {
    location_policy_t policy;
    location_policy_set_defaults(&policy);

    TEST_ASSERT_FALSE(policy.enabled);
    TEST_ASSERT_EQUAL(LOCATION_TIER_COARSE, policy.default_tier);
    TEST_ASSERT_EQUAL(LOCATION_DEFAULT_INTERVAL_S, policy.interval_s);
}

void test_location_policy_interval_floor(void) {
    location_policy_t policy = {
        .enabled = true,
        .default_tier = LOCATION_TIER_FULL,
        .interval_s = (uint16_t)(LOCATION_MIN_INTERVAL_S - 1)
    };

    location_policy_normalize(&policy);

    TEST_ASSERT_EQUAL(LOCATION_MIN_INTERVAL_S, policy.interval_s);
}

void test_location_should_send_distance(void) {
    location_add_contact(&mgr, 0x20, LOCATION_TIER_FULL);
    location_contact_t *c = location_find_contact(&mgr, 0x20);
    c->last_sent_ms = 1000; /* recently sent */

    /* Set current position */
    bramble_position_t pos1 = {
        .latitude_e7 = 374220000,
        .longitude_e7 = -1220840000,
        .valid = true
    };
    location_set_position(&mgr, &pos1);

    /* Cache peer's last known position (same as ours = where we were when we last sent) */
    location_cache_update(&mgr, 0x20, &pos1, 1000);

    /* Haven't moved much — time not elapsed */
    TEST_ASSERT_FALSE(location_should_send(&mgr, 0x20, 1500));

    /* Move >100m north (~0.001 degrees lat ~ 111m) */
    bramble_position_t pos2 = pos1;
    pos2.latitude_e7 += 15000; /* ~167m north */
    location_set_position(&mgr, &pos2);

    TEST_ASSERT_TRUE(location_should_send(&mgr, 0x20, 1500));
}

void test_location_policy_should_send_disabled(void) {
    location_policy_t policy = {
        .enabled = false,
        .default_tier = LOCATION_TIER_COARSE,
        .interval_s = LOCATION_DEFAULT_INTERVAL_S,
    };

    TEST_ASSERT_FALSE(location_policy_should_send(&policy, true, true, 1000, 0));
}

void test_location_policy_should_send_no_source(void) {
    location_policy_t policy = {
        .enabled = true,
        .default_tier = LOCATION_TIER_COARSE,
        .interval_s = LOCATION_DEFAULT_INTERVAL_S,
    };

    TEST_ASSERT_FALSE(location_policy_should_send(&policy, false, true, 1000, 0));
}

void test_location_policy_should_send_no_target(void) {
    location_policy_t policy = {
        .enabled = true,
        .default_tier = LOCATION_TIER_COARSE,
        .interval_s = LOCATION_DEFAULT_INTERVAL_S,
    };

    TEST_ASSERT_FALSE(location_policy_should_send(&policy, true, false, 1000, 0));
}

void test_location_policy_should_send_interval_not_reached(void) {
    location_policy_t policy = {
        .enabled = true,
        .default_tier = LOCATION_TIER_COARSE,
        .interval_s = 60,
    };

    TEST_ASSERT_FALSE(location_policy_should_send(&policy, true, true, 59000, 1000));
}

void test_location_policy_should_send_allowed_send(void) {
    location_policy_t policy = {
        .enabled = true,
        .default_tier = LOCATION_TIER_COARSE,
        .interval_s = 60,
    };

    TEST_ASSERT_TRUE(location_policy_should_send(&policy, true, true, 61000, 1000));
}

void test_location_deserialize_for_tier_full_coarse_presence(void) {
    bramble_position_t full = {
        .latitude_e7 = 374220000,
        .longitude_e7 = -1220840000,
        .altitude_m = 9,
        .accuracy_m = 7,
        .speed_kmh = 3,
        .heading_deg2 = 30,
        .timestamp = 1234,
        .valid = true,
    };

    uint8_t full_buf[LOCATION_FULL_SIZE];
    TEST_ASSERT_EQUAL(LOCATION_FULL_SIZE, location_serialize_full(&full, full_buf, sizeof(full_buf)));

    bramble_position_t out = {0};
    TEST_ASSERT_EQUAL(LOCATION_FULL_SIZE,
                      location_deserialize_for_tier(full_buf, sizeof(full_buf), LOCATION_TIER_FULL, &out));
    TEST_ASSERT_EQUAL(full.latitude_e7, out.latitude_e7);
    TEST_ASSERT_EQUAL(full.longitude_e7, out.longitude_e7);

    uint8_t coarse_buf[LOCATION_COARSE_SIZE];
    TEST_ASSERT_EQUAL(LOCATION_COARSE_SIZE, location_serialize_coarse(&full, coarse_buf, sizeof(coarse_buf)));
    memset(&out, 0, sizeof(out));
    TEST_ASSERT_EQUAL(LOCATION_COARSE_SIZE,
                      location_deserialize_for_tier(coarse_buf, sizeof(coarse_buf), LOCATION_TIER_COARSE, &out));
    TEST_ASSERT_TRUE(out.valid);

    uint8_t presence_buf[LOCATION_PRESENCE_SIZE];
    TEST_ASSERT_EQUAL(LOCATION_PRESENCE_SIZE, location_serialize_presence(&full, presence_buf, sizeof(presence_buf)));
    memset(&out, 0, sizeof(out));
    TEST_ASSERT_EQUAL(LOCATION_PRESENCE_SIZE,
                      location_deserialize_for_tier(presence_buf, sizeof(presence_buf), LOCATION_TIER_PRESENCE, &out));
    TEST_ASSERT_TRUE(out.valid);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_location_serialize_full_roundtrip);
    RUN_TEST(test_location_serialize_coarse_roundtrip);
    RUN_TEST(test_location_coarse_precision);
    RUN_TEST(test_location_contact_management);
    RUN_TEST(test_location_cache);
    RUN_TEST(test_location_policy_defaults);
    RUN_TEST(test_location_policy_interval_floor);
    RUN_TEST(test_location_should_send_time);
    RUN_TEST(test_location_should_send_distance);
    RUN_TEST(test_location_policy_should_send_disabled);
    RUN_TEST(test_location_policy_should_send_no_source);
    RUN_TEST(test_location_policy_should_send_no_target);
    RUN_TEST(test_location_policy_should_send_interval_not_reached);
    RUN_TEST(test_location_policy_should_send_allowed_send);
    RUN_TEST(test_location_deserialize_for_tier_full_coarse_presence);
    return UNITY_END();
}
