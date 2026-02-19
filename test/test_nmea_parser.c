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
    TEST_ASSERT_FALSE(result);  /* No position data = invalid */
}

/* Test truncated RMC sentence */
void test_nmea_parse_rmc_truncated(void) {
    char sentence[] = "$GPRMC,123519";
    nmea_position_t pos = {0};
    
    bool result = nmea_parse_rmc(sentence, &pos);
    
    TEST_ASSERT_FALSE(result);
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
    TEST_ASSERT_INT16_WITHIN(1, 8848, pos.altitude_m);  /* Mt. Everest */
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
    RUN_TEST(test_nmea_parse_rmc_high_speed);
    RUN_TEST(test_nmea_parse_rmc_speed_clamp);
    
    /* GGA parsing tests */
    RUN_TEST(test_nmea_parse_gga_valid);
    RUN_TEST(test_nmea_parse_gngga_valid);
    RUN_TEST(test_nmea_parse_gga_no_fix);
    RUN_TEST(test_nmea_parse_gga_empty_altitude);
    RUN_TEST(test_nmea_parse_gga_high_altitude);
    
    /* Edge case tests */
    RUN_TEST(test_nmea_parse_near_dateline);
    
    return UNITY_END();
}
