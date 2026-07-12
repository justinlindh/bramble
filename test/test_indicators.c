/* setenv/usleep need glibc's non-strict feature set; must be defined before
 * any system header is pulled in (including transitively via unity.h). */
#define _DEFAULT_SOURCE

/* Whitebox tests for indicator_virt (emulator/DESIGN.md sections 5 and 8).
 * Includes the driver source directly (same convention as test_emu_link.c)
 * alongside the real emu_link.c and attaches a socketpair fd as the broker
 * test double. Each indicator setter must emit one `ind` message carrying the
 * full (led, buzzer_hz, vibra) state, so the frontend always has a complete
 * snapshot regardless of which field changed. */
#include "unity.h"

#include "../components/emu_link/emu_link.c"
#include "../components/indicators/indicator_virt.c"

#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static int s_broker_fd = -1;

void setUp(void) { s_broker_fd = -1; }

void tearDown(void) {
    emu_link_close();
    if (s_broker_fd != -1) {
        close(s_broker_fd);
        s_broker_fd = -1;
    }
    memset(s_handlers, 0, sizeof(s_handlers));
    /* Reset indicator state between tests. */
    pthread_mutex_lock(&s_ind_mu);
    s_led = false;
    s_buzzer_hz = 0;
    s_vibra = false;
    pthread_mutex_unlock(&s_ind_mu);
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

static void attach_and_drain_hello(const char* node_id) {
    int fds[2];
    TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    s_broker_fd = fds[0];
    TEST_ASSERT_EQUAL_INT(0, emu_link_attach(fds[1], node_id, ""));
    char hello[256];
    read_line_timeout(s_broker_fd, hello, sizeof(hello), 2000);
}

/* Reads the next `ind` line and returns its parsed object (caller frees). */
static cJSON* read_ind(void) {
    char line[256];
    read_line_timeout(s_broker_fd, line, sizeof(line), 2000);
    cJSON* m = cJSON_Parse(line);
    TEST_ASSERT_NOT_NULL_MESSAGE(m, "expected an ind message");
    TEST_ASSERT_EQUAL_STRING("ind", cJSON_GetObjectItem(m, "t")->valuestring);
    return m;
}

void test_set_led_emits_full_state(void) {
    attach_and_drain_hello("pager-ind-1");

    indicator_set_led(true);
    cJSON* m = read_ind();
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(m, "led")));
    TEST_ASSERT_EQUAL_INT(0, cJSON_GetObjectItem(m, "buzzer_hz")->valueint);
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(m, "vibra")));
    cJSON_Delete(m);
}

void test_buzzer_emits_full_state(void) {
    attach_and_drain_hello("pager-ind-2");

    indicator_buzzer(2731);
    cJSON* m = read_ind();
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(m, "led")));
    TEST_ASSERT_EQUAL_INT(2731, cJSON_GetObjectItem(m, "buzzer_hz")->valueint);
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(m, "vibra")));
    cJSON_Delete(m);
}

/* Each change carries the full state: prior fields persist across setters. */
void test_state_accumulates_across_setters(void) {
    attach_and_drain_hello("pager-ind-3");

    indicator_set_led(true);
    cJSON_Delete(read_ind());
    indicator_vibra(true);
    cJSON_Delete(read_ind());
    indicator_buzzer(1000);

    cJSON* m = read_ind();
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(m, "led")));
    TEST_ASSERT_EQUAL_INT(1000, cJSON_GetObjectItem(m, "buzzer_hz")->valueint);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(m, "vibra")));
    cJSON_Delete(m);

    /* Turning the buzzer off (0) reports the rest of the state unchanged. */
    indicator_buzzer(0);
    m = read_ind();
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(m, "led")));
    TEST_ASSERT_EQUAL_INT(0, cJSON_GetObjectItem(m, "buzzer_hz")->valueint);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(m, "vibra")));
    cJSON_Delete(m);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_set_led_emits_full_state);
    RUN_TEST(test_buzzer_emits_full_state);
    RUN_TEST(test_state_accumulates_across_setters);
    return UNITY_END();
}
