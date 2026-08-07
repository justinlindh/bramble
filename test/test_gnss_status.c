/* GNSS three-way state classifier and formatters.
 *
 * The acceptance criterion for the whole GNSS-visibility change lives here:
 * "nothing reaching the receiver", "satellites heard but no fix" and "fix"
 * must be three distinct states, in the enum, in the badge text and on the
 * wire. A test that lets any two of them collapse defeats the change. */

#include "unity.h"
#include "gnss_status.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* A powered, GNSS-capable receiver that has been fed NMEA well past the
 * warmup grace, with nothing heard. Individual tests override from here. */
static gnss_ui_input_t base(void) {
    gnss_ui_input_t in;
    memset(&in, 0, sizeof(in));
    in.board_has_gnss = true;
    in.powered = true;
    in.nmea_age_s = 120;
    return in;
}

void test_classify_no_signal_when_nothing_tracked(void) {
    gnss_ui_input_t in = base();
    TEST_ASSERT_EQUAL_INT(GNSS_UI_NO_SIGNAL, gnss_ui_classify(&in));
}

void test_classify_acquiring_when_tracked_without_fix(void) {
    gnss_ui_input_t in = base();
    in.sats_in_view = 12;
    in.sats_tracked = 7;
    TEST_ASSERT_EQUAL_INT(GNSS_UI_ACQUIRING, gnss_ui_classify(&in));
}

void test_classify_fix(void) {
    gnss_ui_input_t in = base();
    in.has_fix = true;
    in.sats_used = 6;
    TEST_ASSERT_EQUAL_INT(GNSS_UI_FIX, gnss_ui_classify(&in));
}

/* Receivers list almanac-predicted satellites in GSV with a blank C/N0. A
 * dead antenna therefore reports a healthy in-view count while hearing
 * nothing, which must not read as progress. */
void test_classify_no_signal_when_in_view_but_none_tracked(void) {
    gnss_ui_input_t in = base();
    in.sats_in_view = 12;
    TEST_ASSERT_EQUAL_INT(GNSS_UI_NO_SIGNAL, gnss_ui_classify(&in));
}

void test_classify_acquiring_during_warmup(void) {
    gnss_ui_input_t in = base();
    in.nmea_age_s = 5;
    TEST_ASSERT_EQUAL_INT(GNSS_UI_ACQUIRING, gnss_ui_classify(&in));
}

void test_classify_no_signal_when_module_silent(void) {
    gnss_ui_input_t in = base();
    in.nmea_age_s = GNSS_UI_NMEA_NEVER;
    TEST_ASSERT_EQUAL_INT(GNSS_UI_NO_SIGNAL, gnss_ui_classify(&in));
}

void test_classify_acquiring_when_gsv_disabled_but_gga_reports_sats(void) {
    gnss_ui_input_t in = base();
    in.sats_used = 5;
    TEST_ASSERT_EQUAL_INT(GNSS_UI_ACQUIRING, gnss_ui_classify(&in));
}

void test_classify_absent_without_board_cap(void) {
    gnss_ui_input_t in = base();
    in.board_has_gnss = false;
    TEST_ASSERT_EQUAL_INT(GNSS_UI_ABSENT, gnss_ui_classify(&in));
}

void test_classify_absent_when_powered_off(void) {
    gnss_ui_input_t in = base();
    in.powered = false;
    in.sats_tracked = 9;
    in.has_fix = true;
    TEST_ASSERT_EQUAL_INT(GNSS_UI_ABSENT, gnss_ui_classify(&in));
}

void test_badge_count_three_states_are_distinct(void) {
    char none[4], acq[4], fix[4];

    gnss_ui_input_t in = base();
    gnss_ui_badge_count(&in, none, sizeof(none));
    TEST_ASSERT_EQUAL_STRING("--", none);

    in = base();
    in.sats_in_view = 12;
    in.sats_tracked = 7;
    gnss_ui_badge_count(&in, acq, sizeof(acq));
    TEST_ASSERT_EQUAL_STRING(" 7", acq);

    in = base();
    in.has_fix = true;
    in.sats_used = 6;
    gnss_ui_badge_count(&in, fix, sizeof(fix));
    TEST_ASSERT_EQUAL_STRING(" 6", fix);

    TEST_ASSERT_NOT_EQUAL(0, strcmp(none, acq));
    TEST_ASSERT_NOT_EQUAL(0, strcmp(none, fix));
    TEST_ASSERT_NOT_EQUAL(0, strcmp(acq, fix));
}

void test_badge_count_is_fixed_width(void) {
    char buf[4];

    gnss_ui_input_t in = base();
    TEST_ASSERT_EQUAL_INT(2, gnss_ui_badge_count(&in, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_size_t(2u, strlen(buf));

    in = base();
    in.sats_tracked = 99;
    TEST_ASSERT_EQUAL_INT(2, gnss_ui_badge_count(&in, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("99", buf);

    in = base();
    in.has_fix = true;
    in.sats_used = 99;
    TEST_ASSERT_EQUAL_INT(2, gnss_ui_badge_count(&in, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("99", buf);

    /* Absent renders nothing at all, so the badge can be hidden outright. */
    in = base();
    in.board_has_gnss = false;
    TEST_ASSERT_EQUAL_INT(0, gnss_ui_badge_count(&in, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("", buf);
}

void test_state_wire_tokens(void) {
    TEST_ASSERT_EQUAL_STRING("absent", gnss_ui_state_wire(GNSS_UI_ABSENT));
    TEST_ASSERT_EQUAL_STRING("no_signal", gnss_ui_state_wire(GNSS_UI_NO_SIGNAL));
    TEST_ASSERT_EQUAL_STRING("acquiring", gnss_ui_state_wire(GNSS_UI_ACQUIRING));
    TEST_ASSERT_EQUAL_STRING("fix", gnss_ui_state_wire(GNSS_UI_FIX));

    const char* tokens[4] = {
        gnss_ui_state_wire(GNSS_UI_ABSENT),
        gnss_ui_state_wire(GNSS_UI_NO_SIGNAL),
        gnss_ui_state_wire(GNSS_UI_ACQUIRING),
        gnss_ui_state_wire(GNSS_UI_FIX),
    };
    for (int i = 0; i < 4; i++) {
        for (int j = i + 1; j < 4; j++) {
            TEST_ASSERT_NOT_EQUAL(0, strcmp(tokens[i], tokens[j]));
        }
    }
}

void test_state_labels_are_distinct(void) {
    const char* labels[4] = {
        gnss_ui_state_label(GNSS_UI_ABSENT),
        gnss_ui_state_label(GNSS_UI_NO_SIGNAL),
        gnss_ui_state_label(GNSS_UI_ACQUIRING),
        gnss_ui_state_label(GNSS_UI_FIX),
    };
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_NOT_NULL(labels[i]);
        for (int j = i + 1; j < 4; j++) {
            TEST_ASSERT_NOT_EQUAL(0, strcmp(labels[i], labels[j]));
        }
    }
}

void test_detail_line_no_signal_names_the_gap(void) {
    char buf[48];

    gnss_ui_input_t in = base();
    gnss_ui_detail_line(&in, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("no satellites detected", buf);

    in = base();
    in.sats_in_view = 12;
    gnss_ui_detail_line(&in, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("0 tracked of 12 in view", buf);
}

void test_detail_line_acquiring_includes_snr(void) {
    char buf[48];
    gnss_ui_input_t in = base();
    in.sats_in_view = 12;
    in.sats_tracked = 7;
    in.snr_max_dbhz = 22;
    gnss_ui_detail_line(&in, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("7 tracked of 12 in view, best 22 dBHz", buf);
    TEST_ASSERT_TRUE(strlen(buf) <= 40);
}

void test_detail_line_fix_includes_used(void) {
    char buf[48];
    gnss_ui_input_t in = base();
    in.has_fix = true;
    in.sats_used = 6;
    in.sats_in_view = 12;
    in.snr_max_dbhz = 41;
    gnss_ui_detail_line(&in, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("6 used of 12 in view, best 41 dBHz", buf);
    TEST_ASSERT_TRUE(strlen(buf) <= 40);
}

void test_detail_line_truncates_safely(void) {
    char buf[8];
    memset(buf, 'x', sizeof(buf));
    gnss_ui_input_t in = base();
    in.sats_in_view = 12;
    in.sats_tracked = 7;
    in.snr_max_dbhz = 22;
    int n = gnss_ui_detail_line(&in, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(7, n);
    TEST_ASSERT_EQUAL_size_t(7u, strlen(buf));
    TEST_ASSERT_EQUAL_CHAR('\0', buf[7]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_classify_no_signal_when_nothing_tracked);
    RUN_TEST(test_classify_acquiring_when_tracked_without_fix);
    RUN_TEST(test_classify_fix);
    RUN_TEST(test_classify_no_signal_when_in_view_but_none_tracked);
    RUN_TEST(test_classify_acquiring_during_warmup);
    RUN_TEST(test_classify_no_signal_when_module_silent);
    RUN_TEST(test_classify_acquiring_when_gsv_disabled_but_gga_reports_sats);
    RUN_TEST(test_classify_absent_without_board_cap);
    RUN_TEST(test_classify_absent_when_powered_off);
    RUN_TEST(test_badge_count_three_states_are_distinct);
    RUN_TEST(test_badge_count_is_fixed_width);
    RUN_TEST(test_state_wire_tokens);
    RUN_TEST(test_state_labels_are_distinct);
    RUN_TEST(test_detail_line_no_signal_names_the_gap);
    RUN_TEST(test_detail_line_acquiring_includes_snr);
    RUN_TEST(test_detail_line_fix_includes_used);
    RUN_TEST(test_detail_line_truncates_safely);
    return UNITY_END();
}
