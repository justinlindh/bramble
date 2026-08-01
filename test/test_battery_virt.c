/* setenv/usleep need glibc's non-strict feature set; must be defined before
 * any system header is pulled in (including transitively via unity.h /
 * emu_link.c). */
#define _DEFAULT_SOURCE

/*
 * Whitebox tests for battery_virt (the charging-aware emu-link battery).
 * Plays the broker over a socketpair (same pattern as test_radio_virt.c /
 * test_gps_virt.c) and drives battery_virt through the real battery.h
 * surface (battery_get_status). emu_link.c, battery_virt.c, and
 * battery_helpers.c (battery_virt.c calls battery_infer_charging) are all
 * #included directly into this one translation unit so the test can attach
 * a socketpair via emu_link's internal emu_link_attach() double.
 */
#include "unity.h"

#include "../components/emu_link/emu_link.c"
#include "../components/battery/battery_pct.c"
#include "../components/battery/battery_helpers.c"
#include "../components/battery/battery_virt.c"

#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static int s_broker_fd = -1;

void setUp(void) {
    s_broker_fd = -1;
    /* Reset battery_virt's module state between tests: no first-batt-
     * message-yet is the pre-init default (4000 mV, UNKNOWN). */
    atomic_store(&s_mv, BATTERY_VIRT_DEFAULT_MV);
    atomic_store(&s_charging, BATTERY_CHG_UNKNOWN);
}

void tearDown(void) {
    emu_link_close();
    if (s_broker_fd != -1) {
        close(s_broker_fd);
        s_broker_fd = -1;
    }
    memset(s_handlers, 0, sizeof(s_handlers));
}

/* --- helpers (mirrors test_radio_virt.c / test_gps_virt.c) ------------- */

static void read_line_timeout(int fd, char* out, size_t out_sz, int deadline_ms) {
    size_t len = 0;
    out[0] = '\0';
    struct timeval start;
    gettimeofday(&start, NULL);
    for (;;) {
        struct timeval now;
        gettimeofday(&now, NULL);
        long elapsed_ms = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_usec - start.tv_usec) / 1000;
        if (elapsed_ms >= deadline_ms)
            return;
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv = {0, 20000};
        int r = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (r <= 0)
            continue;
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0)
            return;
        if (c == '\n') {
            out[len] = '\0';
            return;
        }
        if (len + 1 < out_sz)
            out[len++] = c;
    }
}

/* Attaches one socketpair end as the node under test (emu_link's internal
 * double), returns the broker-side fd (stashed for tearDown), swallows the
 * hello line, then battery_init()s the node under test. */
static void attach(const char* node_id) {
    int fds[2];
    TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    s_broker_fd = fds[0];
    TEST_ASSERT_EQUAL_INT(0, emu_link_attach(fds[1], node_id, ""));
    char hello[512];
    read_line_timeout(s_broker_fd, hello, sizeof(hello), 2000);
    battery_init();
}

static void send_line(int fd, cJSON* obj) {
    char* text = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    TEST_ASSERT_NOT_NULL(text);
    TEST_ASSERT_EQUAL_INT(0, write_all(fd, text, strlen(text)));
    TEST_ASSERT_EQUAL_INT(0, write_all(fd, "\n", 1));
    free(text);
}

/* Polls battery_get_status until want_mv is observed or the deadline
 * passes; the batt handler runs asynchronously on emu_link's reader
 * thread. */
static bool wait_for_mv(uint32_t want_mv, int deadline_ms) {
    for (int waited = 0; waited < deadline_ms; waited += 5) {
        battery_status_t st;
        battery_get_status(&st);
        if (st.mv == want_mv)
            return true;
        usleep(5000);
    }
    return false;
}

/* --- tests --------------------------------------------------------------*/

void test_default_before_first_message_is_unknown_and_present(void) {
    attach("test-node");

    battery_status_t st;
    battery_get_status(&st);
    TEST_ASSERT_EQUAL_UINT32(BATTERY_VIRT_DEFAULT_MV, st.mv);
    TEST_ASSERT_EQUAL_INT(BATTERY_CHG_UNKNOWN, st.charging);
    TEST_ASSERT_TRUE(st.present);
}

void test_charging_true_maps_to_yes(void) {
    attach("test-node");

    cJSON* msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "t", "batt");
    cJSON_AddNumberToObject(msg, "mv", 4798);
    cJSON_AddBoolToObject(msg, "charging", true);
    send_line(s_broker_fd, msg);

    TEST_ASSERT_TRUE(wait_for_mv(4798, 2000));
    battery_status_t st;
    battery_get_status(&st);
    TEST_ASSERT_EQUAL_INT(BATTERY_CHG_YES, st.charging);
    TEST_ASSERT_TRUE(st.present);
}

void test_charging_false_maps_to_no(void) {
    attach("test-node");

    cJSON* msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "t", "batt");
    cJSON_AddNumberToObject(msg, "mv", 3800);
    cJSON_AddBoolToObject(msg, "charging", false);
    send_line(s_broker_fd, msg);

    TEST_ASSERT_TRUE(wait_for_mv(3800, 2000));
    battery_status_t st;
    battery_get_status(&st);
    TEST_ASSERT_EQUAL_INT(BATTERY_CHG_NO, st.charging);
}

void test_charging_absent_maps_to_unknown(void) {
    attach("test-node");

    /* First establish a definite YES so the next message's absence of
     * "charging" is provably the thing that resets it, not the untouched
     * initial default. */
    cJSON* first = cJSON_CreateObject();
    cJSON_AddStringToObject(first, "t", "batt");
    cJSON_AddNumberToObject(first, "mv", 4798);
    cJSON_AddBoolToObject(first, "charging", true);
    send_line(s_broker_fd, first);
    TEST_ASSERT_TRUE(wait_for_mv(4798, 2000));

    cJSON* second = cJSON_CreateObject();
    cJSON_AddStringToObject(second, "t", "batt");
    cJSON_AddNumberToObject(second, "mv", 3700);
    send_line(s_broker_fd, second);
    TEST_ASSERT_TRUE(wait_for_mv(3700, 2000));

    battery_status_t st;
    battery_get_status(&st);
    TEST_ASSERT_EQUAL_INT(BATTERY_CHG_UNKNOWN, st.charging);
}

void test_pct_derived_from_averaged_virt_mv(void) {
    attach("test-node");

    cJSON* msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "t", "batt");
    cJSON_AddNumberToObject(msg, "mv", 3900);
    send_line(s_broker_fd, msg);

    TEST_ASSERT_TRUE(wait_for_mv(3900, 2000));
    battery_status_t st;
    battery_get_status(&st);
    TEST_ASSERT_EQUAL_UINT8(battery_mv_to_pct(3900), st.pct);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_default_before_first_message_is_unknown_and_present);
    RUN_TEST(test_charging_true_maps_to_yes);
    RUN_TEST(test_charging_false_maps_to_no);
    RUN_TEST(test_charging_absent_maps_to_unknown);
    RUN_TEST(test_pct_derived_from_averaged_virt_mv);
    return UNITY_END();
}
