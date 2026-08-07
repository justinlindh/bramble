#include "unity.h"
#include "nmea_parser.h"
#include <string.h>
#include <math.h>

void setUp(void) {}
void tearDown(void) {}

/* Test NMEA DDMM.MMMM to decimal degrees conversion */
void test_nmea_dm_to_degrees_north(void) {
    float result = nmea_dm_to_degrees("3725.4321", 'N');
    TEST_ASSERT_FLOAT_WITHIN(0.0001, 37.4238683, result);
}

void test_nmea_dm_to_degrees_south(void) {
    float result = nmea_dm_to_degrees("3725.4321", 'S');
    TEST_ASSERT_FLOAT_WITHIN(0.0001, -37.4238683, result);
}

void test_nmea_dm_to_degrees_east(void) {
    float result = nmea_dm_to_degrees("12215.6789", 'E');
    TEST_ASSERT_FLOAT_WITHIN(0.0001, 122.2613150, result);
}

void test_nmea_dm_to_degrees_west(void) {
    float result = nmea_dm_to_degrees("12215.6789", 'W');
    TEST_ASSERT_FLOAT_WITHIN(0.0001, -122.2613150, result);
}

void test_nmea_dm_to_degrees_equator(void) {
    float result = nmea_dm_to_degrees("0000.0000", 'N');
    TEST_ASSERT_FLOAT_WITHIN(0.0001, 0.0, result);
}

void test_nmea_dm_to_degrees_prime_meridian(void) {
    float result = nmea_dm_to_degrees("00000.0000", 'E');
    TEST_ASSERT_FLOAT_WITHIN(0.0001, 0.0, result);
}

/* Test valid RMC sentence parsing */
void test_nmea_parse_rmc_valid(void) {
    char sentence[] = "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A";
    nmea_position_t pos = {0};

    bool result = nmea_parse_rmc(sentence, &pos);

    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_TRUE(pos.valid);

    /* Latitude: 48°07.038' N = 48.1173° */
    TEST_ASSERT_INT32_WITHIN(1000, 481173000, pos.latitude_e7);

    /* Longitude: 11°31.000' E = 11.5166667° */
    TEST_ASSERT_INT32_WITHIN(1000, 115166667, pos.longitude_e7);

    /* Speed: 22.4 knots = ~41.5 km/h */
    TEST_ASSERT_UINT8_WITHIN(1, 41, pos.speed_kmh);

    /* Heading: 84.4° / 2 = 42 */
    TEST_ASSERT_EQUAL_UINT8(42, pos.heading_deg2);
}

/* Test GNRMC (multi-constellation) sentence */
void test_nmea_parse_gnrmc_valid(void) {
    char sentence[] = "$GNRMC,123519,A,3725.123,S,12215.456,W,010.0,180.0,010122,,,A*50";
    nmea_position_t pos = {0};

    bool result = nmea_parse_rmc(sentence, &pos);

    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_TRUE(pos.valid);

    /* Southern hemisphere - should be negative */
    TEST_ASSERT_TRUE(pos.latitude_e7 < 0);

    /* Western hemisphere - should be negative */
    TEST_ASSERT_TRUE(pos.longitude_e7 < 0);
}

/* Test RMC sentence with invalid status */
void test_nmea_parse_rmc_invalid_status(void) {
    char sentence[] = "$GPRMC,123519,V,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A";
    nmea_position_t pos = {0};

    bool result = nmea_parse_rmc(sentence, &pos);

    TEST_ASSERT_FALSE(result);
    TEST_ASSERT_FALSE(pos.valid);
}

/* Test RMC sentence with empty fields */
void test_nmea_parse_rmc_empty_fields(void) {
    char sentence[] = "$GPRMC,123519,A,,,,,,,,,,*00";
    nmea_position_t pos = {0};

    /* Should still parse status but skip empty fields */
    bool result = nmea_parse_rmc(sentence, &pos);

    /* Valid status but no position data */
    TEST_ASSERT_FALSE(result); /* No position data = invalid */
}

/* Test truncated RMC sentence */
void test_nmea_parse_rmc_truncated(void) {
    char sentence[] = "$GPRMC,123519";
    nmea_position_t pos = {0};

    bool result = nmea_parse_rmc(sentence, &pos);

    TEST_ASSERT_FALSE(result);
}

/* Reject malformed coordinate fields that can appear during startup garbage */
void test_nmea_parse_rmc_rejects_short_longitude_field(void) {
    char sentence[] = "$GPRMC,123519,A,3555.930,N,012.000,W,0.0,0.0,010122,,,A*00";
    nmea_position_t pos = {0};

    bool result = nmea_parse_rmc(sentence, &pos);

    TEST_ASSERT_FALSE(result);
    TEST_ASSERT_FALSE(pos.valid);
}

/* Test valid GGA sentence parsing */
void test_nmea_parse_gga_valid(void) {
    char sentence[] = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47";
    nmea_position_t pos = {0};

    bool result = nmea_parse_gga(sentence, &pos);

    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_TRUE(pos.valid);

    /* Latitude: 48°07.038' N = 48.1173° */
    TEST_ASSERT_INT32_WITHIN(1000, 481173000, pos.latitude_e7);

    /* Longitude: 11°31.000' E = 11.5166667° */
    TEST_ASSERT_INT32_WITHIN(1000, 115166667, pos.longitude_e7);

    /* Altitude: 545.4 m */
    TEST_ASSERT_INT16_WITHIN(1, 545, pos.altitude_m);

    /* Accuracy: HDOP 0.9 * 5 = ~4-5m */
    TEST_ASSERT_UINT8_WITHIN(1, 4, pos.accuracy_m);
}

/* Test GNGGA (multi-constellation) sentence */
void test_nmea_parse_gngga_valid(void) {
    char sentence[] = "$GNGGA,123519,3725.123,S,12215.456,W,2,12,1.5,100.0,M,0.0,M,,*00";
    nmea_position_t pos = {0};

    bool result = nmea_parse_gga(sentence, &pos);

    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_TRUE(pos.valid);

    /* Southern hemisphere - should be negative */
    TEST_ASSERT_TRUE(pos.latitude_e7 < 0);

    /* Western hemisphere - should be negative */
    TEST_ASSERT_TRUE(pos.longitude_e7 < 0);

    /* Altitude: 100.0 m */
    TEST_ASSERT_INT16_WITHIN(1, 100, pos.altitude_m);
}

/* Test GGA sentence with no fix */
void test_nmea_parse_gga_no_fix(void) {
    char sentence[] = "$GPGGA,123519,4807.038,N,01131.000,E,0,00,99.9,0.0,M,0.0,M,,*00";
    nmea_position_t pos = {0};

    bool result = nmea_parse_gga(sentence, &pos);

    TEST_ASSERT_FALSE(result);
    TEST_ASSERT_FALSE(pos.valid);
}

/* Test GGA sentence with empty altitude */
void test_nmea_parse_gga_empty_altitude(void) {
    char sentence[] = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,,M,,M,,*00";
    nmea_position_t pos = {0};

    bool result = nmea_parse_gga(sentence, &pos);

    /* Should still parse position even without altitude */
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_TRUE(pos.valid);
}

/* Test extreme coordinates - near dateline */
void test_nmea_parse_near_dateline(void) {
    char sentence[] = "$GPRMC,123519,A,0000.000,N,17959.999,E,0.0,0.0,010122,,,A*00";
    nmea_position_t pos = {0};

    bool result = nmea_parse_rmc(sentence, &pos);

    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_TRUE(pos.valid);

    /* Close to equator */
    TEST_ASSERT_INT32_WITHIN(100000, 0, pos.latitude_e7);

    /* Close to dateline (179.99998°) */
    TEST_ASSERT_INT32_WITHIN(100000, 1799999833, pos.longitude_e7);
}

/* Test high altitude */
void test_nmea_parse_gga_high_altitude(void) {
    char sentence[] = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,8848.0,M,46.9,M,,*00";
    nmea_position_t pos = {0};

    bool result = nmea_parse_gga(sentence, &pos);

    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_INT16_WITHIN(1, 8848, pos.altitude_m); /* Mt. Everest */
}

/* Test GGA sentence captures satellites-used even without a fix */
void test_nmea_parse_gga_sats_used_no_fix(void) {
    char sentence[] = "$GPGGA,123519,4807.038,N,01131.000,E,0,05,99.9,0.0,M,0.0,M,,*00";
    nmea_position_t pos = {0};

    bool result = nmea_parse_gga(sentence, &pos);

    TEST_ASSERT_FALSE(result);
    TEST_ASSERT_EQUAL_UINT8(5, pos.sats_used);
}

/* Test GGA sentence captures satellites-used with a fix */
void test_nmea_parse_gga_sats_used_with_fix(void) {
    char sentence[] = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47";
    nmea_position_t pos = {0};

    bool result = nmea_parse_gga(sentence, &pos);

    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_EQUAL_UINT8(8, pos.sats_used);
}

/* Test valid GSV sentence parsing */
void test_nmea_parse_gsv_valid(void) {
    char sentence[] = "$GPGSV,3,1,11,10,63,137,17,07,61,308,17,05,59,169,18,30,54,042,*7D";
    nmea_gsv_t gsv;

    bool result = nmea_parse_gsv(sentence, &gsv);

    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_EQUAL_UINT8(11, gsv.sats_in_view);
    TEST_ASSERT_EQUAL_STRING("GP", gsv.talker);
}

/* Test GNGSV (multi-constellation) sentence */
void test_nmea_parse_gngsv_valid(void) {
    char sentence[] = "$GNGSV,1,1,04,10,63,137,17,07,61,308,17*70";
    nmea_gsv_t gsv;

    bool result = nmea_parse_gsv(sentence, &gsv);

    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_EQUAL_UINT8(4, gsv.sats_in_view);
    TEST_ASSERT_EQUAL_STRING("GN", gsv.talker);
}

/* Test truncated GSV sentence is rejected */
void test_nmea_parse_gsv_truncated(void) {
    char sentence[] = "$GPGSV,3,1";
    nmea_gsv_t gsv;

    bool result = nmea_parse_gsv(sentence, &gsv);

    TEST_ASSERT_FALSE(result);
}

/* GLONASS is a talker the old whitelist dropped outright, which undercounted
 * every multi-constellation receiver. */
void test_nmea_parse_gsv_glonass_talker_accepted(void) {
    char sentence[] = "$GLGSV,2,1,07,65,45,120,33,66,20,300,28,72,10,050,,73,05,200,*60";
    nmea_gsv_t gsv;

    TEST_ASSERT_TRUE(nmea_parse_gsv(sentence, &gsv));
    TEST_ASSERT_EQUAL_STRING("GL", gsv.talker);
    TEST_ASSERT_EQUAL_UINT8(7, gsv.sats_in_view);
    TEST_ASSERT_EQUAL_UINT8(2, gsv.tracked);
    TEST_ASSERT_EQUAL_UINT8(33, gsv.snr_max);
}

void test_nmea_parse_gsv_galileo_and_beidou_talkers_accepted(void) {
    char galileo[] = "$GAGSV,1,1,03,01,40,100,30,02,30,200,25,03,20,300,*70";
    char beidou[] = "$GBGSV,1,1,05,11,50,110,44,12,35,210,,13,25,310,*70";
    nmea_gsv_t gsv;

    TEST_ASSERT_TRUE(nmea_parse_gsv(galileo, &gsv));
    TEST_ASSERT_EQUAL_STRING("GA", gsv.talker);
    TEST_ASSERT_EQUAL_UINT8(3, gsv.sats_in_view);

    TEST_ASSERT_TRUE(nmea_parse_gsv(beidou, &gsv));
    TEST_ASSERT_EQUAL_STRING("GB", gsv.talker);
    TEST_ASSERT_EQUAL_UINT8(5, gsv.sats_in_view);
    TEST_ASSERT_EQUAL_UINT8(44, gsv.snr_max);
}

/* A talker-agnostic gate must still reject sentences that are not GSV. */
void test_nmea_parse_gsv_unknown_sentence_rejected(void) {
    char gga[] = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47";
    char gsa[] = "$GPGSA,A,3,04,05,,09,12,,,24,,,,,2.5,1.3,2.1*39";
    nmea_gsv_t gsv;

    TEST_ASSERT_FALSE(nmea_parse_gsv(gga, &gsv));
    TEST_ASSERT_FALSE(nmea_parse_gsv(gsa, &gsv));
}

/* A blank or zero C/N0 is a satellite the almanac predicts and the receiver
 * is not hearing, which is the distinction the tracked count exists for. */
void test_nmea_parse_gsv_counts_tracked_and_max_snr(void) {
    char sentence[] = "$GPGSV,1,1,04,10,63,137,17,07,61,308,00,05,59,169,,30,54,042,42*70";
    nmea_gsv_t gsv;

    TEST_ASSERT_TRUE(nmea_parse_gsv(sentence, &gsv));
    TEST_ASSERT_EQUAL_UINT8(2, gsv.tracked);
    TEST_ASSERT_EQUAL_UINT8(42, gsv.snr_max);
    TEST_ASSERT_EQUAL_UINT8(4, gsv.sats_in_view);
}

void test_nmea_parse_gsv_blank_snr_is_not_tracked(void) {
    char sentence[] = "$GPGSV,1,1,04,10,63,137,,07,61,308,,05,59,169,,30,54,042,*70";
    nmea_gsv_t gsv;

    TEST_ASSERT_TRUE(nmea_parse_gsv(sentence, &gsv));
    TEST_ASSERT_EQUAL_UINT8(0, gsv.tracked);
    TEST_ASSERT_EQUAL_UINT8(0, gsv.snr_max);
    TEST_ASSERT_EQUAL_UINT8(4, gsv.sats_in_view);
}

void test_nmea_parse_gsv_msg_num_and_total_parsed(void) {
    char sentence[] = "$GPGSV,3,2,11,10,63,137,17,07,61,308,17,05,59,169,18,30,54,042,*7D";
    nmea_gsv_t gsv;

    TEST_ASSERT_TRUE(nmea_parse_gsv(sentence, &gsv));
    TEST_ASSERT_EQUAL_UINT8(3, gsv.total_msgs);
    TEST_ASSERT_EQUAL_UINT8(2, gsv.msg_num);
}

/* NMEA 4.11 appends a signal-id field after the last satellite group; it must
 * not be read as a fifth satellite's carrier-to-noise ratio. */
void test_nmea_parse_gsv_tolerates_trailing_signal_id(void) {
    char sentence[] = "$GPGSV,1,1,03,10,63,137,17,07,61,308,18,05,59,169,19,1*70";
    nmea_gsv_t gsv;

    TEST_ASSERT_TRUE(nmea_parse_gsv(sentence, &gsv));
    TEST_ASSERT_EQUAL_UINT8(3, gsv.tracked);
    TEST_ASSERT_EQUAL_UINT8(19, gsv.snr_max);
}

void test_nmea_parse_gsv_snr_clamped_to_99(void) {
    char sentence[] = "$GPGSV,1,1,01,10,63,137,255*70";
    nmea_gsv_t gsv;

    TEST_ASSERT_TRUE(nmea_parse_gsv(sentence, &gsv));
    TEST_ASSERT_EQUAL_UINT8(99, gsv.snr_max);
    TEST_ASSERT_EQUAL_UINT8(1, gsv.tracked);
}

/* Fix quality is the receiver's own verdict and is captured whether or not
 * the sentence survives the fix gate. */
void test_nmea_parse_gga_fix_quality_captured_without_fix(void) {
    char nofix[] = "$GPGGA,123519,,,,,0,05,,,M,,M,,*5C";
    nmea_position_t pos = {0};
    pos.fix_quality = 7;

    TEST_ASSERT_FALSE(nmea_parse_gga(nofix, &pos));
    TEST_ASSERT_EQUAL_UINT8(0, pos.fix_quality);

    char dgps[] = "$GPGGA,123519,4807.038,N,01131.000,E,2,08,0.9,545.4,M,46.9,M,,*47";
    TEST_ASSERT_TRUE(nmea_parse_gga(dgps, &pos));
    TEST_ASSERT_EQUAL_UINT8(2, pos.fix_quality);
}

void test_nmea_parse_gga_fix_quality_empty_field(void) {
    char sentence[] = "$GPGGA,123519,4807.038,N,01131.000,E,,08,0.9,545.4,M,46.9,M,,*47";
    nmea_position_t pos = {0};
    pos.fix_quality = 5;

    TEST_ASSERT_FALSE(nmea_parse_gga(sentence, &pos));
    TEST_ASSERT_EQUAL_UINT8(0, pos.fix_quality);
}

/* Test antenna-open detection */
void test_nmea_is_antenna_open_detects_warning(void) {
    TEST_ASSERT_TRUE(nmea_is_antenna_open("$GPTXT,01,01,02,ANTENNA OPEN*35"));
    TEST_ASSERT_TRUE(nmea_is_antenna_open("$GNTXT,01,01,02,ANTENNA OPEN*35"));
}

/* Test antenna-open detection ignores unrelated TXT and other sentences */
void test_nmea_is_antenna_open_ignores_other_text(void) {
    TEST_ASSERT_FALSE(nmea_is_antenna_open("$GPTXT,01,01,02,ANTENNA OK*3B"));
    TEST_ASSERT_FALSE(nmea_is_antenna_open("$GPRMC,123519,A,4807.038,N,01131.000,E,,*6A"));
    TEST_ASSERT_FALSE(nmea_is_antenna_open(NULL));
}

/* Test high speed */
void test_nmea_parse_rmc_high_speed(void) {
    char sentence[] = "$GPRMC,123519,A,4807.038,N,01131.000,E,100.0,270.0,010122,,,A*00";
    nmea_position_t pos = {0};

    bool result = nmea_parse_rmc(sentence, &pos);

    TEST_ASSERT_TRUE(result);
    /* 100 knots = ~185 km/h */
    TEST_ASSERT_UINT8_WITHIN(2, 185, pos.speed_kmh);

    /* 270° / 2 = 135 */
    TEST_ASSERT_EQUAL_UINT8(135, pos.heading_deg2);
}

/* Test very high speed (clamped to 255) */
void test_nmea_parse_rmc_speed_clamp(void) {
    char sentence[] = "$GPRMC,123519,A,4807.038,N,01131.000,E,200.0,0.0,010122,,,A*00";
    nmea_position_t pos = {0};

    bool result = nmea_parse_rmc(sentence, &pos);

    TEST_ASSERT_TRUE(result);
    /* 200 knots = ~370 km/h, should clamp to 255 */
    TEST_ASSERT_EQUAL_UINT8(255, pos.speed_kmh);
}

/* An RMC that omits optional speed and track (empty fields 7 and 8) must not
 * let those empties collapse and shift later fields into their slots: speed
 * and heading should read as absent (0), not garbage derived from the date
 * (230394) or magnetic variation (003.1). */
void test_nmea_parse_rmc_empty_speed_track_no_shift(void) {
    char sentence[] = "$GPRMC,123519,A,4807.038,N,01131.000,E,,,230394,003.1,W*6A";
    nmea_position_t pos = {0};

    bool result = nmea_parse_rmc(sentence, &pos);

    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_TRUE(pos.valid);
    /* Position still parses correctly despite the empty speed/track. */
    TEST_ASSERT_INT32_WITHIN(1000, 481173000, pos.latitude_e7);
    TEST_ASSERT_INT32_WITHIN(1000, 115166667, pos.longitude_e7);
    /* Empty -> absent, NOT shifted-in from the date/mag-var fields. */
    TEST_ASSERT_EQUAL_UINT8(0, pos.speed_kmh);
    TEST_ASSERT_EQUAL_UINT8(0, pos.heading_deg2);
}

/* A GGA with an empty satellites-used field (7) must not read the HDOP field
 * as the satellite count. With correct field alignment sats_used is 0 and the
 * later altitude field still lands where it belongs. */
void test_nmea_parse_gga_empty_sats_no_shift(void) {
    char sentence[] = "$GPGGA,123519,4807.038,N,01131.000,E,1,,2.5,545.4,M,46.9,M,,*47";
    nmea_position_t pos = {0};

    bool result = nmea_parse_gga(sentence, &pos);

    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_TRUE(pos.valid);
    /* Empty sats field -> 0, not atoi("2.5") == 2 from a collapsed shift. */
    TEST_ASSERT_EQUAL_UINT8(0, pos.sats_used);
    /* Altitude (field 9) still parses -> proves no left-shift occurred. */
    TEST_ASSERT_INT16_WITHIN(1, 545, pos.altitude_m);
}

int main(void) {
    UNITY_BEGIN();

    /* NMEA conversion tests */
    RUN_TEST(test_nmea_dm_to_degrees_north);
    RUN_TEST(test_nmea_dm_to_degrees_south);
    RUN_TEST(test_nmea_dm_to_degrees_east);
    RUN_TEST(test_nmea_dm_to_degrees_west);
    RUN_TEST(test_nmea_dm_to_degrees_equator);
    RUN_TEST(test_nmea_dm_to_degrees_prime_meridian);

    /* RMC parsing tests */
    RUN_TEST(test_nmea_parse_rmc_valid);
    RUN_TEST(test_nmea_parse_gnrmc_valid);
    RUN_TEST(test_nmea_parse_rmc_invalid_status);
    RUN_TEST(test_nmea_parse_rmc_empty_fields);
    RUN_TEST(test_nmea_parse_rmc_truncated);
    RUN_TEST(test_nmea_parse_rmc_rejects_short_longitude_field);
    RUN_TEST(test_nmea_parse_rmc_high_speed);
    RUN_TEST(test_nmea_parse_rmc_speed_clamp);
    RUN_TEST(test_nmea_parse_rmc_empty_speed_track_no_shift);

    /* GGA parsing tests */
    RUN_TEST(test_nmea_parse_gga_valid);
    RUN_TEST(test_nmea_parse_gngga_valid);
    RUN_TEST(test_nmea_parse_gga_no_fix);
    RUN_TEST(test_nmea_parse_gga_empty_altitude);
    RUN_TEST(test_nmea_parse_gga_high_altitude);
    RUN_TEST(test_nmea_parse_gga_sats_used_no_fix);
    RUN_TEST(test_nmea_parse_gga_sats_used_with_fix);
    RUN_TEST(test_nmea_parse_gga_empty_sats_no_shift);
    RUN_TEST(test_nmea_parse_gga_fix_quality_captured_without_fix);
    RUN_TEST(test_nmea_parse_gga_fix_quality_empty_field);

    /* GSV parsing tests */
    RUN_TEST(test_nmea_parse_gsv_valid);
    RUN_TEST(test_nmea_parse_gngsv_valid);
    RUN_TEST(test_nmea_parse_gsv_truncated);
    RUN_TEST(test_nmea_parse_gsv_glonass_talker_accepted);
    RUN_TEST(test_nmea_parse_gsv_galileo_and_beidou_talkers_accepted);
    RUN_TEST(test_nmea_parse_gsv_unknown_sentence_rejected);
    RUN_TEST(test_nmea_parse_gsv_counts_tracked_and_max_snr);
    RUN_TEST(test_nmea_parse_gsv_blank_snr_is_not_tracked);
    RUN_TEST(test_nmea_parse_gsv_msg_num_and_total_parsed);
    RUN_TEST(test_nmea_parse_gsv_tolerates_trailing_signal_id);
    RUN_TEST(test_nmea_parse_gsv_snr_clamped_to_99);

    /* TXT / antenna warning tests */
    RUN_TEST(test_nmea_is_antenna_open_detects_warning);
    RUN_TEST(test_nmea_is_antenna_open_ignores_other_text);

    /* Edge case tests */
    RUN_TEST(test_nmea_parse_near_dateline);

    return UNITY_END();
}
