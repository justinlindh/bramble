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
    TEST_ASSERT_TRUE(gps_feed_has_fix(&f));
    TEST_ASSERT_EQUAL_INT(1, cb_count);
    TEST_ASSERT_INT32_WITHIN(1000, 481173000, cb_last.latitude_e7);
    TEST_ASSERT_EQUAL_UINT32(5u, cb_last.timestamp); /* now_ms/1000 */
}

void test_dollar_mid_line_restarts(void) {
    const char garbled[] =
        "$GPRMC,123519,A,48$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n";
    gps_feed_bytes(&f, (const uint8_t*)garbled, strlen(garbled), 1000);
    TEST_ASSERT_TRUE(gps_feed_has_fix(&f)); /* the GGA after the restart parses */
    TEST_ASSERT_EQUAL_UINT8(8, f.sats_used);
}

void test_overlong_line_truncates_without_crash(void) {
    uint8_t big[300];
    memset(big, 'A', sizeof(big));
    big[0] = '$';
    gps_feed_bytes(&f, big, sizeof(big), 1000);
    gps_feed_bytes(&f, (const uint8_t*)"\r\n", 2, 1000);
    TEST_ASSERT_FALSE(gps_feed_has_fix(&f));
}

void test_gga_sats_used_without_fix(void) {
    /* fix quality 0: no fix, but sats_used still reported */
    const char nofix[] = "$GPGGA,123519,,,,,0,05,,,M,,M,,*5C";
    gps_feed_line(&f, nofix, 1000);
    TEST_ASSERT_FALSE(gps_feed_has_fix(&f));
    gps_stats_t st;
    gps_feed_get_stats(&f, 1000, &st);
    TEST_ASSERT_EQUAL_UINT8(5, st.sats_used);
}

void test_merge_gga_altitude_onto_rmc_fix(void) {
    gps_feed_line(&f, RMC, 1000);
    gps_feed_line(&f, GGA, 2000);
    bramble_position_t p;
    TEST_ASSERT_TRUE(gps_feed_get_position(&f, &p));
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
    TEST_ASSERT_TRUE(gps_feed_get_utc_hm(&f, &h, &m));
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
    TEST_ASSERT_FALSE(gps_feed_has_fix(&f));
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
    TEST_ASSERT_TRUE(gps_feed_has_fix(&f));
    uint8_t h, m;
    TEST_ASSERT_TRUE(gps_feed_get_utc_hm(&f, &h, &m));
    uint32_t rx_lines_before = f.rx_lines_total;

    gps_feed_clear_fix(&f);

    TEST_ASSERT_FALSE(gps_feed_has_fix(&f));
    bramble_position_t p;
    TEST_ASSERT_FALSE(gps_feed_get_position(&f, &p));
    TEST_ASSERT_FALSE(gps_feed_get_utc_hm(&f, &h, &m));

    /* Sats/antenna stats, rx counters, and the chip banner all survive. */
    gps_stats_t st;
    gps_feed_get_stats(&f, 1000, &st);
    TEST_ASSERT_EQUAL_UINT8(8, st.sats_used);
    TEST_ASSERT_EQUAL_UINT8(11, st.sats_in_view);
    TEST_ASSERT_EQUAL_UINT32(rx_lines_before, f.rx_lines_total);
    TEST_ASSERT_EQUAL_STRING("$PAIR021,AG3335M,V1.0*3A", f.chip_banner);

    /* A fresh valid sentence re-fixes and fires the callback again. */
    int calls_before = cb_count;
    gps_feed_line(&f, RMC, 2000);
    TEST_ASSERT_TRUE(gps_feed_has_fix(&f));
    TEST_ASSERT_EQUAL_INT(calls_before + 1, cb_count);
}

void test_gsv_updates_sats_in_view(void) {
    gps_feed_line(&f, GSV, 1000);
    gps_stats_t st;
    gps_feed_get_stats(&f, 1000, &st);
    TEST_ASSERT_EQUAL_UINT8(11, st.sats_in_view);
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
    return UNITY_END();
}
