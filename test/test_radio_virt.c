/* setenv/usleep need glibc's non-strict feature set; must be defined before
 * any system header is pulled in (including transitively via unity.h /
 * emu_link.c). */
#define _DEFAULT_SOURCE

/*
 * Whitebox tests for radio_virt (emulator/DESIGN.md sections 5 and 8): the
 * virtual radio backend that speaks the emu-link broker protocol. The test
 * plays the broker over a socketpair (same pattern as test_emu_link.c) and
 * drives radio_virt through the real radio.h / radio_internal.h surface.
 *
 * emu_link.c, radio_airtime.c, radio_profiles.c and radio_virt.c are all
 * #included directly into this one translation unit so the test can attach a
 * socketpair via emu_link's internal emu_link_attach() double and reach
 * radio_virt's static base64 helpers.
 */
#include "unity.h"

#include "../components/emu_link/emu_link.c"
#include "../components/radio/radio_airtime.c"
#include "../components/radio/radio_profiles.c"
#include "../components/radio/radio_virt.c"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static int s_broker_fd = -1;

/* --- test-visible radio callback capture ------------------------------- */

static _Atomic int s_rx_calls;
static uint8_t s_rx_data[256];
static uint8_t s_rx_len;
static int16_t s_rx_rssi;
static int8_t s_rx_snr;

static void capture_rx(const uint8_t* data, uint8_t len, const radio_rx_info_t* info) {
    memcpy(s_rx_data, data, len);
    s_rx_len = len;
    s_rx_rssi = info->rssi;
    s_rx_snr = info->snr;
    atomic_fetch_add(&s_rx_calls, 1);
}

static _Atomic int s_tx_done_calls;
static void capture_tx_done(void) { atomic_fetch_add(&s_tx_done_calls, 1); }

void setUp(void) {
    s_broker_fd = -1;
    atomic_store(&s_rx_calls, 0);
    atomic_store(&s_tx_done_calls, 0);
    s_rx_len = 0;
    s_rx_rssi = 0;
    s_rx_snr = 0;
    s_rx_cb = NULL;
    s_tx_done_cb = NULL;
    s_cad_done_cb = NULL;
    /* Reset the CAD-timeout policy statics so the consecutive-timeout counter
     * and reinit flag do not leak across tests (issue #118). */
    s_cad_timeout_policy.consecutive_timeouts = 0;
    atomic_store(&s_needs_reinit, false);
}

void tearDown(void) {
    emu_link_close();
    if (s_broker_fd != -1) {
        close(s_broker_fd);
        s_broker_fd = -1;
    }
    memset(s_handlers, 0, sizeof(s_handlers));
    unsetenv("EMU_BROKER");
}

/* --- helpers (mirrors test_emu_link.c) --------------------------------- */

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

static bool wait_for_calls(_Atomic int* counter, int want, int deadline_ms) {
    for (int waited = 0; waited < deadline_ms; waited += 5) {
        if (atomic_load(counter) >= want)
            return true;
        usleep(5000);
    }
    return atomic_load(counter) >= want;
}

/* Attaches one socketpair end as the node under test (emu_link's internal
 * double), returns the broker-side fd (stashed for tearDown), then swallows
 * the hello line and radio_init()s the node with the given config. */
static void attach_and_init(const char* node_id, const radio_config_t* cfg) {
    int fds[2];
    TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    s_broker_fd = fds[0];
    TEST_ASSERT_EQUAL_INT(0, emu_link_attach(fds[1], node_id, ""));
    char hello[512];
    read_line_timeout(s_broker_fd, hello, sizeof(hello), 2000);
    TEST_ASSERT_EQUAL_INT(0, radio_init(cfg));
}

static void send_line(int fd, cJSON* obj) {
    char* text = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    TEST_ASSERT_NOT_NULL(text);
    TEST_ASSERT_EQUAL_INT(0, write_all(fd, text, strlen(text)));
    TEST_ASSERT_EQUAL_INT(0, write_all(fd, "\n", 1));
    free(text);
}

static radio_config_t default_cfg(void) {
    radio_config_t cfg;
    radio_get_profile_config(RADIO_PROFILE_LONG_RANGE, &cfg);
    return cfg;
}

/* ---------------------------------------------------------------------- */
/*  TX: JSON correctness, in-flight state, airtime agreement, tx-done cb   */
/* ---------------------------------------------------------------------- */

typedef struct {
    const uint8_t* data;
    uint8_t len;
    int rc;
} tx_arg_t;

static void* tx_worker(void* arg) {
    tx_arg_t* a = (tx_arg_t*)arg;
    a->rc = radio_transmit_raw(a->data, a->len);
    return NULL;
}

void test_tx_emits_correct_json_and_completes(void) {
    /* SF10/125k/CR2 so the airtime lines up with a known reference vector. */
    radio_config_t cfg = default_cfg();
    cfg.sf = 10;
    cfg.bw_hz = 125000;
    cfg.coding_rate = 2;
    cfg.tx_power = 17;
    attach_and_init("pager-tx", &cfg);
    radio_set_tx_done_callback(capture_tx_done);

    const uint8_t payload[22] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
                                 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0xff};
    tx_arg_t a = {payload, sizeof(payload), -999};
    pthread_t worker;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&worker, NULL, tx_worker, &a));

    /* Broker reads the tx message. */
    char line[1024];
    read_line_timeout(s_broker_fd, line, sizeof(line), 2000);
    cJSON* tx = cJSON_Parse(line);
    TEST_ASSERT_NOT_NULL(tx);
    TEST_ASSERT_EQUAL_STRING("tx", cJSON_GetObjectItem(tx, "t")->valuestring);

    /* Modulation params come straight from the active radio_config_t. */
    TEST_ASSERT_EQUAL_INT(10, cJSON_GetObjectItem(tx, "sf")->valueint);
    TEST_ASSERT_EQUAL_INT(125000, cJSON_GetObjectItem(tx, "bw")->valueint);
    TEST_ASSERT_EQUAL_INT(2, cJSON_GetObjectItem(tx, "cr")->valueint);
    TEST_ASSERT_EQUAL_INT(17, cJSON_GetObjectItem(tx, "power")->valueint);
    TEST_ASSERT_FLOAT_WITHIN(0.01, 915.0, cJSON_GetObjectItem(tx, "freq")->valuedouble);

    /* Payload round-trips through base64. */
    const char* b64 = cJSON_GetObjectItem(tx, "payload")->valuestring;
    uint8_t decoded[256];
    size_t dlen = b64_decode(b64, decoded, sizeof(decoded));
    TEST_ASSERT_EQUAL_UINT(sizeof(payload), dlen);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, decoded, sizeof(payload));

    /* Radio is in TX while the frame is in flight (worker still blocked). */
    TEST_ASSERT_EQUAL_INT(RADIO_STATE_TX, radio_get_state());

    /* Airtime agreement: the params the node emitted, priced by the same
     * radio_airtime function the broker uses, match the reference vector for
     * a 22-byte frame at SF10/125k/CR2 (Semtech AN1200.13 ~= 444 ms). */
    uint32_t sf = cJSON_GetObjectItem(tx, "sf")->valueint;
    uint32_t bw = cJSON_GetObjectItem(tx, "bw")->valueint;
    uint32_t cr = cJSON_GetObjectItem(tx, "cr")->valueint;
    uint32_t toa_ms = bramble_calculate_airtime_us((uint16_t)dlen, sf, bw, cr) / 1000u;
    TEST_ASSERT_UINT32_WITHIN(1, 444, toa_ms);
    cJSON_Delete(tx);

    /* Broker prices time-on-air; node's tx-done fires and it returns to RX. */
    cJSON* done = cJSON_CreateObject();
    cJSON_AddStringToObject(done, "t", "txdone");
    cJSON_AddNumberToObject(done, "toa_ms", toa_ms);
    send_line(s_broker_fd, done);

    pthread_join(worker, NULL);
    TEST_ASSERT_EQUAL_INT(0, a.rc);
    TEST_ASSERT_TRUE(wait_for_calls(&s_tx_done_calls, 1, 2000));
    TEST_ASSERT_EQUAL_INT(RADIO_STATE_RX, radio_get_state());
}

void test_tx_broker_down_fails_and_recovers(void) {
    /* With no broker, the tx send fails immediately: radio_transmit_raw must
     * report the radio error and return the state machine to RX rather than
     * blocking on a txdone that can never arrive. */
    radio_config_t cfg = default_cfg();
    attach_and_init("pager-txto", &cfg);

    emu_link_close();
    close(s_broker_fd);
    s_broker_fd = -1;
    usleep(20000);

    const uint8_t payload[4] = {1, 2, 3, 4};
    int rc = radio_transmit_raw(payload, sizeof(payload));
    TEST_ASSERT_EQUAL_INT(-1, rc);
    TEST_ASSERT_EQUAL_INT(RADIO_STATE_RX, radio_get_state());
}

/* ---------------------------------------------------------------------- */
/*  RX: broker rx dispatches to the registered callback with RSSI/SNR      */
/* ---------------------------------------------------------------------- */

void test_rx_dispatches_to_callback_with_rssi_snr(void) {
    radio_config_t cfg = default_cfg();
    attach_and_init("pager-rx", &cfg);
    radio_set_rx_callback(capture_rx);

    const uint8_t frame[5] = {'h', 'e', 'l', 'l', 'o'};
    char b64[16];
    b64_encode(frame, sizeof(frame), b64, sizeof(b64));

    cJSON* rx = cJSON_CreateObject();
    cJSON_AddStringToObject(rx, "t", "rx");
    cJSON_AddStringToObject(rx, "payload", b64);
    cJSON_AddNumberToObject(rx, "rssi", -87);
    cJSON_AddNumberToObject(rx, "snr", -3);
    cJSON_AddNumberToObject(rx, "freq", 915.0);
    send_line(s_broker_fd, rx);

    TEST_ASSERT_TRUE(wait_for_calls(&s_rx_calls, 1, 2000));
    TEST_ASSERT_EQUAL_UINT8(sizeof(frame), s_rx_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(frame, s_rx_data, sizeof(frame));
    TEST_ASSERT_EQUAL_INT16(-87, s_rx_rssi);
    TEST_ASSERT_EQUAL_INT8(-3, s_rx_snr);
}

void test_rx_without_callback_is_dropped_not_crash(void) {
    radio_config_t cfg = default_cfg();
    attach_and_init("pager-rxnull", &cfg);
    /* no rx callback registered */

    const uint8_t frame[3] = {1, 2, 3};
    char b64[16];
    b64_encode(frame, sizeof(frame), b64, sizeof(b64));
    cJSON* rx = cJSON_CreateObject();
    cJSON_AddStringToObject(rx, "t", "rx");
    cJSON_AddStringToObject(rx, "payload", b64);
    send_line(s_broker_fd, rx);

    usleep(50000);
    TEST_ASSERT_EQUAL_INT(0, atomic_load(&s_rx_calls));
}

/* ---------------------------------------------------------------------- */
/*  CAD: cad out, cadres in, radio_cad_check returns the broker's verdict  */
/* ---------------------------------------------------------------------- */

typedef struct {
    int fd;
    bool reply_busy;
} cad_broker_t;

static void* cad_broker_thread(void* arg) {
    cad_broker_t* b = (cad_broker_t*)arg;
    char line[512];
    read_line_timeout(b->fd, line, sizeof(line), 2000);
    cJSON* cad = cJSON_Parse(line);
    if (!cad)
        return NULL;
    /* Verify it is a cad request before answering. */
    const cJSON* t = cJSON_GetObjectItem(cad, "t");
    bool is_cad = cJSON_IsString(t) && strcmp(t->valuestring, "cad") == 0;
    cJSON_Delete(cad);
    if (!is_cad)
        return NULL;
    cJSON* res = cJSON_CreateObject();
    cJSON_AddStringToObject(res, "t", "cadres");
    cJSON_AddBoolToObject(res, "busy", b->reply_busy);
    char* text = cJSON_PrintUnformatted(res);
    cJSON_Delete(res);
    write_all(b->fd, text, strlen(text));
    write_all(b->fd, "\n", 1);
    free(text);
    return NULL;
}

static bool run_cad_check(bool broker_busy) {
    cad_broker_t b = {s_broker_fd, broker_busy};
    pthread_t th;
    pthread_create(&th, NULL, cad_broker_thread, &b);
    bool busy = radio_cad_check();
    pthread_join(th, NULL);
    return busy;
}

void test_cad_check_reports_busy_channel(void) {
    radio_config_t cfg = default_cfg();
    attach_and_init("pager-cad1", &cfg);
    TEST_ASSERT_TRUE(run_cad_check(true));
    /* radio_cad_check restores RX when done. */
    TEST_ASSERT_EQUAL_INT(RADIO_STATE_RX, radio_get_state());
}

void test_cad_check_reports_clear_channel(void) {
    radio_config_t cfg = default_cfg();
    attach_and_init("pager-cad2", &cfg);
    TEST_ASSERT_FALSE(run_cad_check(false));
    TEST_ASSERT_EQUAL_INT(RADIO_STATE_RX, radio_get_state());
}

void test_cad_check_timeout_reports_clear(void) {
    /* No broker answer within the CAD window: fail safe to "clear" (matches
     * radio_esp.c, which transmits anyway rather than starving). */
    radio_config_t cfg = default_cfg();
    attach_and_init("pager-cad3", &cfg);
    /* Drain the cad message but never answer. */
    bool busy = radio_cad_check();
    char sink[512];
    read_line_timeout(s_broker_fd, sink, sizeof(sink), 500); /* the cad line */
    TEST_ASSERT_FALSE(busy);
    TEST_ASSERT_EQUAL_INT(RADIO_STATE_RX, radio_get_state());
}

/* A fast config so each forced CAD timeout floors at BRAMBLE_CAD_TIMEOUT_MIN_MS
 * (50 ms) instead of a long-range SF12 budget. */
static radio_config_t fast_cad_cfg(void) {
    radio_config_t cfg = default_cfg();
    cfg.sf = 7;
    cfg.bw_hz = 500000;
    return cfg;
}

/* Run one radio_cad_check with no broker answer, draining the cad line so the
 * socketpair buffer never backs up. */
static bool cad_check_unanswered(void) {
    bool busy = radio_cad_check();
    char sink[512];
    read_line_timeout(s_broker_fd, sink, sizeof(sink), 500); /* the cad line */
    return busy;
}

/* Fail-open/closed policy (issue #118): the first
 * BRAMBLE_CAD_TIMEOUT_REINIT_THRESHOLD-1 consecutive timeouts fail open and
 * request no reinit. */
void test_cad_timeout_fails_open_before_threshold(void) {
    radio_config_t cfg = fast_cad_cfg();
    attach_and_init("pager-cad-open", &cfg);
    for (unsigned i = 0; i < BRAMBLE_CAD_TIMEOUT_REINIT_THRESHOLD - 1; i++) {
        TEST_ASSERT_FALSE(cad_check_unanswered()); /* fail open: channel "clear" */
    }
    TEST_ASSERT_FALSE(radio_check_and_clear_reinit()); /* no reinit requested yet */
}

/* On the threshold-th consecutive timeout the policy fails closed (reports
 * busy) and flags the radio for reinit. */
void test_cad_timeout_fails_closed_and_requests_reinit_at_threshold(void) {
    radio_config_t cfg = fast_cad_cfg();
    attach_and_init("pager-cad-closed", &cfg);
    for (unsigned i = 0; i < BRAMBLE_CAD_TIMEOUT_REINIT_THRESHOLD - 1; i++) {
        TEST_ASSERT_FALSE(cad_check_unanswered());
    }
    TEST_ASSERT_TRUE(cad_check_unanswered()); /* fail closed: report busy */
    /* The reinit flag is raised once, then cleared by the check. */
    TEST_ASSERT_TRUE(radio_check_and_clear_reinit());
    TEST_ASSERT_FALSE(radio_check_and_clear_reinit());
}

/* A CAD that actually completes resets the streak, so a later lone timeout
 * fails open again rather than counting toward a stale run. */
void test_cad_success_resets_timeout_streak(void) {
    radio_config_t cfg = fast_cad_cfg();
    attach_and_init("pager-cad-reset", &cfg);
    for (unsigned i = 0; i < BRAMBLE_CAD_TIMEOUT_REINIT_THRESHOLD - 1; i++) {
        TEST_ASSERT_FALSE(cad_check_unanswered());
    }
    /* An answered CAD clears the run. */
    TEST_ASSERT_FALSE(run_cad_check(false));
    /* The next timeout is therefore a fresh first timeout: fail open, no reinit. */
    TEST_ASSERT_FALSE(cad_check_unanswered());
    TEST_ASSERT_FALSE(radio_check_and_clear_reinit());
}

/* ---------------------------------------------------------------------- */
/*  State machine: init/start_rx/sleep transitions                         */
/* ---------------------------------------------------------------------- */

void test_init_ends_in_rx(void) {
    radio_config_t cfg = default_cfg();
    attach_and_init("pager-st1", &cfg);
    TEST_ASSERT_EQUAL_INT(RADIO_STATE_RX, radio_get_state());
}

void test_sleep_then_start_rx(void) {
    radio_config_t cfg = default_cfg();
    attach_and_init("pager-st2", &cfg);
    radio_sleep();
    TEST_ASSERT_EQUAL_INT(RADIO_STATE_SLEEP, radio_get_state());
    radio_start_rx();
    TEST_ASSERT_EQUAL_INT(RADIO_STATE_RX, radio_get_state());
}

void test_get_config_round_trips(void) {
    radio_config_t cfg = default_cfg();
    cfg.tx_power = 13;
    attach_and_init("pager-st3", &cfg);
    radio_config_t got;
    radio_get_config(&got);
    TEST_ASSERT_EQUAL_INT(cfg.sf, got.sf);
    TEST_ASSERT_EQUAL_UINT32(cfg.bw_hz, got.bw_hz);
    TEST_ASSERT_EQUAL_INT(cfg.coding_rate, got.coding_rate);
    TEST_ASSERT_EQUAL_INT(13, got.tx_power);
    radio_set_tx_power(20);
    radio_get_config(&got);
    TEST_ASSERT_EQUAL_INT(20, got.tx_power);
}

/* ---------------------------------------------------------------------- */
/*  Profile table parity with the SX1262 authoritative values              */
/* ---------------------------------------------------------------------- */

void test_profile_long_range_values(void) {
    radio_config_t c;
    radio_get_profile_config(RADIO_PROFILE_LONG_RANGE, &c);
    TEST_ASSERT_FLOAT_WITHIN(0.01, 915.0, c.frequency_mhz);
    TEST_ASSERT_EQUAL_INT(10, c.sf);
    TEST_ASSERT_EQUAL_UINT32(125000, c.bw_hz);
    TEST_ASSERT_EQUAL_INT(1, c.coding_rate);
    TEST_ASSERT_EQUAL_INT(22, c.tx_power);
    TEST_ASSERT_EQUAL_INT(12, c.preamble);
}

void test_profile_medium_range_values(void) {
    radio_config_t c;
    radio_get_profile_config(RADIO_PROFILE_MEDIUM_RANGE, &c);
    TEST_ASSERT_EQUAL_INT(7, c.sf);
    TEST_ASSERT_EQUAL_UINT32(250000, c.bw_hz);
    TEST_ASSERT_EQUAL_INT(17, c.tx_power);
    TEST_ASSERT_EQUAL_INT(8, c.preamble);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_tx_emits_correct_json_and_completes);
    RUN_TEST(test_tx_broker_down_fails_and_recovers);
    RUN_TEST(test_rx_dispatches_to_callback_with_rssi_snr);
    RUN_TEST(test_rx_without_callback_is_dropped_not_crash);
    RUN_TEST(test_cad_check_reports_busy_channel);
    RUN_TEST(test_cad_check_reports_clear_channel);
    RUN_TEST(test_cad_check_timeout_reports_clear);
    RUN_TEST(test_cad_timeout_fails_open_before_threshold);
    RUN_TEST(test_cad_timeout_fails_closed_and_requests_reinit_at_threshold);
    RUN_TEST(test_cad_success_resets_timeout_streak);
    RUN_TEST(test_init_ends_in_rx);
    RUN_TEST(test_sleep_then_start_rx);
    RUN_TEST(test_get_config_round_trips);
    RUN_TEST(test_profile_long_range_values);
    RUN_TEST(test_profile_medium_range_values);
    return UNITY_END();
}
