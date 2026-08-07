#include "unity.h"
#include "location.h"
#include <string.h>
#include <math.h>

static location_manager_t mgr;
extern int location_deserialize_for_tier(const uint8_t* buf, size_t len, uint8_t tier,
                                         bramble_position_t* pos);

void setUp(void) { location_init(&mgr); }
void tearDown(void) {}

void test_location_serialize_full_roundtrip(void) {
    bramble_position_t pos = {.latitude_e7 = 374220000,    /* ~37.422° N */
                              .longitude_e7 = -1220840000, /* ~-122.084° W */
                              .altitude_m = 42,
                              .accuracy_m = 10,
                              .speed_kmh = 5,
                              .heading_deg2 = 90,
                              .timestamp = 1700000000,
                              .valid = true};
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
        .latitude_e7 = 374225678, .longitude_e7 = -1220841234, .timestamp = 0x42, .valid = true};
    uint8_t buf[8];
    int n = location_serialize_coarse(&pos, buf, sizeof(buf));
    TEST_ASSERT_EQUAL(LOCATION_COARSE_SIZE, n);

    bramble_position_t out;
    n = location_deserialize_coarse(buf, LOCATION_COARSE_SIZE, &out);
    TEST_ASSERT_EQUAL(LOCATION_COARSE_SIZE, n);
    /* Coarse quantization: lat/10000 -> +90000 -> /3 -> *3 -> -90000 -> *10000
     * 374225678/10000 = 37422, +90000=127422, /3=42474, *3=127422, -90000=37422, *10000=374220000
     * lon: -1220841234/10000 = -122084, +180000=57916, /6=9652, *6=57912, -180000=-122088,
     * *10000=-1220880000 */
    TEST_ASSERT_EQUAL(374220000, out.latitude_e7);
    TEST_ASSERT_EQUAL(-1220880000, out.longitude_e7);
    TEST_ASSERT_TRUE(out.valid);
}

void test_location_coarse_precision(void) {
    /* Verify coarse position is within ~1.5km of original */
    bramble_position_t pos = {.latitude_e7 = 374229999, .longitude_e7 = -1220849999, .valid = true};
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

void test_location_cache(void) {
    bramble_position_t pos = {.latitude_e7 = 100000000, .longitude_e7 = 200000000, .valid = true};

    TEST_ASSERT_EQUAL(0, location_cache_update(&mgr, 0xAA, &pos, 1000));
    const location_cache_entry_t* e = location_cache_get(&mgr, 0xAA);
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

void test_location_policy_defaults(void) {
    location_policy_t policy;
    location_policy_set_defaults(&policy);

    TEST_ASSERT_FALSE(policy.enabled);
    TEST_ASSERT_EQUAL(LOCATION_TIER_COARSE, policy.default_tier);
    TEST_ASSERT_EQUAL(LOCATION_DEFAULT_INTERVAL_S, policy.interval_s);
}

void test_location_policy_interval_floor(void) {
    location_policy_t policy = {.enabled = true,
                                .default_tier = LOCATION_TIER_FULL,
                                .interval_s = (uint16_t)(LOCATION_MIN_INTERVAL_S - 1)};

    location_policy_normalize(&policy);

    TEST_ASSERT_EQUAL(LOCATION_MIN_INTERVAL_S, policy.interval_s);
}

void test_location_share_round_disabled(void) {
    location_policy_t policy = {
        .enabled = false,
        .default_tier = LOCATION_TIER_COARSE,
        .interval_s = LOCATION_DEFAULT_INTERVAL_S,
    };

    TEST_ASSERT_FALSE(location_share_round_enabled(&policy, true, 1));
}

void test_location_share_round_no_source(void) {
    location_policy_t policy = {
        .enabled = true,
        .default_tier = LOCATION_TIER_COARSE,
        .interval_s = LOCATION_DEFAULT_INTERVAL_S,
    };

    TEST_ASSERT_FALSE(location_share_round_enabled(&policy, false, 1));
}

void test_location_share_round_no_target(void) {
    location_policy_t policy = {
        .enabled = true,
        .default_tier = LOCATION_TIER_COARSE,
        .interval_s = LOCATION_DEFAULT_INTERVAL_S,
    };

    TEST_ASSERT_FALSE(location_share_round_enabled(&policy, true, 0));
}

void test_location_share_round_allowed(void) {
    location_policy_t policy = {
        .enabled = true,
        .default_tier = LOCATION_TIER_COARSE,
        .interval_s = 60,
    };

    TEST_ASSERT_TRUE(location_share_round_enabled(&policy, true, 1));
}

/* The defect this target-selection API exists to prevent: a channel target
   the config surface accepts and the send path never looks at. An enabled
   channel rule MUST resolve to a channel target. */
void test_location_target_from_entry_channel(void) {
    location_target_t target;
    memset(&target, 0, sizeof(target));

    TEST_ASSERT_TRUE(location_target_from_entry("lch_00", "1|coarse|60", &target));
    TEST_ASSERT_EQUAL(LOCATION_TARGET_CHANNEL, target.kind);
    TEST_ASSERT_EQUAL_UINT32(0, target.id);
    TEST_ASSERT_EQUAL(LOCATION_TIER_COARSE, target.tier);
    TEST_ASSERT_EQUAL_UINT16(60, target.interval_s);

    TEST_ASSERT_TRUE(location_target_from_entry("lch_07", "1|full|900", &target));
    TEST_ASSERT_EQUAL(LOCATION_TARGET_CHANNEL, target.kind);
    TEST_ASSERT_EQUAL_UINT32(7, target.id);
    TEST_ASSERT_EQUAL(LOCATION_TIER_FULL, target.tier);
    TEST_ASSERT_EQUAL_UINT16(900, target.interval_s);
}

void test_location_target_from_entry_contact(void) {
    location_target_t target;
    memset(&target, 0, sizeof(target));

    TEST_ASSERT_TRUE(location_target_from_entry("lcr_AABBCCDD", "1|presence|300", &target));
    TEST_ASSERT_EQUAL(LOCATION_TARGET_CONTACT, target.kind);
    TEST_ASSERT_EQUAL_UINT32(0xAABBCCDDu, target.id);
    TEST_ASSERT_EQUAL(LOCATION_TIER_PRESENCE, target.tier);
    TEST_ASSERT_EQUAL_UINT16(300, target.interval_s);
}

void test_location_target_from_entry_rejects_non_targets(void) {
    location_target_t target;

    /* Policy keys share the namespace and are not targets. */
    TEST_ASSERT_FALSE(location_target_from_entry("def_tier", "coarse", &target));
    TEST_ASSERT_FALSE(location_target_from_entry("interval_s", "300", &target));
    TEST_ASSERT_FALSE(location_target_from_entry("lp_AABBCCDD", "blob", &target));
    /* A disabled rule is a target switched off, not a target. */
    TEST_ASSERT_FALSE(location_target_from_entry("lch_00", "0|coarse|60", &target));
    TEST_ASSERT_FALSE(location_target_from_entry("lcr_AABBCCDD", "0|coarse|60", &target));
    /* Outside the channel key space, so it names no channel. */
    TEST_ASSERT_FALSE(location_target_from_entry("lch_99", "1|coarse|60", &target));
    TEST_ASSERT_FALSE(location_target_from_entry("lch_", "1|coarse|60", &target));
}

void test_location_target_interval_floor(void) {
    location_target_t target;

    TEST_ASSERT_TRUE(location_target_from_entry("lch_01", "1|coarse|5", &target));
    TEST_ASSERT_EQUAL_UINT16(LOCATION_MIN_INTERVAL_S, target.interval_s);
}

void test_location_rule_codec_roundtrip(void) {
    location_rule_t rule = {.enabled = true, .tier = LOCATION_TIER_FULL, .interval_s = 120};
    char buf[48];
    location_rule_format(buf, sizeof(buf), &rule);
    TEST_ASSERT_EQUAL_STRING("1|full|120", buf);

    location_rule_t parsed = {0};
    TEST_ASSERT_TRUE(location_rule_parse(buf, &parsed));
    TEST_ASSERT_TRUE(parsed.enabled);
    TEST_ASSERT_EQUAL(LOCATION_TIER_FULL, parsed.tier);
    TEST_ASSERT_EQUAL_UINT16(120, parsed.interval_s);

    /* A bare tier name reads as an enabled rule at the default interval. */
    TEST_ASSERT_TRUE(location_rule_parse("presence", &parsed));
    TEST_ASSERT_TRUE(parsed.enabled);
    TEST_ASSERT_EQUAL(LOCATION_TIER_PRESENCE, parsed.tier);
    TEST_ASSERT_EQUAL_UINT16(LOCATION_DEFAULT_INTERVAL_S, parsed.interval_s);
}

void test_location_target_keys(void) {
    char key[LOCATION_TARGET_KEY_SIZE];

    TEST_ASSERT_TRUE(location_contact_key(key, sizeof(key), 0xAABBCCDDu));
    TEST_ASSERT_EQUAL_STRING("lcr_AABBCCDD", key);

    TEST_ASSERT_TRUE(location_channel_key(key, sizeof(key), 0));
    TEST_ASSERT_EQUAL_STRING("lch_00", key);
    TEST_ASSERT_TRUE(location_channel_key(key, sizeof(key), 15));
    TEST_ASSERT_EQUAL_STRING("lch_15", key);

    /* Out-of-range indices are refused rather than written as a key nothing
       will ever match. */
    TEST_ASSERT_FALSE(location_channel_key(key, sizeof(key), -1));
    TEST_ASSERT_FALSE(location_channel_key(key, sizeof(key), LOCATION_MAX_CHANNEL_TARGETS));

    /* A key built for a channel round-trips back to the same target. */
    location_target_t target;
    TEST_ASSERT_TRUE(location_channel_key(key, sizeof(key), 3));
    TEST_ASSERT_TRUE(location_target_from_entry(key, "1|coarse|60", &target));
    TEST_ASSERT_EQUAL(LOCATION_TARGET_CHANNEL, target.kind);
    TEST_ASSERT_EQUAL_UINT32(3, target.id);
}

void test_location_targets_min_interval(void) {
    location_target_t targets[3] = {
        {.kind = LOCATION_TARGET_CONTACT, .id = 1, .tier = 0, .interval_s = 900},
        {.kind = LOCATION_TARGET_CHANNEL, .id = 0, .tier = 0, .interval_s = 60},
        {.kind = LOCATION_TARGET_CONTACT, .id = 2, .tier = 0, .interval_s = 300},
    };

    TEST_ASSERT_EQUAL_UINT16(60, location_targets_min_interval_s(targets, 3));
    TEST_ASSERT_EQUAL_UINT16(0, location_targets_min_interval_s(targets, 0));
}

void test_location_schedule_paces_each_target_separately(void) {
    location_schedule_t sched;
    location_schedule_init(&sched);

    location_target_t fast = {
        .kind = LOCATION_TARGET_CHANNEL, .id = 0, .tier = 0, .interval_s = 60};
    location_target_t slow = {
        .kind = LOCATION_TARGET_CONTACT, .id = 0xAABBCCDDu, .tier = 0, .interval_s = 600};

    /* Never attempted: both share on the first round. */
    TEST_ASSERT_TRUE(location_schedule_is_due(&sched, &fast, 1000));
    TEST_ASSERT_TRUE(location_schedule_is_due(&sched, &slow, 1000));

    location_schedule_record(&sched, &fast, 1000, true);
    location_schedule_record(&sched, &slow, 1000, true);

    TEST_ASSERT_FALSE(location_schedule_is_due(&sched, &fast, 1000 + 59000));
    TEST_ASSERT_TRUE(location_schedule_is_due(&sched, &fast, 1000 + 60000));
    /* The fast target coming due does not drag the slow one along. */
    TEST_ASSERT_FALSE(location_schedule_is_due(&sched, &slow, 1000 + 60000));
    TEST_ASSERT_TRUE(location_schedule_is_due(&sched, &slow, 1000 + 600000));
}

/* A failed channel broadcast is a transient radio-gate condition, so it
   retries soon. A failed contact unicast is a missing DM session, which a
   fast retry cannot fix, so it waits out its interval. */
void test_location_schedule_failure_backoff_differs_by_kind(void) {
    location_schedule_t sched;
    location_schedule_init(&sched);

    location_target_t channel = {
        .kind = LOCATION_TARGET_CHANNEL, .id = 0, .tier = 0, .interval_s = 600};
    location_target_t contact = {
        .kind = LOCATION_TARGET_CONTACT, .id = 7, .tier = 0, .interval_s = 600};

    location_schedule_record(&sched, &channel, 1000, false);
    location_schedule_record(&sched, &contact, 1000, false);

    uint32_t retry_ms = (uint32_t)LOCATION_SEND_RETRY_S * 1000U;
    TEST_ASSERT_FALSE(location_schedule_is_due(&sched, &channel, 1000 + retry_ms - 1));
    TEST_ASSERT_TRUE(location_schedule_is_due(&sched, &channel, 1000 + retry_ms));
    TEST_ASSERT_FALSE(location_schedule_is_due(&sched, &contact, 1000 + retry_ms));
    TEST_ASSERT_TRUE(location_schedule_is_due(&sched, &contact, 1000 + 600000));
}

/* The retry never stretches past the target's own interval. */
void test_location_schedule_retry_never_exceeds_interval(void) {
    location_schedule_t sched;
    location_schedule_init(&sched);

    location_target_t channel = {
        .kind = LOCATION_TARGET_CHANNEL, .id = 0, .tier = 0, .interval_s = LOCATION_MIN_INTERVAL_S};

    location_schedule_record(&sched, &channel, 0, false);
    TEST_ASSERT_TRUE(
        location_schedule_is_due(&sched, &channel, (uint32_t)LOCATION_SEND_RETRY_S * 1000U));
}

void test_location_schedule_retain_releases_removed_targets(void) {
    location_schedule_t sched;
    location_schedule_init(&sched);

    location_target_t targets[2] = {
        {.kind = LOCATION_TARGET_CHANNEL, .id = 0, .tier = 0, .interval_s = 600},
        {.kind = LOCATION_TARGET_CONTACT, .id = 9, .tier = 0, .interval_s = 600},
    };

    location_schedule_record(&sched, &targets[0], 1000, true);
    location_schedule_record(&sched, &targets[1], 1000, true);
    TEST_ASSERT_FALSE(location_schedule_is_due(&sched, &targets[0], 2000));

    /* Dropping the channel target from the config releases its slot, so
       adding it back shares immediately instead of inheriting old pacing. */
    location_schedule_retain(&sched, &targets[1], 1);
    TEST_ASSERT_TRUE(location_schedule_is_due(&sched, &targets[0], 2000));
    TEST_ASSERT_FALSE(location_schedule_is_due(&sched, &targets[1], 2000));
}

/* The mesh clock wraps, so pacing has to be wrap-safe. */
void test_location_schedule_wraps_with_the_mesh_clock(void) {
    location_schedule_t sched;
    location_schedule_init(&sched);

    location_target_t channel = {
        .kind = LOCATION_TARGET_CHANNEL, .id = 2, .tier = 0, .interval_s = 60};

    uint32_t before_wrap = 0xFFFFFFFFu - 30000U;
    location_schedule_record(&sched, &channel, before_wrap, true);

    TEST_ASSERT_FALSE(location_schedule_is_due(&sched, &channel, before_wrap + 59000U));
    TEST_ASSERT_TRUE(location_schedule_is_due(&sched, &channel, before_wrap + 60000U));
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
    TEST_ASSERT_EQUAL(LOCATION_FULL_SIZE,
                      location_serialize_full(&full, full_buf, sizeof(full_buf)));

    bramble_position_t out = {0};
    TEST_ASSERT_EQUAL(LOCATION_FULL_SIZE, location_deserialize_for_tier(full_buf, sizeof(full_buf),
                                                                        LOCATION_TIER_FULL, &out));
    TEST_ASSERT_EQUAL(full.latitude_e7, out.latitude_e7);
    TEST_ASSERT_EQUAL(full.longitude_e7, out.longitude_e7);

    uint8_t coarse_buf[LOCATION_COARSE_SIZE];
    TEST_ASSERT_EQUAL(LOCATION_COARSE_SIZE,
                      location_serialize_coarse(&full, coarse_buf, sizeof(coarse_buf)));
    memset(&out, 0, sizeof(out));
    TEST_ASSERT_EQUAL(
        LOCATION_COARSE_SIZE,
        location_deserialize_for_tier(coarse_buf, sizeof(coarse_buf), LOCATION_TIER_COARSE, &out));
    TEST_ASSERT_TRUE(out.valid);

    uint8_t presence_buf[LOCATION_PRESENCE_SIZE];
    TEST_ASSERT_EQUAL(LOCATION_PRESENCE_SIZE,
                      location_serialize_presence(&full, presence_buf, sizeof(presence_buf)));
    memset(&out, 0, sizeof(out));
    TEST_ASSERT_EQUAL(LOCATION_PRESENCE_SIZE,
                      location_deserialize_for_tier(presence_buf, sizeof(presence_buf),
                                                    LOCATION_TIER_PRESENCE, &out));
    TEST_ASSERT_TRUE(out.valid);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_location_serialize_full_roundtrip);
    RUN_TEST(test_location_serialize_coarse_roundtrip);
    RUN_TEST(test_location_coarse_precision);
    RUN_TEST(test_location_cache);
    RUN_TEST(test_location_policy_defaults);
    RUN_TEST(test_location_policy_interval_floor);
    RUN_TEST(test_location_share_round_disabled);
    RUN_TEST(test_location_share_round_no_source);
    RUN_TEST(test_location_share_round_no_target);
    RUN_TEST(test_location_share_round_allowed);
    RUN_TEST(test_location_target_from_entry_channel);
    RUN_TEST(test_location_target_from_entry_contact);
    RUN_TEST(test_location_target_from_entry_rejects_non_targets);
    RUN_TEST(test_location_target_interval_floor);
    RUN_TEST(test_location_rule_codec_roundtrip);
    RUN_TEST(test_location_target_keys);
    RUN_TEST(test_location_targets_min_interval);
    RUN_TEST(test_location_schedule_paces_each_target_separately);
    RUN_TEST(test_location_schedule_failure_backoff_differs_by_kind);
    RUN_TEST(test_location_schedule_retry_never_exceeds_interval);
    RUN_TEST(test_location_schedule_retain_releases_removed_targets);
    RUN_TEST(test_location_schedule_wraps_with_the_mesh_clock);
    RUN_TEST(test_location_deserialize_for_tier_full_coarse_presence);
    return UNITY_END();
}
