/* setenv/unsetenv/usleep need glibc's non-strict feature set; must be
 * defined before any system header is pulled in (including transitively
 * via unity.h). */
#define _DEFAULT_SOURCE

/* Whitebox tests for emu_link (emulator/DESIGN.md section 8). Includes the
 * component source directly (same convention as test_neighbor.c/routing.c
 * and test_mailbox.c/mailbox.c) so tests can attach a socketpair fd via the
 * internal emu_link_attach() helper as a broker test double, instead of
 * dialing a real EMU_BROKER unix socket for every framing/dispatch case. */
#include "unity.h"

#include "../components/emu_link/emu_link.c"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

/* The "broker" end of the pair under test: the far side of the socketpair
 * (or of a real listener for the connect()-level tests). */
static int s_broker_fd = -1;

void setUp(void) { s_broker_fd = -1; }

void tearDown(void) {
    emu_link_close();
    if (s_broker_fd != -1) {
        close(s_broker_fd);
        s_broker_fd = -1;
    }
    memset(s_handlers, 0, sizeof(s_handlers));
    unsetenv("EMU_BROKER");
}

/* --- helpers ----------------------------------------------------------- */

/* Reads from fd until a '\n' is seen or deadline_ms elapses, returning the
 * line (without the newline) in out, or an empty string on timeout/EOF. */
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

typedef struct {
    _Atomic int calls;
    char last_type[32];
    int last_int_field;
} capture_t;

static void capture_handler(const cJSON* msg, void* ctx) {
    capture_t* c = (capture_t*)ctx;
    atomic_fetch_add(&c->calls, 1);
    const cJSON* t = cJSON_GetObjectItem(msg, "t");
    if (cJSON_IsString(t)) {
        strncpy(c->last_type, t->valuestring, sizeof(c->last_type) - 1);
        c->last_type[sizeof(c->last_type) - 1] = '\0';
    }
    const cJSON* v = cJSON_GetObjectItem(msg, "v");
    if (cJSON_IsNumber(v))
        c->last_int_field = v->valueint;
}

static bool wait_for_calls(_Atomic int* counter, int want, int deadline_ms) {
    for (int waited = 0; waited < deadline_ms; waited += 5) {
        if (atomic_load(counter) >= want)
            return true;
        usleep(5000);
    }
    return atomic_load(counter) >= want;
}

/* Opens a connected pair and attaches one end as the node under test via the
 * internal helper, returning the broker-side fd (also stashed in
 * s_broker_fd so tearDown cleans it up). */
static void attach_pair(const char* node_id, const char* caps) {
    int fds[2];
    TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    s_broker_fd = fds[0];
    TEST_ASSERT_EQUAL_INT(0, emu_link_attach(fds[1], node_id, caps));
}

/* --- hello on connect ---------------------------------------------------*/

void test_hello_sent_on_attach(void) {
    attach_pair("pager-1", "radio,display,gps");

    char line[512];
    read_line_timeout(s_broker_fd, line, sizeof(line), 2000);
    TEST_ASSERT_TRUE(strlen(line) > 0);

    cJSON* hello = cJSON_Parse(line);
    TEST_ASSERT_NOT_NULL(hello);
    TEST_ASSERT_EQUAL_STRING("hello", cJSON_GetObjectItem(hello, "t")->valuestring);
    TEST_ASSERT_EQUAL_STRING("pager-1", cJSON_GetObjectItem(hello, "node")->valuestring);
    TEST_ASSERT_EQUAL_STRING("radio,display,gps", cJSON_GetObjectItem(hello, "caps")->valuestring);
    TEST_ASSERT_EQUAL_INT(1, cJSON_GetObjectItem(hello, "version")->valueint);
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(hello, "fw"));
    cJSON_Delete(hello);
}

void test_hello_caps_null_becomes_empty(void) {
    attach_pair("pager-2", NULL);

    char line[512];
    read_line_timeout(s_broker_fd, line, sizeof(line), 2000);
    cJSON* hello = cJSON_Parse(line);
    TEST_ASSERT_NOT_NULL(hello);
    TEST_ASSERT_EQUAL_STRING("", cJSON_GetObjectItem(hello, "caps")->valuestring);
    cJSON_Delete(hello);
}

/* --- framing ------------------------------------------------------------*/

void test_framing_object_split_across_multiple_reads(void) {
    attach_pair("pager-3", "");
    char discard[512];
    read_line_timeout(s_broker_fd, discard, sizeof(discard), 2000); /* hello */

    capture_t cap = {0};
    TEST_ASSERT_EQUAL_INT(0, emu_link_on("rx", capture_handler, &cap));

    const char* line = "{\"t\":\"rx\",\"v\":42}\n";
    size_t total = strlen(line);
    size_t split = 6; /* mid-object */
    TEST_ASSERT_EQUAL_INT(0, write_all(s_broker_fd, line, split));
    usleep(20000); /* force two separate recv() calls on the reader side */
    TEST_ASSERT_EQUAL_INT(0, write_all(s_broker_fd, line + split, total - split));

    TEST_ASSERT_TRUE(wait_for_calls(&cap.calls, 1, 2000));
    TEST_ASSERT_EQUAL_INT(1, atomic_load(&cap.calls));
    TEST_ASSERT_EQUAL_STRING("rx", cap.last_type);
    TEST_ASSERT_EQUAL_INT(42, cap.last_int_field);
}

void test_framing_multiple_objects_in_one_read(void) {
    attach_pair("pager-4", "");
    char discard[512];
    read_line_timeout(s_broker_fd, discard, sizeof(discard), 2000); /* hello */

    capture_t cap = {0};
    TEST_ASSERT_EQUAL_INT(0, emu_link_on("btn", capture_handler, &cap));

    const char* both = "{\"t\":\"btn\",\"v\":1}\n{\"t\":\"btn\",\"v\":2}\n";
    TEST_ASSERT_EQUAL_INT(0, write_all(s_broker_fd, both, strlen(both)));

    TEST_ASSERT_TRUE(wait_for_calls(&cap.calls, 2, 2000));
    TEST_ASSERT_EQUAL_INT(2, atomic_load(&cap.calls));
    TEST_ASSERT_EQUAL_INT(2, cap.last_int_field); /* second call overwrote the first */
}

void test_oversized_line_dropped_then_resyncs(void) {
    attach_pair("pager-5", "");
    char discard[512];
    read_line_timeout(s_broker_fd, discard, sizeof(discard), 2000); /* hello */

    capture_t cap = {0};
    TEST_ASSERT_EQUAL_INT(0, emu_link_on("rx", capture_handler, &cap));

    /* A line longer than EMU_LINK_MAX_LINE with no newline: the reader must
     * not crash or hang, and must recover once real framing resumes. */
    size_t junk_len = EMU_LINK_MAX_LINE + 4096;
    char* junk = (char*)malloc(junk_len);
    TEST_ASSERT_NOT_NULL(junk);
    memset(junk, 'a', junk_len);
    TEST_ASSERT_EQUAL_INT(0, write_all(s_broker_fd, junk, junk_len));
    free(junk);
    TEST_ASSERT_EQUAL_INT(0, write_all(s_broker_fd, "\n", 1));

    const char* good = "{\"t\":\"rx\",\"v\":7}\n";
    TEST_ASSERT_EQUAL_INT(0, write_all(s_broker_fd, good, strlen(good)));

    TEST_ASSERT_TRUE(wait_for_calls(&cap.calls, 1, 3000));
    TEST_ASSERT_EQUAL_INT(7, cap.last_int_field);
}

/* --- dispatch -------------------------------------------------------- */

void test_handler_dispatch_by_type(void) {
    attach_pair("pager-6", "");
    char discard[512];
    read_line_timeout(s_broker_fd, discard, sizeof(discard), 2000); /* hello */

    capture_t rx_cap = {0};
    capture_t btn_cap = {0};
    TEST_ASSERT_EQUAL_INT(0, emu_link_on("rx", capture_handler, &rx_cap));
    TEST_ASSERT_EQUAL_INT(0, emu_link_on("btn", capture_handler, &btn_cap));

    const char* msg = "{\"t\":\"btn\",\"v\":9}\n";
    TEST_ASSERT_EQUAL_INT(0, write_all(s_broker_fd, msg, strlen(msg)));

    TEST_ASSERT_TRUE(wait_for_calls(&btn_cap.calls, 1, 2000));
    usleep(50000);
    TEST_ASSERT_EQUAL_INT(0, atomic_load(&rx_cap.calls));
    TEST_ASSERT_EQUAL_INT(1, atomic_load(&btn_cap.calls));
}

void test_unknown_type_silently_ignored(void) {
    attach_pair("pager-7", "");
    char discard[512];
    read_line_timeout(s_broker_fd, discard, sizeof(discard), 2000); /* hello */

    capture_t rx_cap = {0};
    TEST_ASSERT_EQUAL_INT(0, emu_link_on("rx", capture_handler, &rx_cap));

    const char* msgs = "{\"t\":\"someFutureType\",\"v\":1}\n{\"t\":\"rx\",\"v\":5}\n";
    TEST_ASSERT_EQUAL_INT(0, write_all(s_broker_fd, msgs, strlen(msgs)));

    TEST_ASSERT_TRUE(wait_for_calls(&rx_cap.calls, 1, 2000));
    TEST_ASSERT_EQUAL_INT(1, atomic_load(&rx_cap.calls));
    TEST_ASSERT_EQUAL_INT(5, rx_cap.last_int_field);
}

void test_handler_registration_replaces_for_same_type(void) {
    attach_pair("pager-8", "");
    char discard[512];
    read_line_timeout(s_broker_fd, discard, sizeof(discard), 2000);

    capture_t first = {0};
    capture_t second = {0};
    TEST_ASSERT_EQUAL_INT(0, emu_link_on("rx", capture_handler, &first));
    TEST_ASSERT_EQUAL_INT(0, emu_link_on("rx", capture_handler, &second));

    const char* msg = "{\"t\":\"rx\",\"v\":3}\n";
    TEST_ASSERT_EQUAL_INT(0, write_all(s_broker_fd, msg, strlen(msg)));

    TEST_ASSERT_TRUE(wait_for_calls(&second.calls, 1, 2000));
    usleep(50000);
    TEST_ASSERT_EQUAL_INT(0, atomic_load(&first.calls));
    TEST_ASSERT_EQUAL_INT(1, atomic_load(&second.calls));
}

/* --- emu_link_on argument validation ----------------------------------- */

void test_on_rejects_null_type_or_handler(void) {
    TEST_ASSERT_TRUE(emu_link_on(NULL, capture_handler, NULL) != 0);
    TEST_ASSERT_TRUE(emu_link_on("rx", NULL, NULL) != 0);
}

/* --- send ---------------------------------------------------------------*/

void test_send_requires_t_field(void) {
    cJSON* no_t = cJSON_CreateObject();
    cJSON_AddNumberToObject(no_t, "v", 1);
    TEST_ASSERT_TRUE(emu_link_send(no_t) != 0); /* takes ownership even on failure */
}

void test_send_null_is_rejected_not_crashed(void) { TEST_ASSERT_TRUE(emu_link_send(NULL) != 0); }

void test_send_without_connection_fails_cleanly(void) {
    cJSON* msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "t", "log");
    TEST_ASSERT_TRUE(emu_link_send(msg) != 0);
}

void test_send_delivers_line_with_type(void) {
    attach_pair("pager-9", "");
    char discard[512];
    read_line_timeout(s_broker_fd, discard, sizeof(discard), 2000); /* hello */

    cJSON* msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "t", "log");
    cJSON_AddStringToObject(msg, "line", "booted");
    TEST_ASSERT_EQUAL_INT(0, emu_link_send(msg));

    char line[512];
    read_line_timeout(s_broker_fd, line, sizeof(line), 2000);
    cJSON* got = cJSON_Parse(line);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQUAL_STRING("log", cJSON_GetObjectItem(got, "t")->valuestring);
    TEST_ASSERT_EQUAL_STRING("booted", cJSON_GetObjectItem(got, "line")->valuestring);
    cJSON_Delete(got);
}

/* --- concurrency ----------------------------------------------------- */

#define SEND_THREADS 8
#define SENDS_PER_THREAD 50
#define SEND_TOTAL (SEND_THREADS * SENDS_PER_THREAD)

static void* sender_thread(void* arg) {
    int id = (int)(intptr_t)arg;
    for (int i = 0; i < SENDS_PER_THREAD; i++) {
        cJSON* msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "t", "log");
        cJSON_AddNumberToObject(msg, "thread", id);
        cJSON_AddNumberToObject(msg, "seq", i);
        emu_link_send(msg);
    }
    return NULL;
}

/* Drains the broker-side fd concurrently with the senders (a socketpair's
 * kernel buffer is finite; draining only after every sender has finished
 * would deadlock once it fills, exactly like a real broker would if it
 * stopped reading). Captures each line's (thread, seq) into seen[][] for
 * the caller to validate after the drainer finishes. */
typedef struct {
    int fd;
    int expect;
    int seen[SEND_THREADS][SENDS_PER_THREAD];
    int total_seen;
    int corrupt;
} drain_result_t;

static void* drain_thread(void* arg) {
    drain_result_t* r = (drain_result_t*)arg;
    while (r->total_seen < r->expect) {
        char line[512];
        read_line_timeout(r->fd, line, sizeof(line), 5000);
        if (strlen(line) == 0)
            break; /* timeout: caller's count check will catch the shortfall */
        cJSON* msg = cJSON_Parse(line);
        if (!msg) {
            r->corrupt++;
            continue;
        }
        const cJSON* t = cJSON_GetObjectItem(msg, "t");
        const cJSON* tid_j = cJSON_GetObjectItem(msg, "thread");
        const cJSON* seq_j = cJSON_GetObjectItem(msg, "seq");
        if (!cJSON_IsString(t) || strcmp(t->valuestring, "log") != 0 || !cJSON_IsNumber(tid_j) ||
            !cJSON_IsNumber(seq_j)) {
            r->corrupt++;
            cJSON_Delete(msg);
            continue;
        }
        int tid = tid_j->valueint;
        int seq = seq_j->valueint;
        if (tid >= 0 && tid < SEND_THREADS && seq >= 0 && seq < SENDS_PER_THREAD) {
            r->seen[tid][seq]++;
            r->total_seen++;
        } else {
            r->corrupt++;
        }
        cJSON_Delete(msg);
    }
    return NULL;
}

void test_concurrent_sends_are_thread_safe(void) {
    attach_pair("pager-10", "");
    char discard[512];
    read_line_timeout(s_broker_fd, discard, sizeof(discard), 2000); /* hello */

    drain_result_t result;
    memset(&result, 0, sizeof(result));
    result.fd = s_broker_fd;
    result.expect = SEND_TOTAL;

    pthread_t drainer;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&drainer, NULL, drain_thread, &result));

    pthread_t threads[SEND_THREADS];
    for (int i = 0; i < SEND_THREADS; i++)
        TEST_ASSERT_EQUAL_INT(0,
                              pthread_create(&threads[i], NULL, sender_thread, (void*)(intptr_t)i));
    for (int i = 0; i < SEND_THREADS; i++)
        pthread_join(threads[i], NULL);
    pthread_join(drainer, NULL);

    /* Every line must have been a complete, parseable JSON object with the
     * right fields (interleaved/corrupted writes would show up as a parse
     * failure), and every (thread, seq) pair must appear exactly once. */
    TEST_ASSERT_EQUAL_INT(0, result.corrupt);
    TEST_ASSERT_EQUAL_INT(SEND_TOTAL, result.total_seen);
    for (int t = 0; t < SEND_THREADS; t++)
        for (int s = 0; s < SENDS_PER_THREAD; s++)
            TEST_ASSERT_EQUAL_INT_MESSAGE(1, result.seen[t][s],
                                          "missing or duplicated (thread, seq)");
}

/* --- broker disconnect -------------------------------------------------*/

void test_broker_disconnect_mid_run_does_not_crash(void) {
    attach_pair("pager-11", "");
    char discard[512];
    read_line_timeout(s_broker_fd, discard, sizeof(discard), 2000); /* hello */

    close(s_broker_fd); /* simulate the broker vanishing */
    s_broker_fd = -1;
    usleep(50000); /* let the reader thread observe EOF */

    /* Sends after the peer is gone must fail cleanly, not crash. */
    cJSON* msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "t", "log");
    emu_link_send(msg); /* rc unspecified once the peer is gone; must not crash */

    emu_link_close(); /* must not hang joining an already-finished reader */
}

/* --- emu_link_connect (real dial path) ----------------------------------*/

void test_connect_fails_cleanly_when_env_unset(void) {
    unsetenv("EMU_BROKER");
    TEST_ASSERT_TRUE(emu_link_connect("node-x", "") != 0);
}

void test_connect_fails_cleanly_on_bad_prefix(void) {
    setenv("EMU_BROKER", "carrier-pigeon:nope", 1);
    TEST_ASSERT_TRUE(emu_link_connect("node-x", "") != 0);
}

void test_connect_fails_cleanly_when_nothing_listening(void) {
    setenv("EMU_BROKER", "unix:/tmp/emu-link-test-nobody-here.sock", 1);
    TEST_ASSERT_TRUE(emu_link_connect("node-x", "") != 0);
}

void test_connect_null_node_id_rejected(void) {
    setenv("EMU_BROKER", "unix:/tmp/emu-link-test-nobody-here.sock", 1);
    TEST_ASSERT_TRUE(emu_link_connect(NULL, "") != 0);
}

/* End-to-end over a real unix socket listener, exercising the actual
 * EMU_BROKER dial path (not the attach() test double). */
static int s_listen_fd = -1;
static char s_sock_path[128];

static void* accept_one(void* arg) {
    (void)arg;
    struct sockaddr_un peer;
    socklen_t len = sizeof(peer);
    int fd = accept(s_listen_fd, (struct sockaddr*)&peer, &len);
    s_broker_fd = fd;
    return NULL;
}

void test_connect_real_unix_socket_end_to_end(void) {
    snprintf(s_sock_path, sizeof(s_sock_path), "/tmp/emu-link-test-%d.sock", (int)getpid());
    unlink(s_sock_path);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, s_sock_path, sizeof(addr.sun_path) - 1);

    s_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    TEST_ASSERT_TRUE(s_listen_fd >= 0);
    TEST_ASSERT_EQUAL_INT(0, bind(s_listen_fd, (struct sockaddr*)&addr, sizeof(addr)));
    TEST_ASSERT_EQUAL_INT(0, listen(s_listen_fd, 1));

    pthread_t accepter;
    pthread_create(&accepter, NULL, accept_one, NULL);

    char env[256];
    snprintf(env, sizeof(env), "unix:%s", s_sock_path);
    setenv("EMU_BROKER", env, 1);

    TEST_ASSERT_EQUAL_INT(0, emu_link_connect("real-node", "radio"));
    pthread_join(accepter, NULL);
    TEST_ASSERT_TRUE(s_broker_fd >= 0);

    char line[512];
    read_line_timeout(s_broker_fd, line, sizeof(line), 2000);
    cJSON* hello = cJSON_Parse(line);
    TEST_ASSERT_NOT_NULL(hello);
    TEST_ASSERT_EQUAL_STRING("hello", cJSON_GetObjectItem(hello, "t")->valuestring);
    TEST_ASSERT_EQUAL_STRING("real-node", cJSON_GetObjectItem(hello, "node")->valuestring);
    cJSON_Delete(hello);

    close(s_listen_fd);
    unlink(s_sock_path);
}

void test_close_then_reconnect(void) {
    attach_pair("pager-12", "");
    char discard[512];
    read_line_timeout(s_broker_fd, discard, sizeof(discard), 2000);
    emu_link_close();
    close(s_broker_fd);
    s_broker_fd = -1;

    attach_pair("pager-13", "");
    read_line_timeout(s_broker_fd, discard, sizeof(discard), 2000);
    TEST_ASSERT_TRUE(strlen(discard) > 0);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_hello_sent_on_attach);
    RUN_TEST(test_hello_caps_null_becomes_empty);
    RUN_TEST(test_framing_object_split_across_multiple_reads);
    RUN_TEST(test_framing_multiple_objects_in_one_read);
    RUN_TEST(test_oversized_line_dropped_then_resyncs);
    RUN_TEST(test_handler_dispatch_by_type);
    RUN_TEST(test_unknown_type_silently_ignored);
    RUN_TEST(test_handler_registration_replaces_for_same_type);
    RUN_TEST(test_on_rejects_null_type_or_handler);
    RUN_TEST(test_send_requires_t_field);
    RUN_TEST(test_send_null_is_rejected_not_crashed);
    RUN_TEST(test_send_without_connection_fails_cleanly);
    RUN_TEST(test_send_delivers_line_with_type);
    RUN_TEST(test_concurrent_sends_are_thread_safe);
    RUN_TEST(test_broker_disconnect_mid_run_does_not_crash);
    RUN_TEST(test_connect_fails_cleanly_when_env_unset);
    RUN_TEST(test_connect_fails_cleanly_on_bad_prefix);
    RUN_TEST(test_connect_fails_cleanly_when_nothing_listening);
    RUN_TEST(test_connect_null_node_id_rejected);
    RUN_TEST(test_connect_real_unix_socket_end_to_end);
    RUN_TEST(test_close_then_reconnect);
    return UNITY_END();
}
