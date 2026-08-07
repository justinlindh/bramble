/*
 * Persisted peer locations: the on-flash record, the boot counter that makes
 * its timestamp interpretable, and the boot-time restore that refills the
 * in-RAM cache the map draws.
 *
 * The bug these cover: peer positions were written to flash on receipt and
 * never read back, so after a reboot the map drew nothing while
 * bramble.getPeerLocations (which reads flash directly) still listed every
 * peer. A reboot is simulated by resetting module state while the fake flash
 * keeps its contents, which is exactly what a real reset does.
 *
 * Coordinates here are fictional.
 */
#include "unity.h"
#include "location.h"
#include "nvs_fake.h"
#include "nvs_keys.h"

#include <string.h>

#define FICTIONAL_LAT_E7 123456700  /* 12.34567 N */
#define FICTIONAL_LON_E7 -456789000 /* 45.6789 W */

static location_manager_t mgr;

void setUp(void) {
    nvs_fake_reset();
    location_store_reset_boot_state();
    location_init(&mgr);
}

void tearDown(void) {}

static bramble_position_t make_pos(int32_t lat_e7, int32_t lon_e7) {
    bramble_position_t pos;
    memset(&pos, 0, sizeof(pos));
    pos.latitude_e7 = lat_e7;
    pos.longitude_e7 = lon_e7;
    pos.altitude_m = 100;
    pos.accuracy_m = 8;
    pos.speed_kmh = 3;
    pos.heading_deg2 = 45;
    pos.timestamp = 4242;
    pos.valid = true;
    return pos;
}

/* Restart: module statics forgotten, flash kept. */
static void simulate_reboot(void) {
    location_store_reset_boot_state();
    location_init(&mgr);
}

/* ── Boot counter ───────────────────────────────────────────────────────── */

void test_boot_counter_starts_at_one_and_advances_each_boot(void) {
    TEST_ASSERT_EQUAL_UINT32(1, location_store_begin_boot());
    TEST_ASSERT_EQUAL_UINT32(1, location_store_boot_id());

    /* Same boot: no second bump. */
    TEST_ASSERT_EQUAL_UINT32(1, location_store_begin_boot());

    simulate_reboot();
    TEST_ASSERT_EQUAL_UINT32(2, location_store_begin_boot());

    simulate_reboot();
    TEST_ASSERT_EQUAL_UINT32(3, location_store_begin_boot());
}

void test_boot_counter_is_zero_when_flash_is_unreachable(void) {
    nvs_fake_set_open_fails(true);
    /* Zero is the reserved "unknown boot" value, so every stored record reads
     * as age-unknown rather than as belonging to this boot. */
    TEST_ASSERT_EQUAL_UINT32(0, location_store_begin_boot());
    TEST_ASSERT_EQUAL_UINT32(0, location_store_boot_id());
}

/* ── Record codec ───────────────────────────────────────────────────────── */

void test_record_roundtrips_within_one_boot(void) {
    bramble_position_t pos = make_pos(FICTIONAL_LAT_E7, FICTIONAL_LON_E7);
    persisted_peer_location_t rec;
    peer_location_record_encode(&rec, &pos, LOCATION_TIER_FULL, 90000, 7);
    TEST_ASSERT_EQUAL_UINT32(27, sizeof(rec));

    peer_location_record_t out;
    TEST_ASSERT_EQUAL(0, peer_location_record_decode(&rec, sizeof(rec), 7, &out));
    TEST_ASSERT_EQUAL_INT32(FICTIONAL_LAT_E7, out.pos.latitude_e7);
    TEST_ASSERT_EQUAL_INT32(FICTIONAL_LON_E7, out.pos.longitude_e7);
    TEST_ASSERT_EQUAL(LOCATION_TIER_FULL, out.tier);
    TEST_ASSERT_EQUAL_UINT32(90000, out.received_ms);
    TEST_ASSERT_TRUE(out.age_known);
}

void test_record_from_another_boot_has_no_computable_age(void) {
    bramble_position_t pos = make_pos(FICTIONAL_LAT_E7, FICTIONAL_LON_E7);
    persisted_peer_location_t rec;
    /* The exact shape seen on the bench: a stored uptime LARGER than the
     * current one, because it was measured against a clock that has since
     * restarted. */
    peer_location_record_encode(&rec, &pos, LOCATION_TIER_FULL, 3601167, 7);

    peer_location_record_t out;
    TEST_ASSERT_EQUAL(0, peer_location_record_decode(&rec, sizeof(rec), 8, &out));
    TEST_ASSERT_FALSE(out.age_known);
    /* And the position itself still comes back: unknown age, known place. */
    TEST_ASSERT_EQUAL_INT32(FICTIONAL_LAT_E7, out.pos.latitude_e7);
    TEST_ASSERT_FALSE(location_age_is_fresh(out.age_known, out.received_ms, 900000));
}

void test_legacy_record_without_boot_id_decodes_as_age_unknown(void) {
    /* A record written by a build that had no version or boot_id: the new
     * fields are a pure append, so the old bytes are the leading prefix. */
    bramble_position_t pos = make_pos(FICTIONAL_LAT_E7, FICTIONAL_LON_E7);
    persisted_peer_location_t rec;
    peer_location_record_encode(&rec, &pos, LOCATION_TIER_COARSE, 5000, 3);

    peer_location_record_t out;
    TEST_ASSERT_EQUAL(0,
                      peer_location_record_decode(&rec, PEER_LOCATION_RECORD_V0_SIZE, 3, &out));
    TEST_ASSERT_EQUAL_INT32(FICTIONAL_LAT_E7, out.pos.latitude_e7);
    TEST_ASSERT_EQUAL(LOCATION_TIER_COARSE, out.tier);
    TEST_ASSERT_EQUAL_UINT32(5000, out.received_ms);
    TEST_ASSERT_EQUAL_UINT32(0, out.boot_id);
    TEST_ASSERT_FALSE(out.age_known);
}

void test_record_of_unknown_length_is_rejected(void) {
    uint8_t junk[40] = {0};
    peer_location_record_t out;
    TEST_ASSERT_EQUAL(-1, peer_location_record_decode(junk, 7, 1, &out));
    TEST_ASSERT_EQUAL(-1, peer_location_record_decode(junk, sizeof(junk), 1, &out));
}

void test_record_with_unknown_version_is_rejected(void) {
    bramble_position_t pos = make_pos(FICTIONAL_LAT_E7, FICTIONAL_LON_E7);
    persisted_peer_location_t rec;
    peer_location_record_encode(&rec, &pos, LOCATION_TIER_FULL, 1000, 1);
    rec.version = PEER_LOCATION_RECORD_VERSION + 9;

    peer_location_record_t out;
    TEST_ASSERT_EQUAL(-1, peer_location_record_decode(&rec, sizeof(rec), 1, &out));
}

void test_record_key_roundtrip(void) {
    char key[PEER_LOCATION_KEY_MAX];
    peer_location_record_key(key, sizeof(key), 0xA1B2C3D4u);
    TEST_ASSERT_EQUAL_STRING("lp_A1B2C3D4", key);

    uint32_t addr = 0;
    TEST_ASSERT_TRUE(peer_location_key_parse(key, &addr));
    TEST_ASSERT_EQUAL_HEX32(0xA1B2C3D4u, addr);

    /* Other keys in the same namespace are not peer records. */
    TEST_ASSERT_FALSE(peer_location_key_parse("lcr_A1B2C3D4", &addr));
    TEST_ASSERT_FALSE(peer_location_key_parse("boot_id", &addr));
    TEST_ASSERT_FALSE(peer_location_key_parse("lp_", &addr));
}

/* ── Restore across a reboot ────────────────────────────────────────────── */

void test_restore_repopulates_the_cache_after_a_reboot(void) {
    location_store_begin_boot();
    bramble_position_t pos = make_pos(FICTIONAL_LAT_E7, FICTIONAL_LON_E7);
    location_store_save_peer(0xD0C9D311u, &pos, LOCATION_TIER_FULL, 50000);
    location_cache_apply_share(&mgr, 0xD0C9D311u, LOCATION_TIER_FULL, &pos, 50000);
    TEST_ASSERT_EQUAL(1, mgr.cache_count);

    simulate_reboot();
    TEST_ASSERT_EQUAL(0, mgr.cache_count); /* the boot that lost the map */

    location_store_begin_boot();
    peer_location_restore_entry_t entries[LOCATION_MAX_CONTACTS];
    int n = location_store_collect(entries, LOCATION_MAX_CONTACTS);
    TEST_ASSERT_EQUAL(1, n);
    location_store_apply(&mgr, entries, n);

    TEST_ASSERT_EQUAL(1, mgr.cache_count);
    const location_cache_entry_t* e = location_cache_get(&mgr, 0xD0C9D311u);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_INT32(FICTIONAL_LAT_E7, e->pos.latitude_e7);
    TEST_ASSERT_EQUAL_INT32(FICTIONAL_LON_E7, e->pos.longitude_e7);
    TEST_ASSERT_TRUE(e->active);
    /* Restored, so it is the peer's last known position, not a current fix. */
    TEST_ASSERT_FALSE(e->age_known);
    TEST_ASSERT_FALSE(location_age_is_fresh(e->age_known, e->received_ms, 1000));
}

void test_restored_entry_becomes_live_again_on_a_fresh_share(void) {
    location_store_begin_boot();
    bramble_position_t pos = make_pos(FICTIONAL_LAT_E7, FICTIONAL_LON_E7);
    location_store_save_peer(0x3575D5D7u, &pos, LOCATION_TIER_FULL, 50000);

    simulate_reboot();
    location_store_begin_boot();
    peer_location_restore_entry_t entries[LOCATION_MAX_CONTACTS];
    int n = location_store_collect(entries, LOCATION_MAX_CONTACTS);
    location_store_apply(&mgr, entries, n);
    TEST_ASSERT_FALSE(location_cache_get(&mgr, 0x3575D5D7u)->age_known);

    bramble_position_t moved = make_pos(FICTIONAL_LAT_E7 + 1000, FICTIONAL_LON_E7 + 1000);
    location_cache_apply_share(&mgr, 0x3575D5D7u, LOCATION_TIER_FULL, &moved, 12000);

    const location_cache_entry_t* e = location_cache_get(&mgr, 0x3575D5D7u);
    TEST_ASSERT_EQUAL(1, mgr.cache_count); /* updated in place, not duplicated */
    TEST_ASSERT_TRUE(e->age_known);
    TEST_ASSERT_EQUAL_INT32(FICTIONAL_LAT_E7 + 1000, e->pos.latitude_e7);
    TEST_ASSERT_TRUE(location_age_is_fresh(e->age_known, e->received_ms, 13000));
}

void test_restore_keeps_the_most_recently_written_records_when_flash_overflows(void) {
    /* Flash holds no cap on lp_ keys while the cache has LOCATION_MAX_CONTACTS
     * slots, so which records survive has to be decided, not left to whatever
     * the directory happened to list first. */
    location_store_begin_boot();
    bramble_position_t pos = make_pos(FICTIONAL_LAT_E7, FICTIONAL_LON_E7);
    for (int i = 0; i < LOCATION_MAX_CONTACTS + 4; i++) {
        location_store_save_peer(0x1000u + (uint32_t)i, &pos, LOCATION_TIER_FULL,
                                 1000U * (uint32_t)(i + 1));
    }

    simulate_reboot();
    location_store_begin_boot();
    peer_location_restore_entry_t entries[LOCATION_MAX_CONTACTS];
    int n = location_store_collect(entries, LOCATION_MAX_CONTACTS);
    TEST_ASSERT_EQUAL(LOCATION_MAX_CONTACTS, n);

    /* Newest first, and the four oldest fell off. */
    TEST_ASSERT_EQUAL_HEX32(0x1000u + LOCATION_MAX_CONTACTS + 3, entries[0].peer_addr);
    TEST_ASSERT_EQUAL_HEX32(0x1004u, entries[n - 1].peer_addr);

    location_store_apply(&mgr, entries, n);
    TEST_ASSERT_EQUAL(LOCATION_MAX_CONTACTS, mgr.cache_count);
    TEST_ASSERT_NULL(location_cache_get(&mgr, 0x1000u));
}

void test_restore_prefers_records_from_the_later_boot(void) {
    location_store_begin_boot(); /* boot 1 */
    bramble_position_t pos = make_pos(FICTIONAL_LAT_E7, FICTIONAL_LON_E7);
    /* A high uptime in an EARLY boot must not outrank a low uptime in a LATER
     * one; ordering is (boot_id, received_ms), not received_ms alone. */
    location_store_save_peer(0xAAAAAAAAu, &pos, LOCATION_TIER_FULL, 3600000);

    simulate_reboot();
    location_store_begin_boot(); /* boot 2 */
    location_store_save_peer(0xBBBBBBBBu, &pos, LOCATION_TIER_FULL, 5000);

    simulate_reboot();
    location_store_begin_boot();
    peer_location_restore_entry_t entries[LOCATION_MAX_CONTACTS];
    int n = location_store_collect(entries, LOCATION_MAX_CONTACTS);
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL_HEX32(0xBBBBBBBBu, entries[0].peer_addr);
    TEST_ASSERT_EQUAL_HEX32(0xAAAAAAAAu, entries[1].peer_addr);
}

void test_restore_ignores_other_keys_in_the_namespace(void) {
    location_store_begin_boot();
    bramble_position_t pos = make_pos(FICTIONAL_LAT_E7, FICTIONAL_LON_E7);
    location_store_save_peer(0x10B76F29u, &pos, LOCATION_TIER_FULL, 7000);
    /* A contact rule and the boot counter share the namespace. */
    nvs_fake_put_blob(NVS_NS_LOCATION, "lcr_10B76F29", "1|full|60", 10);

    simulate_reboot();
    location_store_begin_boot();
    peer_location_restore_entry_t entries[LOCATION_MAX_CONTACTS];
    TEST_ASSERT_EQUAL(1, location_store_collect(entries, LOCATION_MAX_CONTACTS));
    TEST_ASSERT_EQUAL_HEX32(0x10B76F29u, entries[0].peer_addr);
}

void test_presence_records_are_not_placed_on_the_map(void) {
    /* A PRESENCE share carries an online bit and no coordinates: its position
     * struct is zeroed with valid set. Restoring it as a position would plant
     * a marker on Null Island. */
    location_store_begin_boot();
    bramble_position_t presence;
    memset(&presence, 0, sizeof(presence));
    presence.valid = true;
    location_store_save_peer(0xC0FFEE01u, &presence, LOCATION_TIER_PRESENCE, 3000);

    simulate_reboot();
    location_store_begin_boot();
    peer_location_restore_entry_t entries[LOCATION_MAX_CONTACTS];
    int n = location_store_collect(entries, LOCATION_MAX_CONTACTS);
    TEST_ASSERT_EQUAL(1, n); /* the record is real */
    location_store_apply(&mgr, entries, n);
    TEST_ASSERT_EQUAL(0, mgr.cache_count); /* but it is not a place */
}

void test_presence_share_withdraws_coordinates_already_cached(void) {
    /* Downgrading to PRESENCE is a deliberate privacy choice. Keeping the last
     * exact position on the map would show precisely what the sender just
     * stopped sharing. */
    bramble_position_t pos = make_pos(FICTIONAL_LAT_E7, FICTIONAL_LON_E7);
    location_cache_apply_share(&mgr, 0xC0FFEE02u, LOCATION_TIER_FULL, &pos, 1000);
    TEST_ASSERT_NOT_NULL(location_cache_get(&mgr, 0xC0FFEE02u));

    bramble_position_t presence;
    memset(&presence, 0, sizeof(presence));
    presence.valid = true;
    location_cache_apply_share(&mgr, 0xC0FFEE02u, LOCATION_TIER_PRESENCE, &presence, 2000);
    TEST_ASSERT_NULL(location_cache_get(&mgr, 0xC0FFEE02u));
    TEST_ASSERT_EQUAL(0, mgr.cache_count);
}

void test_restore_finds_nothing_on_a_fresh_device(void) {
    location_store_begin_boot();
    peer_location_restore_entry_t entries[LOCATION_MAX_CONTACTS];
    TEST_ASSERT_EQUAL(0, location_store_collect(entries, LOCATION_MAX_CONTACTS));
    location_store_apply(&mgr, entries, 0);
    TEST_ASSERT_EQUAL(0, mgr.cache_count);
}

/* ── Purge ──────────────────────────────────────────────────────────────── */

void test_purge_leaves_restored_entries_alone(void) {
    location_store_begin_boot();
    bramble_position_t pos = make_pos(FICTIONAL_LAT_E7, FICTIONAL_LON_E7);
    location_store_save_peer(0x83F9B4E9u, &pos, LOCATION_TIER_FULL, 40000);

    simulate_reboot();
    location_store_begin_boot();
    peer_location_restore_entry_t entries[LOCATION_MAX_CONTACTS];
    int n = location_store_collect(entries, LOCATION_MAX_CONTACTS);
    location_store_apply(&mgr, entries, n);
    TEST_ASSERT_EQUAL(1, mgr.cache_count);

    /* Well past the TTL measured against the restored (meaningless) uptime:
     * there is no age to expire, so the last known position stays. */
    location_cache_purge(&mgr, 40000 + LOCATION_CACHE_TTL_MS * 2);
    TEST_ASSERT_EQUAL(1, mgr.cache_count);

    /* A share received in THIS boot has a real age and does expire. */
    location_cache_apply_share(&mgr, 0x83F9B4E9u, LOCATION_TIER_FULL, &pos, 1000);
    location_cache_purge(&mgr, 1000 + LOCATION_CACHE_TTL_MS + 1);
    TEST_ASSERT_EQUAL(0, mgr.cache_count);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_boot_counter_starts_at_one_and_advances_each_boot);
    RUN_TEST(test_boot_counter_is_zero_when_flash_is_unreachable);
    RUN_TEST(test_record_roundtrips_within_one_boot);
    RUN_TEST(test_record_from_another_boot_has_no_computable_age);
    RUN_TEST(test_legacy_record_without_boot_id_decodes_as_age_unknown);
    RUN_TEST(test_record_of_unknown_length_is_rejected);
    RUN_TEST(test_record_with_unknown_version_is_rejected);
    RUN_TEST(test_record_key_roundtrip);
    RUN_TEST(test_restore_repopulates_the_cache_after_a_reboot);
    RUN_TEST(test_restored_entry_becomes_live_again_on_a_fresh_share);
    RUN_TEST(test_restore_keeps_the_most_recently_written_records_when_flash_overflows);
    RUN_TEST(test_restore_prefers_records_from_the_later_boot);
    RUN_TEST(test_restore_ignores_other_keys_in_the_namespace);
    RUN_TEST(test_presence_records_are_not_placed_on_the_map);
    RUN_TEST(test_presence_share_withdraws_coordinates_already_cached);
    RUN_TEST(test_restore_finds_nothing_on_a_fresh_device);
    RUN_TEST(test_purge_leaves_restored_entries_alone);
    return UNITY_END();
}
