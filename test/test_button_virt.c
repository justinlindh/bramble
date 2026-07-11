/* setenv/usleep need glibc's non-strict feature set; must be defined before
 * any system header is pulled in (including transitively via unity.h). */
#define _DEFAULT_SOURCE

/* Whitebox tests for button_virt (emulator/DESIGN.md sections 5 and 8).
 * Includes the driver source directly (same convention as test_emu_link.c)
 * alongside the real emu_link.c and attaches a socketpair fd as the broker
 * test double. Broker `btn` messages become ui_button_t events drained by the
 * firmware's button_poll() contract; a `reset` edge is not a button event, it
 * exits the process (tested via the injectable exit hook so the test survives). */
#include "unity.h"

#include "../components/button/button_virt.c"
#include "../components/emu_link/emu_link.c"

#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static int s_broker_fd = -1;
static int s_exit_calls = 0;
static int s_exit_code = -1;

static void fake_exit(int code) {
    s_exit_calls++;
    s_exit_code = code;
}

void setUp(void) {
    s_broker_fd = -1;
    s_exit_calls = 0;
    s_exit_code = -1;
}

void tearDown(void) {
    emu_link_close();
    if (s_broker_fd != -1) {
        close(s_broker_fd);
        s_broker_fd = -1;
    }
    memset(s_handlers, 0, sizeof(s_handlers));
    pthread_mutex_lock(&s_btn_mu);
    s_head = s_tail = s_count = 0;
    pthread_mutex_unlock(&s_btn_mu);
    s_exit_hook = NULL;
}

static void read_line_timeout(int fd, char *out, size_t out_sz, int deadline_ms) {
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

static void attach_and_drain_hello(const char *node_id) {
    int fds[2];
    TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    s_broker_fd = fds[0];
    TEST_ASSERT_EQUAL_INT(0, emu_link_attach(fds[1], node_id, ""));
    char hello[256];
    read_line_timeout(s_broker_fd, hello, sizeof(hello), 2000);
}

static void send_btn(const char *id, const char *edge) {
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "t", "btn");
    cJSON_AddStringToObject(m, "id", id);
    cJSON_AddStringToObject(m, "edge", edge);
    char *text = cJSON_PrintUnformatted(m);
    cJSON_Delete(m);
    TEST_ASSERT_NOT_NULL(text);
    write_all(s_broker_fd, text, strlen(text));
    write_all(s_broker_fd, "\n", 1);
    free(text);
}

static ui_button_t poll_until_event(int deadline_ms) {
    for (int waited = 0; waited < deadline_ms; waited += 5) {
        ui_button_t b = button_poll(0);
        if (b != BTN_NONE)
            return b;
        usleep(5000);
    }
    return button_poll(0);
}

/* --- a press (down) edge dispatches the mapped button event once --- */
void test_press_edge_dispatches_event(void) {
    attach_and_drain_hello("pager-btn-1");
    button_init();

    send_btn("up", "down");
    TEST_ASSERT_EQUAL_INT(BTN_UP, poll_until_event(2000));
    /* Exactly one event: a subsequent poll drains empty. */
    TEST_ASSERT_EQUAL_INT(BTN_NONE, button_poll(0));
}

void test_all_three_buttons_map(void) {
    attach_and_drain_hello("pager-btn-2");
    button_init();

    send_btn("down", "down");
    TEST_ASSERT_EQUAL_INT(BTN_DOWN, poll_until_event(2000));
    send_btn("select", "down");
    TEST_ASSERT_EQUAL_INT(BTN_SELECT, poll_until_event(2000));
}

/* --- the release (up) edge is not an event; only presses dispatch --- */
void test_release_edge_is_not_an_event(void) {
    attach_and_drain_hello("pager-btn-3");
    button_init();

    send_btn("up", "up");
    usleep(150000);
    TEST_ASSERT_EQUAL_INT(BTN_NONE, button_poll(0));
}

/* --- reset is not a button event: it exits the process cleanly --- */
void test_reset_exits_process(void) {
    attach_and_drain_hello("pager-btn-4");
    button_init();
    s_exit_hook = fake_exit;

    send_btn("reset", "down");
    for (int waited = 0; waited < 2000 && s_exit_calls == 0; waited += 5)
        usleep(5000);

    TEST_ASSERT_EQUAL_INT(1, s_exit_calls);
    TEST_ASSERT_EQUAL_INT(0, s_exit_code); /* clean exit(0) */
    /* reset never surfaces as a poll event. */
    TEST_ASSERT_EQUAL_INT(BTN_NONE, button_poll(0));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_press_edge_dispatches_event);
    RUN_TEST(test_all_three_buttons_map);
    RUN_TEST(test_release_edge_is_not_an_event);
    RUN_TEST(test_reset_exits_process);
    return UNITY_END();
}
