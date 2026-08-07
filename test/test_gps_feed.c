#include "unity.h"
#include "gps_feed.h"
#include <stdio.h>
#include <string.h>

static gps_feed_t f;
static int cb_count;
static bramble_position_t cb_last;
static void on_fix(const bramble_position_t* p, void* ctx) {
    (void)ctx;
    cb_count++;
    cb_last = *p;
}
void setUp(void) {
    memset(&f, 0, sizeof(f));
    cb_count = 0;
    gps_feed_init(&f, on_fix, NULL);
}
void tearDown(void) {}

static const char RMC[] = "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A";
static const char GGA[] = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47";
static const char GSV[] = "$GPGSV,3,1,11,10,63,137,17,07,61,308,17,05,59,169,18,30,54,042,*7D";

void test_bytes_assemble_and_fix(void) {
    char buf[160];
    snprintf(buf, sizeof(buf), "%s\r\n", RMC);
    gps_feed_bytes(&f, (const uint8_t*)buf, strlen(buf), 5000);
    TEST_ASSERT_TRUE(gps_feed_has_fix(&f, 5000));
    TEST_ASSERT_EQUAL_INT(1, cb_count);
    TEST_ASSERT_INT32_WITHIN(1000, 481173000, cb_last.latitude_e7);
    TEST_ASSERT_EQUAL_UINT32(5u, cb_last.timestamp); /* now_ms/1000 */
}

void test_dollar_mid_line_restarts(void) {
    const char garbled[] =
        "$GPRMC,123519,A,48$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n";
    gps_feed_bytes(&f, (const uint8_t*)garbled, strlen(garbled), 1000);
    TEST_ASSERT_TRUE(gps_feed_has_fix(&f, 1000)); /* the GGA after the restart parses */
    TEST_ASSERT_EQUAL_UINT8(8, f.sats_used);
}

void test_overlong_line_truncates_without_crash(void) {
    uint8_t big[300];
    memset(big, 'A', sizeof(big));
    big[0] = '$';
    gps_feed_bytes(&f, big, sizeof(big), 1000);
    gps_feed_bytes(&f, (const uint8_t*)"\r\n", 2, 1000);
    TEST_ASSERT_FALSE(gps_feed_has_fix(&f, 1000));
}

void test_gga_sats_used_without_fix(void) {
    /* fix quality 0: no fix, but sats_used still reported */
    const char nofix[] = "$GPGGA,123519,,,,,0,05,,,M,,M,,*5C";
    gps_feed_line(&f, nofix, 1000);
    TEST_ASSERT_FALSE(gps_feed_has_fix(&f, 1000));
    gps_stats_t st;
    gps_feed_get_stats(&f, 1000, &st);
    TEST_ASSERT_EQUAL_UINT8(5, st.sats_used);
}

void test_merge_gga_altitude_onto_rmc_fix(void) {
    gps_feed_line(&f, RMC, 1000);
    gps_feed_line(&f, GGA, 2000);
    bramble_position_t p;
    TEST_ASSERT_TRUE(gps_feed_get_position(&f, 2000, &p));
    TEST_ASSERT_EQUAL_INT16(545, p.altitude_m); /* from GGA */
    TEST_ASSERT_EQUAL_UINT8(41, p.speed_kmh);   /* survives from RMC */
}

void test_antenna_warning_ttl_expires(void) {
    gps_feed_line(&f, "$GPTXT,01,01,01,ANTENNA OPEN*25", 1000);
    gps_stats_t st;
    gps_feed_get_stats(&f, 2000, &st);
    TEST_ASSERT_TRUE(st.antenna_warning);
    gps_feed_get_stats(&f, 1000 + 60001, &st);
    TEST_ASSERT_FALSE(st.antenna_warning);
}

void test_utc_latched_from_gga(void) {
    gps_feed_line(&f, GGA, 1000);
    uint8_t h, m;
    TEST_ASSERT_TRUE(gps_feed_get_utc_hm(&f, 1000, &h, &m));
    TEST_ASSERT_EQUAL_UINT8(12, h);
    TEST_ASSERT_EQUAL_UINT8(35, m);
}

void test_chip_banner_captured_once(void) {
    gps_feed_line(&f, "$PAIR021,AG3335M,V1.0*3A", 1000);
    gps_feed_line(&f, "$PAIR021,SECOND*00", 1000);
    TEST_ASSERT_EQUAL_STRING("$PAIR021,AG3335M,V1.0*3A", f.chip_banner);
}

void test_reset_clears_fix_keeps_cb(void) {
    gps_feed_line(&f, RMC, 1000);
    gps_feed_reset(&f);
    TEST_ASSERT_FALSE(gps_feed_has_fix(&f, 1000));
    gps_feed_line(&f, RMC, 2000);
    TEST_ASSERT_EQUAL_INT(2, cb_count); /* cb survived the reset */
}

void test_clear_fix_hides_position_and_utc_but_keeps_stats(void) {
    char buf[160];
    snprintf(buf, sizeof(buf), "%s\r\n", GSV);
    gps_feed_bytes(&f, (const uint8_t*)buf, strlen(buf), 1000); /* sats_in_view + rx counters */
    gps_feed_line(&f, GGA, 1000);                               /* sats_used + UTC latch */
    gps_feed_line(&f, RMC, 1000);                               /* fix */
    gps_feed_line(&f, "$PAIR021,AG3335M,V1.0*3A", 1000);        /* chip banner */
    TEST_ASSERT_TRUE(gps_feed_has_fix(&f, 1000));
    uint8_t h, m;
    TEST_ASSERT_TRUE(gps_feed_get_utc_hm(&f, 1000, &h, &m));
    uint32_t rx_lines_before = f.rx_lines_total;

    gps_feed_clear_fix(&f);

    TEST_ASSERT_FALSE(gps_feed_has_fix(&f, 1000));
    bramble_position_t p;
    TEST_ASSERT_FALSE(gps_feed_get_position(&f, 1000, &p));
    TEST_ASSERT_FALSE(gps_feed_get_utc_hm(&f, 1000, &h, &m));

    /* Sats/antenna stats, the GSV slots, the GGA fix quality, rx counters,
     * and the chip banner all survive. */
    gps_stats_t st;
    gps_feed_get_stats(&f, 1000, &st);
    TEST_ASSERT_EQUAL_UINT8(8, st.sats_used);
    TEST_ASSERT_EQUAL_UINT8(11, st.sats_in_view);
    TEST_ASSERT_EQUAL_UINT8(1, st.fix_quality);
    TEST_ASSERT_EQUAL_UINT32(rx_lines_before, f.rx_lines_total);
    TEST_ASSERT_EQUAL_STRING("$PAIR021,AG3335M,V1.0*3A", f.chip_banner);

    /* A fresh valid sentence re-fixes and fires the callback again. */
    int calls_before = cb_count;
    gps_feed_line(&f, RMC, 2000);
    TEST_ASSERT_TRUE(gps_feed_has_fix(&f, 2000));
    TEST_ASSERT_EQUAL_INT(calls_before + 1, cb_count);
}

void test_gsv_updates_sats_in_view(void) {
    gps_feed_line(&f, GSV, 1000);
    gps_stats_t st;
    gps_feed_get_stats(&f, 1000, &st);
    TEST_ASSERT_EQUAL_UINT8(11, st.sats_in_view);
}

/* A single-message GSV cycle: total_msgs == msg_num == 1 commits immediately. */
static const char GPGSV_8[] = "$GPGSV,1,1,08,10,63,137,17,07,61,308,21*70";
static const char GLGSV_6[] = "$GLGSV,1,1,06,65,45,120,33,66,20,300,28*70";

void test_gsv_multi_constellation_sums(void) {
    gps_feed_line(&f, GPGSV_8, 1000);
    gps_feed_line(&f, GLGSV_6, 1000);
    gps_stats_t st;
    gps_feed_get_stats(&f, 1000, &st);
    TEST_ASSERT_EQUAL_UINT8(14, st.sats_in_view);
    TEST_ASSERT_EQUAL_UINT8(4, st.sats_tracked);
    TEST_ASSERT_EQUAL_UINT8(33, st.snr_max_dbhz);
}

void test_gsv_repeat_within_cycle_does_not_double_count(void) {
    /* Every message of a cycle repeats the same field-3 total. */
    gps_feed_line(&f, "$GPGSV,3,1,11,10,63,137,17*70", 1000);
    gps_feed_line(&f, "$GPGSV,3,2,11,07,61,308,18*70", 1000);
    gps_feed_line(&f, "$GPGSV,3,3,11,05,59,169,19*70", 1000);
    gps_stats_t st;
    gps_feed_get_stats(&f, 1000, &st);
    TEST_ASSERT_EQUAL_UINT8(11, st.sats_in_view);
}

void test_gsv_tracked_accumulates_across_cycle(void) {
    gps_stats_t st;
    gps_feed_line(&f, "$GPGSV,3,1,11,10,63,137,17,07,61,308,18,05,59,169,19,30,54,042,20*70", 1000);
    gps_feed_get_stats(&f, 1000, &st);
    TEST_ASSERT_EQUAL_UINT8(0, st.sats_tracked); /* cycle in flight, nothing committed */

    gps_feed_line(&f, "$GPGSV,3,2,11,11,63,137,21,12,61,308,22,13,59,169,23,14,54,042,24*70", 1000);
    gps_feed_get_stats(&f, 1000, &st);
    TEST_ASSERT_EQUAL_UINT8(0, st.sats_tracked);

    gps_feed_line(&f, "$GPGSV,3,3,11,15,63,137,25,16,61,308,26,17,59,169,27*70", 1000);
    gps_feed_get_stats(&f, 1000, &st);
    TEST_ASSERT_EQUAL_UINT8(11, st.sats_tracked);
    TEST_ASSERT_EQUAL_UINT8(27, st.snr_max_dbhz);
}

void test_gsv_cycle_restart_resets_accumulator(void) {
    gps_stats_t st;
    gps_feed_line(&f, "$GPGSV,1,1,08,10,63,137,17,07,61,308,18,05,59,169,19*70", 1000);
    gps_feed_get_stats(&f, 1000, &st);
    TEST_ASSERT_EQUAL_UINT8(3, st.sats_tracked);

    /* A second cycle replaces the first rather than adding to it. */
    gps_feed_line(&f, "$GPGSV,1,1,08,10,63,137,17*70", 2000);
    gps_feed_get_stats(&f, 2000, &st);
    TEST_ASSERT_EQUAL_UINT8(1, st.sats_tracked);
}

void test_gsv_expires_after_ttl(void) {
    gps_feed_line(&f, GPGSV_8, 0);
    gps_stats_t st;
    gps_feed_get_stats(&f, 0, &st);
    TEST_ASSERT_EQUAL_UINT8(8, st.sats_in_view);

    gps_feed_get_stats(&f, GPS_FEED_GSV_TTL_MS + 1, &st);
    TEST_ASSERT_EQUAL_UINT8(0, st.sats_in_view);
    TEST_ASSERT_EQUAL_UINT8(0, st.sats_tracked);
    TEST_ASSERT_EQUAL_UINT8(0, st.snr_max_dbhz);
}

void test_gsv_gn_talker_wins_over_per_constellation(void) {
    gps_feed_line(&f, "$GNGSV,1,1,20,10,63,137,17*70", 1000);
    gps_feed_line(&f, GPGSV_8, 1000);
    gps_stats_t st;
    gps_feed_get_stats(&f, 1000, &st);
    /* A combined cycle already covers every constellation, so summing it with
     * the per-constellation slots would double count. */
    TEST_ASSERT_EQUAL_UINT8(20, st.sats_in_view);
    TEST_ASSERT_EQUAL_UINT8(1, st.sats_tracked);
}

void test_gsv_slot_table_overflow_reclaims_oldest(void) {
    /* Six talkers on two signal bands each fill the table exactly. */
    const char* talkers[6] = {"GP", "GL", "GA", "GB", "GQ", "GI"};
    char line[80];
    uint64_t t = 1000;
    for (int sig = 1; sig <= 2; sig++) {
        for (int i = 0; i < 6; i++) {
            snprintf(line, sizeof(line), "$%sGSV,1,1,0%d,10,63,137,17,%d*70", talkers[i], sig, sig);
            gps_feed_line(&f, line, t++);
        }
    }
    gps_stats_t st;
    gps_feed_get_stats(&f, 1100, &st);
    /* Each talker's two bands fold by maximum (in view 1 and 2), then the six
     * talkers sum; one satellite is heard per band, so folding gives one per
     * talker. */
    TEST_ASSERT_EQUAL_UINT8(12, st.sats_in_view);
    TEST_ASSERT_EQUAL_UINT8(6, st.sats_tracked);

    /* One cycle past capacity reclaims the slot expiring soonest. GN is
     * combined, so it wins outright. */
    gps_feed_line(&f, "$GNGSV,1,1,20,10,63,137,17*70", t++);
    gps_feed_get_stats(&f, 1100, &st);
    TEST_ASSERT_EQUAL_UINT8(20, st.sats_in_view);
    TEST_ASSERT_EQUAL_UINT8(1, st.sats_tracked);

    /* A reclaimed slot re-registers cleanly: once every earlier cycle has
     * expired, a fresh GP cycle reports its own total and nothing else. */
    gps_feed_line(&f, "$GPGSV,1,1,09,10,63,137,17*70", 40000);
    gps_feed_get_stats(&f, 40000, &st);
    TEST_ASSERT_EQUAL_UINT8(9, st.sats_in_view);
}

/* NMEA 4.11 multi-band receivers emit one GSV cycle per talker AND signal id,
 * with message numbering restarting per band, and list a band they are not
 * receiving with blank C/N0 values. Keying slots on the talker alone let the
 * quiet band overwrite the loud one, reporting a receiver hearing eight
 * satellites at 45 dB-Hz as one hearing nothing. */
void test_gsv_signal_bands_fold_rather_than_overwrite(void) {
    gps_feed_line(&f, "$GPGSV,2,1,08,10,63,137,42,07,61,308,40,05,59,169,38,30,54,042,45,1*70",
                  1000);
    gps_feed_line(&f, "$GPGSV,2,2,08,13,40,095,33,15,36,275,31,18,28,015,29,20,22,190,27,1*70",
                  1000);
    gps_feed_line(&f, "$GPGSV,1,1,02,10,63,137,,30,54,042,,6*70", 1000);

    gps_stats_t st;
    gps_feed_get_stats(&f, 1000, &st);
    TEST_ASSERT_EQUAL_UINT8(8, st.sats_in_view);
    TEST_ASSERT_EQUAL_UINT8(8, st.sats_tracked);
    TEST_ASSERT_EQUAL_UINT8(45, st.snr_max_dbhz);
}

/* Bands fold by maximum rather than by sum: a satellite received on two bands
 * is listed once per band and is still one satellite. */
void test_gsv_signal_bands_do_not_double_count(void) {
    gps_feed_line(&f, "$GPGSV,1,1,04,10,63,137,42,07,61,308,40,1*70", 1000);
    gps_feed_line(&f, "$GPGSV,1,1,04,10,63,137,30,07,61,308,28,6*70", 1000);

    gps_stats_t st;
    gps_feed_get_stats(&f, 1000, &st);
    TEST_ASSERT_EQUAL_UINT8(4, st.sats_in_view);
    TEST_ASSERT_EQUAL_UINT8(2, st.sats_tracked);
    TEST_ASSERT_EQUAL_UINT8(42, st.snr_max_dbhz);
}

/* A receiver that stops talking has stopped being evidence for anything: the
 * fix it last reported, the position behind it, its UTC time-of-day and its
 * GGA-derived counts all expire. Carrying a node from a place it fixed to a
 * place where it hears nothing is the most common way a fix stops being
 * true, and a latch would report the fix until reboot. */
void test_fix_and_counts_expire_when_the_receiver_goes_silent(void) {
    gps_feed_line(&f, GGA, 1000);
    gps_feed_line(&f, RMC, 1000);
    TEST_ASSERT_TRUE(gps_feed_has_fix(&f, 1000 + GPS_FEED_SILENCE_TTL_MS - 1));

    const uint64_t dead = 1000 + GPS_FEED_SILENCE_TTL_MS;
    TEST_ASSERT_FALSE(gps_feed_has_fix(&f, dead));
    bramble_position_t p;
    TEST_ASSERT_FALSE(gps_feed_get_position(&f, dead, &p));
    uint8_t h, m;
    TEST_ASSERT_FALSE(gps_feed_get_utc_hm(&f, dead, &h, &m));

    gps_stats_t st;
    gps_feed_get_stats(&f, dead, &st);
    TEST_ASSERT_EQUAL_UINT8(0, st.sats_used);
    TEST_ASSERT_EQUAL_UINT8(0, st.fix_quality);
    TEST_ASSERT_EQUAL_UINT8(0, st.sats_in_view);
    TEST_ASSERT_EQUAL_UINT8(0, st.sats_tracked);
}

/* An unpowered or disconnected module that managed one GGA before dying must
 * not keep that satellite count alive: the count is the only evidence a UI
 * has that anything is being heard. */
void test_silent_receiver_reports_no_satellites_used(void) {
    gps_feed_line(&f, "$GPGSV,1,1,04,10,63,137,17*70", 1000);
    gps_feed_line(&f, "$GPGGA,123519,,,,,0,04,,,M,,M,,*5C", 1000);
    gps_stats_t st;
    gps_feed_get_stats(&f, 1000, &st);
    TEST_ASSERT_EQUAL_UINT8(4, st.sats_used);

    gps_feed_get_stats(&f, 3600000, &st);
    TEST_ASSERT_EQUAL_UINT8(0, st.sats_used);
    TEST_ASSERT_EQUAL_UINT8(0, st.sats_in_view);
    TEST_ASSERT_EQUAL_UINT8(0, st.sats_tracked);
}

/* A talking receiver reporting quality 0 is stating it holds no fix. */
void test_no_fix_gga_clears_a_held_fix(void) {
    gps_feed_line(&f, RMC, 1000);
    TEST_ASSERT_TRUE(gps_feed_has_fix(&f, 1000));

    gps_feed_line(&f, "$GPGGA,123520,,,,,0,00,,,M,,M,,*5C", 2000);
    TEST_ASSERT_FALSE(gps_feed_has_fix(&f, 2000));
    uint8_t h, m;
    TEST_ASSERT_FALSE(gps_feed_get_utc_hm(&f, 2000, &h, &m));
}

/* Same for an RMC whose status field reports the fix as invalid. */
void test_invalid_rmc_clears_a_held_fix(void) {
    gps_feed_line(&f, RMC, 1000);
    TEST_ASSERT_TRUE(gps_feed_has_fix(&f, 1000));

    gps_feed_line(&f, "$GPRMC,123520,V,,,,,,,230394,,*00", 2000);
    TEST_ASSERT_FALSE(gps_feed_has_fix(&f, 2000));
}

/* A line that failed to parse says nothing about the fix, so the fix stands.
 * Only the receiver's own no-fix verdict clears it. */
void test_garbled_sentence_leaves_a_held_fix_alone(void) {
    gps_feed_line(&f, RMC, 1000);
    gps_feed_line(&f, "$GPRMC,123520,A,48", 2000);
    gps_feed_line(&f, "$GPGGA,,,,", 2000);
    TEST_ASSERT_TRUE(gps_feed_has_fix(&f, 2000));
}

void test_nmea_age_never_before_any_line(void) {
    gps_stats_t st;
    gps_feed_get_stats(&f, 5000, &st);
    TEST_ASSERT_EQUAL_UINT32(GPS_STATS_NMEA_NEVER, st.nmea_age_s);
}

void test_nmea_age_counts_from_first_line(void) {
    gps_feed_line(&f, "$GPXXX,1,2,3", 1000);
    gps_stats_t st;
    gps_feed_get_stats(&f, 31000, &st);
    TEST_ASSERT_EQUAL_UINT32(30u, st.nmea_age_s);
}

void test_reset_clears_gsv_slots_and_nmea_age(void) {
    gps_feed_line(&f, GPGSV_8, 1000);
    gps_feed_reset(&f);
    gps_stats_t st;
    gps_feed_get_stats(&f, 1000, &st);
    TEST_ASSERT_EQUAL_UINT8(0, st.sats_in_view);
    TEST_ASSERT_EQUAL_UINT8(0, st.sats_tracked);
    TEST_ASSERT_EQUAL_UINT32(GPS_STATS_NMEA_NEVER, st.nmea_age_s);
}

void test_clear_fix_preserves_gsv_slots_and_fix_quality(void) {
    gps_feed_line(&f, GPGSV_8, 1000);
    gps_feed_line(&f, GGA, 1000);
    gps_feed_line(&f, RMC, 1000);
    TEST_ASSERT_TRUE(gps_feed_has_fix(&f, 1000));

    gps_feed_clear_fix(&f);

    gps_stats_t st;
    gps_feed_get_stats(&f, 1000, &st);
    TEST_ASSERT_EQUAL_UINT8(8, st.sats_in_view);
    TEST_ASSERT_EQUAL_UINT8(2, st.sats_tracked);
    TEST_ASSERT_EQUAL_UINT8(21, st.snr_max_dbhz);
    TEST_ASSERT_EQUAL_UINT8(1, st.fix_quality);
}

void test_fix_quality_survives_a_no_fix_gga(void) {
    gps_stats_t st;
    gps_feed_line(&f, "$GPGGA,123519,,,,,0,05,,,M,,M,,*5C", 1000);
    gps_feed_get_stats(&f, 1000, &st);
    TEST_ASSERT_EQUAL_UINT8(0, st.fix_quality);

    gps_feed_line(&f, GGA, 2000);
    gps_feed_get_stats(&f, 2000, &st);
    TEST_ASSERT_EQUAL_UINT8(1, st.fix_quality);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_bytes_assemble_and_fix);
    RUN_TEST(test_dollar_mid_line_restarts);
    RUN_TEST(test_overlong_line_truncates_without_crash);
    RUN_TEST(test_gga_sats_used_without_fix);
    RUN_TEST(test_merge_gga_altitude_onto_rmc_fix);
    RUN_TEST(test_antenna_warning_ttl_expires);
    RUN_TEST(test_utc_latched_from_gga);
    RUN_TEST(test_chip_banner_captured_once);
    RUN_TEST(test_reset_clears_fix_keeps_cb);
    RUN_TEST(test_clear_fix_hides_position_and_utc_but_keeps_stats);
    RUN_TEST(test_gsv_updates_sats_in_view);
    RUN_TEST(test_gsv_multi_constellation_sums);
    RUN_TEST(test_gsv_repeat_within_cycle_does_not_double_count);
    RUN_TEST(test_gsv_tracked_accumulates_across_cycle);
    RUN_TEST(test_gsv_cycle_restart_resets_accumulator);
    RUN_TEST(test_gsv_expires_after_ttl);
    RUN_TEST(test_gsv_gn_talker_wins_over_per_constellation);
    RUN_TEST(test_gsv_slot_table_overflow_reclaims_oldest);
    RUN_TEST(test_gsv_signal_bands_fold_rather_than_overwrite);
    RUN_TEST(test_gsv_signal_bands_do_not_double_count);
    RUN_TEST(test_fix_and_counts_expire_when_the_receiver_goes_silent);
    RUN_TEST(test_silent_receiver_reports_no_satellites_used);
    RUN_TEST(test_no_fix_gga_clears_a_held_fix);
    RUN_TEST(test_invalid_rmc_clears_a_held_fix);
    RUN_TEST(test_garbled_sentence_leaves_a_held_fix_alone);
    RUN_TEST(test_nmea_age_never_before_any_line);
    RUN_TEST(test_nmea_age_counts_from_first_line);
    RUN_TEST(test_reset_clears_gsv_slots_and_nmea_age);
    RUN_TEST(test_clear_fix_preserves_gsv_slots_and_fix_quality);
    RUN_TEST(test_fix_quality_survives_a_no_fix_gga);
    return UNITY_END();
}
