/* setenv/usleep need glibc's non-strict feature set; must be defined before
 * any system header is pulled in (including transitively via unity.h). */
#define _DEFAULT_SOURCE

/* Whitebox tests for gps_virt (emulator/DESIGN.md sections 5 and 8). Includes
 * the driver source directly (same convention as test_emu_link.c) alongside
 * the real emu_link.c and the real nmea_parser.c, and attaches a socketpair
 * fd as the broker test double: the driver's nmea handler runs on emu_link's
 * reader thread exactly as it would against a real broker, and gpsgate
 * messages are read back off the broker end. No firmware, no board_config. */
#include "unity.h"

#include "../components/emu_link/emu_link.c"
#include "../components/gps/gps_virt.c"
#include "../components/gps/nmea_parser.c"

#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* A known-good RMC sentence (same one test_gps.c uses): 48 07.038'N /
 * 011 31.000'E => lat 481173000e-7, lon 115166667e-7. */
static const char* RMC = "$GPRMC,123519,A,4807.038,N,01131.000,E,010.0,084.4,230394,003.1,W*6A";

static int s_broker_fd = -1;

void setUp(void) { s_broker_fd = -1; }

void tearDown(void) {
    emu_link_close();
    if (s_broker_fd != -1) {
        close(s_broker_fd);
        s_broker_fd = -1;
    }
    memset(s_handlers, 0, sizeof(s_handlers));
    /* Reset gps_virt module state between tests. */
    pthread_mutex_lock(&s_mu);
    s_cb = NULL;
    s_cb_ctx = NULL;
    gps_feed_reset(&s_feed);
    s_gate_on = false;
    pthread_mutex_unlock(&s_mu);
}

static void read_line_timeout(int fd, char* out, size_t out_sz, int deadline_ms) {
    size_t len = 0;
    out[0] = '\0';
    struct timeval start;
    gettimeofday(&start, NULL);
    for (;;) {
        struct timeval now;
        gettimeofday(&now, NULL);
        long elapsed = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_usec - start.tv_usec) / 1000;
        if (elapsed >= deadline_ms)
            return;
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv = {0, 20000};
        if (select(fd + 1, &rfds, NULL, NULL, &tv) <= 0)
            continue;
        char c;
        if (recv(fd, &c, 1, 0) <= 0)
            return;
        if (c == '\n') {
            out[len] = '\0';
            return;
        }
        if (len + 1 < out_sz)
            out[len++] = c;
    }
}

static bool wait_for_fix(int deadline_ms) {
    for (int waited = 0; waited < deadline_ms; waited += 5) {
        if (gps_has_fix())
            return true;
        usleep(5000);
    }
    return gps_has_fix();
}

static void attach_and_drain_hello(const char* node_id) {
    int fds[2];
    TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    s_broker_fd = fds[0];
    TEST_ASSERT_EQUAL_INT(0, emu_link_attach(fds[1], node_id, ""));
    char hello[256];
    read_line_timeout(s_broker_fd, hello, sizeof(hello), 2000);
}

static void write_nmea(const char* sentence) {
    cJSON* m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "t", "nmea");
    cJSON_AddStringToObject(m, "sentence", sentence);
    char* text = cJSON_PrintUnformatted(m);
    cJSON_Delete(m);
    TEST_ASSERT_NOT_NULL(text);
    write_all(s_broker_fd, text, strlen(text));
    write_all(s_broker_fd, "\n", 1);
    free(text);
}

/* --- gps_init powers the gate on and emits a gpsgate on message --- */
void test_gps_init_emits_gpsgate_on(void) {
    attach_and_drain_hello("pager-gps-1");
    TEST_ASSERT_EQUAL_INT(0, gps_init(NULL, NULL));

    char line[256];
    read_line_timeout(s_broker_fd, line, sizeof(line), 2000);
    cJSON* m = cJSON_Parse(line);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_EQUAL_STRING("gpsgate", cJSON_GetObjectItem(m, "t")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(m, "on")));
    cJSON_Delete(m);
}

/* --- a valid sentence while gated on updates the position + fires callback --- */
static int s_cb_calls = 0;
static bramble_position_t s_cb_last;
static void fix_cb(const bramble_position_t* p, void* ctx) {
    (void)ctx;
    s_cb_calls++;
    s_cb_last = *p;
}

void test_valid_sentence_sets_fix_and_calls_back(void) {
    s_cb_calls = 0;
    attach_and_drain_hello("pager-gps-2");
    TEST_ASSERT_EQUAL_INT(0, gps_init(fix_cb, NULL));
    char gate[256];
    read_line_timeout(s_broker_fd, gate, sizeof(gate), 2000); /* gpsgate on */

    write_nmea(RMC);
    TEST_ASSERT_TRUE_MESSAGE(wait_for_fix(2000), "expected a fix after a valid RMC");

    bramble_position_t out = {0};
    TEST_ASSERT_TRUE(gps_get_position(&out));
    TEST_ASSERT_TRUE(out.valid);
    TEST_ASSERT_INT_WITHIN(10000, 481173000, out.latitude_e7);
    TEST_ASSERT_INT_WITHIN(10000, 115166667, out.longitude_e7);
    TEST_ASSERT_EQUAL_INT(1, s_cb_calls);
    TEST_ASSERT_INT_WITHIN(10000, 481173000, s_cb_last.latitude_e7);
}

/* --- gps_deinit powers the gate off and emits a gpsgate off message --- */
void test_gps_deinit_emits_gpsgate_off(void) {
    attach_and_drain_hello("pager-gps-3");
    TEST_ASSERT_EQUAL_INT(0, gps_init(NULL, NULL));
    char gate[256];
    read_line_timeout(s_broker_fd, gate, sizeof(gate), 2000); /* gpsgate on */

    gps_deinit();
    read_line_timeout(s_broker_fd, gate, sizeof(gate), 2000);
    cJSON* m = cJSON_Parse(gate);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_EQUAL_STRING("gpsgate", cJSON_GetObjectItem(m, "t")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(m, "on")));
    cJSON_Delete(m);
}

/* --- gated OFF: sentences are dropped, mirroring the P-FET (no power, no
 *     sentences). After deinit the nmea handler stays registered but must
 *     ignore everything until gps_init powers the gate back on. --- */
void test_gated_off_drops_nmea(void) {
    attach_and_drain_hello("pager-gps-4");
    TEST_ASSERT_EQUAL_INT(0, gps_init(NULL, NULL));
    char gate[256];
    read_line_timeout(s_broker_fd, gate, sizeof(gate), 2000); /* gpsgate on */

    gps_deinit();                                             /* gate OFF */
    read_line_timeout(s_broker_fd, gate, sizeof(gate), 2000); /* gpsgate off */

    write_nmea(RMC);
    /* Give the reader thread time to (not) process it. */
    usleep(200000);
    TEST_ASSERT_FALSE_MESSAGE(gps_has_fix(), "a gated-off GPS must drop all sentences");
    bramble_position_t out = {0};
    TEST_ASSERT_FALSE(gps_get_position(&out));
}

/* --- re-powering the gate lets sentences flow again --- */
void test_gate_reopen_accepts_nmea(void) {
    attach_and_drain_hello("pager-gps-5");
    TEST_ASSERT_EQUAL_INT(0, gps_init(NULL, NULL));
    char gate[256];
    read_line_timeout(s_broker_fd, gate, sizeof(gate), 2000);
    gps_deinit();
    read_line_timeout(s_broker_fd, gate, sizeof(gate), 2000);

    /* Power back on. */
    TEST_ASSERT_EQUAL_INT(0, gps_init(NULL, NULL));
    read_line_timeout(s_broker_fd, gate, sizeof(gate), 2000); /* gpsgate on */

    write_nmea(RMC);
    TEST_ASSERT_TRUE_MESSAGE(wait_for_fix(2000), "expected a fix once the gate reopened");
}

/* --- the Settings toggle path: gps_set_enabled(false) cuts the gate and emits
 *     gpsgate off; gps_set_enabled(true) re-powers it and emits gpsgate on,
 *     and sentences flow again using the callback from the original init. --- */
void test_gps_set_enabled_toggles_gate(void) {
    s_cb_calls = 0;
    attach_and_drain_hello("pager-gps-6");
    TEST_ASSERT_EQUAL_INT(0, gps_init(fix_cb, NULL));
    char gate[256];
    read_line_timeout(s_broker_fd, gate, sizeof(gate), 2000); /* gpsgate on (init) */

    /* Toggle OFF via the runtime seam. */
    TEST_ASSERT_EQUAL_INT(0, gps_set_enabled(false));
    read_line_timeout(s_broker_fd, gate, sizeof(gate), 2000);
    cJSON* off = cJSON_Parse(gate);
    TEST_ASSERT_NOT_NULL(off);
    TEST_ASSERT_EQUAL_STRING("gpsgate", cJSON_GetObjectItem(off, "t")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(off, "on")));
    cJSON_Delete(off);

    /* Gated off: a sentence is dropped, no fix. */
    write_nmea(RMC);
    usleep(200000);
    TEST_ASSERT_FALSE_MESSAGE(gps_has_fix(), "toggled-off GPS must drop sentences");

    /* Toggle ON again: gpsgate on, and the retained callback resumes fixes. */
    TEST_ASSERT_EQUAL_INT(0, gps_set_enabled(true));
    read_line_timeout(s_broker_fd, gate, sizeof(gate), 2000);
    cJSON* on = cJSON_Parse(gate);
    TEST_ASSERT_NOT_NULL(on);
    TEST_ASSERT_EQUAL_STRING("gpsgate", cJSON_GetObjectItem(on, "t")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(on, "on")));
    cJSON_Delete(on);

    write_nmea(RMC);
    TEST_ASSERT_TRUE_MESSAGE(wait_for_fix(2000), "expected a fix after re-enabling GPS");
    TEST_ASSERT_TRUE(s_cb_calls >= 1);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_gps_init_emits_gpsgate_on);
    RUN_TEST(test_valid_sentence_sets_fix_and_calls_back);
    RUN_TEST(test_gps_deinit_emits_gpsgate_off);
    RUN_TEST(test_gated_off_drops_nmea);
    RUN_TEST(test_gate_reopen_accepts_nmea);
    RUN_TEST(test_gps_set_enabled_toggles_gate);
    return UNITY_END();
}
