#include "unity.h"

#include "bramble_tz.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

#define US_PACIFIC "PST8PDT,M3.2.0,M11.1.0"

static bramble_tz_time_t utc_at(int32_t y, uint8_t mo, uint8_t d, uint8_t h, uint8_t mi) {
    bramble_tz_time_t t = {.year = y, .month = mo, .day = d, .hour = h, .minute = mi};
    return t;
}

/* Assert that spec maps the given UTC instant to the given local wall clock,
 * abbreviation, and standard/daylight status. */
static void expect_local(const char* spec, bramble_tz_time_t utc, bramble_tz_status_t want_status,
                         int32_t y, uint8_t mo, uint8_t d, uint8_t h, uint8_t mi,
                         const char* want_abbr) {
    bramble_tz_time_t got;
    char abbr[BRAMBLE_TZ_ABBREV_MAX];
    bramble_tz_status_t st = bramble_tz_localtime(spec, &utc, &got, abbr, sizeof(abbr));
    TEST_ASSERT_EQUAL_INT(want_status, st);
    TEST_ASSERT_EQUAL_INT32(y, got.year);
    TEST_ASSERT_EQUAL_UINT8(mo, got.month);
    TEST_ASSERT_EQUAL_UINT8(d, got.day);
    TEST_ASSERT_EQUAL_UINT8(h, got.hour);
    TEST_ASSERT_EQUAL_UINT8(mi, got.minute);
    TEST_ASSERT_EQUAL_STRING(want_abbr, abbr);
}

/* ── Spring-forward boundary (US Pacific, 2026-03-08) ──────────────────────
 * Ground truth from the IANA database via Python zoneinfo: 09:59 UTC is
 * 01:59 PST and one minute later, 10:00 UTC, is 03:00 PDT. */

void test_pacific_minute_before_spring_forward_is_standard(void) {
    expect_local(US_PACIFIC, utc_at(2026, 3, 8, 9, 59), BRAMBLE_TZ_STD, 2026, 3, 8, 1, 59, "PST");
}

void test_pacific_spring_forward_skips_to_0300(void) {
    expect_local(US_PACIFIC, utc_at(2026, 3, 8, 10, 0), BRAMBLE_TZ_DST, 2026, 3, 8, 3, 0, "PDT");
}

/* ── Fall-back boundary (US Pacific, 2026-11-01) ───────────────────────────
 * 08:59 UTC is 01:59 PDT; 09:00 UTC repeats the hour as 01:00 PST. */

void test_pacific_minute_before_fall_back_is_daylight(void) {
    expect_local(US_PACIFIC, utc_at(2026, 11, 1, 8, 59), BRAMBLE_TZ_DST, 2026, 11, 1, 1, 59, "PDT");
}

void test_pacific_fall_back_repeats_the_hour_in_standard_time(void) {
    expect_local(US_PACIFIC, utc_at(2026, 11, 1, 9, 0), BRAMBLE_TZ_STD, 2026, 11, 1, 1, 0, "PST");
}

/* ── Ordinary dates well away from a transition ───────────────────────────── */

void test_pacific_midwinter_is_standard(void) {
    expect_local(US_PACIFIC, utc_at(2026, 1, 15, 12, 0), BRAMBLE_TZ_STD, 2026, 1, 15, 4, 0, "PST");
}

void test_pacific_midsummer_is_daylight(void) {
    expect_local(US_PACIFIC, utc_at(2026, 7, 4, 19, 30), BRAMBLE_TZ_DST, 2026, 7, 4, 12, 30, "PDT");
}

/* Converting backwards across midnight must roll the calendar date back, not
 * just wrap the hour. */
void test_pacific_conversion_rolls_back_across_midnight(void) {
    expect_local(US_PACIFIC, utc_at(2026, 7, 4, 3, 15), BRAMBLE_TZ_DST, 2026, 7, 3, 20, 15, "PDT");
}

void test_pacific_conversion_rolls_back_across_new_year(void) {
    expect_local(US_PACIFIC, utc_at(2026, 1, 1, 4, 0), BRAMBLE_TZ_STD, 2025, 12, 31, 20, 0, "PST");
}

/* ── Zones east of Greenwich, where the offset adds rather than subtracts ── */

void test_india_half_hour_offset_forward(void) {
    expect_local("IST-5:30", utc_at(2026, 6, 1, 20, 15), BRAMBLE_TZ_STD, 2026, 6, 2, 1, 45, "IST");
}

void test_central_europe_summer_is_daylight(void) {
    expect_local("CET-1CEST,M3.5.0,M10.5.0/3", utc_at(2026, 7, 4, 12, 0), BRAMBLE_TZ_DST, 2026, 7,
                 4, 14, 0, "CEST");
}

void test_central_europe_winter_is_standard(void) {
    expect_local("CET-1CEST,M3.5.0,M10.5.0/3", utc_at(2026, 1, 4, 12, 0), BRAMBLE_TZ_STD, 2026, 1,
                 4, 13, 0, "CET");
}

/* ── Southern hemisphere: the daylight interval wraps the year boundary ──── */

void test_new_zealand_january_is_daylight(void) {
    expect_local("NZST-12NZDT,M9.5.0,M4.1.0/3", utc_at(2026, 1, 15, 0, 0), BRAMBLE_TZ_DST, 2026, 1,
                 15, 13, 0, "NZDT");
}

void test_new_zealand_july_is_standard(void) {
    expect_local("NZST-12NZDT,M9.5.0,M4.1.0/3", utc_at(2026, 7, 15, 0, 0), BRAMBLE_TZ_STD, 2026, 7,
                 15, 12, 0, "NZST");
}

/* ── Zones with no daylight-saving rule at all ───────────────────────────── */

void test_utc_default_spec_is_identity(void) {
    expect_local(BRAMBLE_TZ_DEFAULT_SPEC, utc_at(2026, 7, 4, 19, 30), BRAMBLE_TZ_STD, 2026, 7, 4,
                 19, 30, "UTC");
}

void test_arizona_stays_standard_across_the_pacific_transition(void) {
    /* Same instant that moves Pacific onto daylight time leaves Arizona,
     * which has no rule, on standard time. */
    expect_local("MST7", utc_at(2026, 3, 8, 10, 0), BRAMBLE_TZ_STD, 2026, 3, 8, 3, 0, "MST");
}

/* ── Unknown or malformed zone ───────────────────────────────────────────── */

void test_unknown_zone_is_rejected_and_writes_nothing(void) {
    bramble_tz_time_t utc = utc_at(2026, 7, 4, 19, 30);
    bramble_tz_time_t out;
    char abbr[BRAMBLE_TZ_ABBREV_MAX];

    memset(&out, 0xA5, sizeof(out));
    bramble_tz_time_t untouched = out;

    TEST_ASSERT_EQUAL_INT(BRAMBLE_TZ_BAD_SPEC,
                          bramble_tz_localtime(NULL, &utc, &out, abbr, sizeof(abbr)));
    TEST_ASSERT_EQUAL_MEMORY(&untouched, &out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("", abbr);

    TEST_ASSERT_EQUAL_INT(BRAMBLE_TZ_BAD_SPEC,
                          bramble_tz_localtime("", &utc, &out, abbr, sizeof(abbr)));
    TEST_ASSERT_EQUAL_MEMORY(&untouched, &out, sizeof(out));
}

void test_malformed_specs_are_rejected(void) {
    /* Name too short, no offset, junk trailing the rules, bad month, bad
     * weekday, and an hour outside the permitted range. */
    TEST_ASSERT_FALSE(bramble_tz_spec_valid("PS8PDT,M3.2.0,M11.1.0"));
    TEST_ASSERT_FALSE(bramble_tz_spec_valid("PST"));
    TEST_ASSERT_FALSE(bramble_tz_spec_valid("PST8PDT,M3.2.0,M11.1.0,extra"));
    TEST_ASSERT_FALSE(bramble_tz_spec_valid("PST8PDT,M13.2.0,M11.1.0"));
    TEST_ASSERT_FALSE(bramble_tz_spec_valid("PST8PDT,M3.2.7,M11.1.0"));
    TEST_ASSERT_FALSE(bramble_tz_spec_valid("PST25PDT,M3.2.0,M11.1.0"));
    TEST_ASSERT_FALSE(bramble_tz_spec_valid("PST8:99PDT,M3.2.0,M11.1.0"));
}

/* A daylight name with no transition rules is ambiguous: POSIX leaves it
 * implementation-defined, so Bramble rejects it rather than guessing US rules
 * and silently producing a wrong clock elsewhere in the world. */
void test_daylight_name_without_rules_is_rejected(void) {
    TEST_ASSERT_FALSE(bramble_tz_spec_valid("PST8PDT"));
}

void test_valid_specs_are_accepted(void) {
    TEST_ASSERT_TRUE(bramble_tz_spec_valid(US_PACIFIC));
    TEST_ASSERT_TRUE(bramble_tz_spec_valid("UTC0"));
    TEST_ASSERT_TRUE(bramble_tz_spec_valid("MST7"));
    TEST_ASSERT_TRUE(bramble_tz_spec_valid("IST-5:30"));
    TEST_ASSERT_TRUE(bramble_tz_spec_valid("PST8PDT,M3.2.0/2:00:00,M11.1.0/2:00:00"));
    TEST_ASSERT_TRUE(bramble_tz_spec_valid("<+0545>-5:45"));
    TEST_ASSERT_TRUE(bramble_tz_spec_valid("EST5EDT,J60,J300"));
    TEST_ASSERT_TRUE(bramble_tz_spec_valid("EST5EDT,59,299"));
}

void test_spec_longer_than_the_storage_limit_is_rejected(void) {
    char long_spec[BRAMBLE_TZ_SPEC_MAX + 16];
    memset(long_spec, 'A', sizeof(long_spec) - 1);
    long_spec[sizeof(long_spec) - 1] = '\0';
    TEST_ASSERT_FALSE(bramble_tz_spec_valid(long_spec));
}

/* ── Invalid UTC input ───────────────────────────────────────────────────── */

void test_invalid_utc_date_is_rejected(void) {
    bramble_tz_time_t out;
    TEST_ASSERT_EQUAL_INT(BRAMBLE_TZ_BAD_TIME,
                          bramble_tz_localtime(US_PACIFIC, NULL, &out, NULL, 0));

    bramble_tz_time_t feb30 = utc_at(2026, 2, 30, 12, 0);
    TEST_ASSERT_EQUAL_INT(BRAMBLE_TZ_BAD_TIME,
                          bramble_tz_localtime(US_PACIFIC, &feb30, &out, NULL, 0));

    bramble_tz_time_t bad_hour = utc_at(2026, 2, 10, 24, 0);
    TEST_ASSERT_EQUAL_INT(BRAMBLE_TZ_BAD_TIME,
                          bramble_tz_localtime(US_PACIFIC, &bad_hour, &out, NULL, 0));

    bramble_tz_time_t bad_month = utc_at(2026, 0, 10, 1, 0);
    TEST_ASSERT_EQUAL_INT(BRAMBLE_TZ_BAD_TIME,
                          bramble_tz_localtime(US_PACIFIC, &bad_month, &out, NULL, 0));
}

/* February 29 must be a valid input in a leap year and must convert. */
void test_leap_day_converts(void) {
    expect_local(US_PACIFIC, utc_at(2028, 2, 29, 12, 0), BRAMBLE_TZ_STD, 2028, 2, 29, 4, 0, "PST");
}

/* ── Transition dates are recomputed per year, not pinned to one year ────── */

void test_spring_forward_tracks_the_second_sunday_each_year(void) {
    /* 2027-03-14 is the second Sunday of March; the day before is standard. */
    expect_local(US_PACIFIC, utc_at(2027, 3, 14, 9, 59), BRAMBLE_TZ_STD, 2027, 3, 14, 1, 59, "PST");
    expect_local(US_PACIFIC, utc_at(2027, 3, 14, 10, 0), BRAMBLE_TZ_DST, 2027, 3, 14, 3, 0, "PDT");
}

/* ── The preset table every picker reads ─────────────────────────────────── */

void test_every_preset_parses(void) {
    const size_t n = bramble_tz_preset_count();
    TEST_ASSERT_GREATER_THAN_UINT32(0, (uint32_t)n);
    for (size_t i = 0; i < n; i++) {
        const bramble_tz_preset_t* p = bramble_tz_preset(i);
        TEST_ASSERT_NOT_NULL(p);
        TEST_ASSERT_NOT_NULL(p->label);
        TEST_ASSERT_NOT_NULL(p->spec);
        TEST_ASSERT_TRUE_MESSAGE(bramble_tz_spec_valid(p->spec), p->label);
        TEST_ASSERT_TRUE(strlen(p->spec) < BRAMBLE_TZ_SPEC_MAX);
    }
    TEST_ASSERT_NULL(bramble_tz_preset(n));
}

void test_default_spec_is_a_preset(void) {
    bool found = false;
    for (size_t i = 0; i < bramble_tz_preset_count(); i++) {
        if (strcmp(bramble_tz_preset(i)->spec, BRAMBLE_TZ_DEFAULT_SPEC) == 0)
            found = true;
    }
    TEST_ASSERT_TRUE(found);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_pacific_minute_before_spring_forward_is_standard);
    RUN_TEST(test_pacific_spring_forward_skips_to_0300);
    RUN_TEST(test_pacific_minute_before_fall_back_is_daylight);
    RUN_TEST(test_pacific_fall_back_repeats_the_hour_in_standard_time);
    RUN_TEST(test_pacific_midwinter_is_standard);
    RUN_TEST(test_pacific_midsummer_is_daylight);
    RUN_TEST(test_pacific_conversion_rolls_back_across_midnight);
    RUN_TEST(test_pacific_conversion_rolls_back_across_new_year);
    RUN_TEST(test_india_half_hour_offset_forward);
    RUN_TEST(test_central_europe_summer_is_daylight);
    RUN_TEST(test_central_europe_winter_is_standard);
    RUN_TEST(test_new_zealand_january_is_daylight);
    RUN_TEST(test_new_zealand_july_is_standard);
    RUN_TEST(test_utc_default_spec_is_identity);
    RUN_TEST(test_arizona_stays_standard_across_the_pacific_transition);
    RUN_TEST(test_unknown_zone_is_rejected_and_writes_nothing);
    RUN_TEST(test_malformed_specs_are_rejected);
    RUN_TEST(test_daylight_name_without_rules_is_rejected);
    RUN_TEST(test_valid_specs_are_accepted);
    RUN_TEST(test_spec_longer_than_the_storage_limit_is_rejected);
    RUN_TEST(test_invalid_utc_date_is_rejected);
    RUN_TEST(test_leap_day_converts);
    RUN_TEST(test_spring_forward_tracks_the_second_sunday_each_year);
    RUN_TEST(test_every_preset_parses);
    RUN_TEST(test_default_spec_is_a_preset);
    return UNITY_END();
}
