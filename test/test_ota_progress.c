/* test/test_ota_progress.c */
#include "ota_progress.h"
#include "unity.h"

static int s_cb_count;
static ota_progress_snapshot_t s_last;

static void cb(const ota_progress_snapshot_t* snap) {
    s_cb_count++;
    s_last = *snap;
}

void setUp(void) {
    s_cb_count = 0;
    ota_progress_set_callback(NULL);
    ota_progress_report(OTA_PROG_IDLE, 0, 0);
    ota_progress_set_callback(cb);
}
void tearDown(void) {}

static void test_state_change_fires_callback(void) {
    ota_progress_report(OTA_PROG_DOWNLOADING, 0, 1000);
    TEST_ASSERT_EQUAL_INT(1, s_cb_count);
    TEST_ASSERT_EQUAL_INT(OTA_PROG_DOWNLOADING, s_last.state);
}

static void test_download_percent_throttled_to_5_percent_steps(void) {
    ota_progress_report(OTA_PROG_DOWNLOADING, 0, 1000);   /* fires: state change */
    ota_progress_report(OTA_PROG_DOWNLOADING, 20, 1000);  /* 2%: suppressed */
    ota_progress_report(OTA_PROG_DOWNLOADING, 40, 1000);  /* 4%: suppressed */
    ota_progress_report(OTA_PROG_DOWNLOADING, 50, 1000);  /* 5%: fires */
    ota_progress_report(OTA_PROG_DOWNLOADING, 90, 1000);  /* 9%: suppressed */
    ota_progress_report(OTA_PROG_DOWNLOADING, 100, 1000); /* 10%: fires */
    TEST_ASSERT_EQUAL_INT(3, s_cb_count);
    TEST_ASSERT_EQUAL_INT(100, s_last.bytes);
}

static void test_get_returns_latest_even_when_throttled(void) {
    ota_progress_report(OTA_PROG_DOWNLOADING, 0, 1000);
    ota_progress_report(OTA_PROG_DOWNLOADING, 20, 1000);
    ota_progress_snapshot_t snap;
    ota_progress_get(&snap);
    TEST_ASSERT_EQUAL_INT(20, snap.bytes);
}

static void test_unknown_total_reports_zero_percent_and_still_fires_on_state(void) {
    ota_progress_report(OTA_PROG_DOWNLOADING, 500, 0); /* fires: state change */
    ota_progress_report(OTA_PROG_DOWNLOADING, 900, 0); /* percent stuck at 0: suppressed */
    ota_progress_report(OTA_PROG_VERIFYING, 0, 0);     /* fires: state change */
    TEST_ASSERT_EQUAL_INT(2, s_cb_count);
}

static void test_state_str(void) {
    TEST_ASSERT_EQUAL_STRING("downloading", ota_progress_state_str(OTA_PROG_DOWNLOADING));
    TEST_ASSERT_EQUAL_STRING("failed", ota_progress_state_str(OTA_PROG_FAILED));
}

static void test_percent_zero_when_total_not_positive(void) {
    ota_progress_snapshot_t snap = {.state = OTA_PROG_DOWNLOADING, .bytes = 500, .total = 0};
    TEST_ASSERT_EQUAL_INT(0, ota_progress_percent(&snap));
}

static void test_percent_of_half(void) {
    ota_progress_snapshot_t snap = {.state = OTA_PROG_DOWNLOADING, .bytes = 500, .total = 1000};
    TEST_ASSERT_EQUAL_INT(50, ota_progress_percent(&snap));
}

static void test_set_state_preserves_bytes_total_and_fires_on_change(void) {
    ota_progress_report(OTA_PROG_DOWNLOADING, 250, 1000); /* fires: state change */
    s_cb_count = 0;
    ota_progress_set_state(OTA_PROG_VERIFYING); /* fires: state change */
    TEST_ASSERT_EQUAL_INT(1, s_cb_count);
    TEST_ASSERT_EQUAL_INT(OTA_PROG_VERIFYING, s_last.state);
    TEST_ASSERT_EQUAL_INT(250, s_last.bytes);
    TEST_ASSERT_EQUAL_INT(1000, s_last.total);

    ota_progress_snapshot_t snap;
    ota_progress_get(&snap);
    TEST_ASSERT_EQUAL_INT(250, snap.bytes);
    TEST_ASSERT_EQUAL_INT(1000, snap.total);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_state_change_fires_callback);
    RUN_TEST(test_download_percent_throttled_to_5_percent_steps);
    RUN_TEST(test_get_returns_latest_even_when_throttled);
    RUN_TEST(test_unknown_total_reports_zero_percent_and_still_fires_on_state);
    RUN_TEST(test_state_str);
    RUN_TEST(test_percent_zero_when_total_not_positive);
    RUN_TEST(test_percent_of_half);
    RUN_TEST(test_set_state_preserves_bytes_total_and_fires_on_change);
    return UNITY_END();
}
