/*
 * Host coverage for main/ws_server.c (issue #96).
 *
 * ws_server.c is the primary client surface: the webapp, the Electron app
 * and the CLI all speak to it. The auth *primitives* were already covered
 * (test_ws_auth.c for the allowlist and notify filter, test_ws_origin.c for
 * the Origin policy, test_ws_auth_credential.c for token extraction), but
 * nothing checked that the server actually applies them to every frame. That
 * gap is what this suite closes.
 *
 * Method: the suite #includes main/ws_server.c directly (the same pattern as
 * test_dm_session.c including crypto_host.c) so the file's static handlers
 * and its static client table are reachable. ws_server.c itself is NOT
 * modified: no seams were added, and the production path is byte-identical.
 * The httpd dependency is met by a host fake, test/stubs/ws/esp_http_server.h.
 *
 * Ordered by risk. The invariants that matter most, and are asserted first:
 *
 *   1. An unauthenticated connection cannot invoke anything outside the
 *      unauthenticated allowlist.
 *   2. Per-connection auth state cannot leak between connections, including
 *      across an fd being closed and reused.
 *   3. Every path that cannot establish auth state fails CLOSED (client
 *      table full, token unavailable in NVS, untracked fd).
 *
 * Framing, connection lifecycle and notification backpressure follow.
 *
 * ── Not covered, deliberately ────────────────────────────────────────────
 *
 *   - Real WebSocket wire framing: masking, fragmentation, continuation
 *     frames, UTF-8 validation, and the 101 handshake itself are ESP-IDF's
 *     job and happen below ws_handler(). The fake hands ws_handler() an
 *     already-decoded frame, exactly as httpd does.
 *   - Concurrency. The client table is guarded by a portMUX spinlock that
 *     the host shim compiles to a no-op, so this suite cannot show that the
 *     locking is correct, only that the bookkeeping under it is. Races
 *     between the httpd task and rpc_notify() callers remain untested.
 *   - Socket-level backpressure. The throttle in ws_notify_cb() is tested;
 *     what happens when the underlying LwIP send buffer fills is not, since
 *     the fake never blocks.
 *   - The /config setup portal beyond its CSRF and token gate: HTML
 *     rendering and the reboot side effect are asserted only shallowly.
 *   - httpd_start() failure handling and ws_server_stop() teardown of live
 *     sockets, which need a real httpd.
 */

#include "unity.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "esp_http_server.h"
#include "nvs.h"
/* Must precede ws_server.c: claims BRAMBLE_MESH_TASK_H so the real
 * main/mesh_task.h, which a quoted include from main/ would otherwise
 * always win, expands to nothing. See stubs/ws/mesh_task.h. */
#include "mesh_task.h"

/* ── Controllable clock (drives the notification throttle) ───────────── */
/* stubs/esp_timer.h exposes a static-inline zero clock unless this is set. */
#define ESP_TIMER_CUSTOM_IMPL
static int64_t g_now_us;
int64_t esp_timer_get_time(void) { return g_now_us; }

/* ── Controllable identity + NVS backing for the module under test ───── */

static char g_nvs_token[128];
static int g_token_rc;
static char g_nvs_origins[256];
static int g_reboot_calls;

int identity_ensure_ws_auth_token(char* token_out, size_t token_out_len) {
    if (g_token_rc < 0)
        return g_token_rc;
    snprintf(token_out, token_out_len, "%s", g_nvs_token);
    return 0;
}

void mesh_reboot_delayed(int delay_ms) {
    (void)delay_ms;
    g_reboot_calls++;
}

esp_err_t nvs_open(const char* ns, int mode, nvs_handle_t* out) {
    (void)ns;
    (void)mode;
    if (out)
        *out = 1;
    return ESP_OK;
}
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_get_str(nvs_handle_t h, const char* key, char* out, size_t* len) {
    (void)h;
    (void)key;
    if (g_nvs_origins[0] == '\0')
        return ESP_FAIL;
    size_t n = strlen(g_nvs_origins);
    if (!out || !len || *len < n + 1)
        return ESP_FAIL;
    memcpy(out, g_nvs_origins, n + 1);
    *len = n + 1;
    return ESP_OK;
}

/* The module under test, statics and all.
 *
 * The host suite builds with -Wextra -Werror, which the ESP-IDF firmware
 * build does not. Two ESP-IDF callback signatures (rpc_notify_cb_t and
 * httpd_close_func_t) carry parameters ws_server.c has no use for. The
 * relaxation is scoped to this include so the test code itself, and every
 * other suite, keeps the full warning set. Nothing in ws_server.c was
 * changed to make it compile here. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "../main/ws_server.c"
#pragma GCC diagnostic pop

#define GOOD_TOKEN "sekrit-token-0123456789abcdef"
#define DEVICE_HOST "192.0.2.10"

/* ── RPC methods the tests dispatch against ──────────────────────────── */

static int handler_ok(const cJSON* params, cJSON* result) {
    (void)params;
    cJSON_AddStringToObject(result, "ok", "yes");
    return 0;
}

/* ── Request construction helpers ────────────────────────────────────── */

static httpd_req_t make_req(int method, int fd) {
    httpd_req_t r;
    memset(&r, 0, sizeof(r));
    r.method = method;
    r.fake_sockfd = fd;
    r.handle = s_server;
    return r;
}

/*
 * Perform a WS upgrade. `authz`, `subproto`, `origin` and `host` are header
 * values or NULL for absent. Returns what ws_handler() returned.
 */
static esp_err_t handshake(int fd, const char* authz, const char* subproto, const char* origin,
                           const char* host) {
    httpd_req_t r = make_req(HTTP_GET, fd);
    if (authz)
        fake_req_add_header(&r, "Authorization", authz);
    if (subproto)
        fake_req_add_header(&r, "Sec-WebSocket-Protocol", subproto);
    if (origin)
        fake_req_add_header(&r, "Origin", origin);
    if (host)
        fake_req_add_header(&r, "Host", host);
    return ws_handler(&r);
}

/* Same-origin upgrade with no credentials: the pairing path. */
static esp_err_t handshake_unauth(int fd) {
    return handshake(fd, NULL, NULL, "http://" DEVICE_HOST, DEVICE_HOST);
}

/* Upgrade presenting the correct device token. */
static esp_err_t handshake_authed(int fd) {
    return handshake(fd, "Bearer " GOOD_TOKEN, NULL, "http://" DEVICE_HOST, DEVICE_HOST);
}

/* Deliver one already-decoded frame to the established connection on `fd`. */
static esp_err_t deliver_frame(int fd, httpd_ws_type_t type, const void* payload, size_t len) {
    httpd_req_t r = make_req(HTTP_POST, fd); /* any non-GET method */
    r.fake_rx_type = type;
    r.fake_rx_payload = (const uint8_t*)payload;
    r.fake_rx_len = len;
    return ws_handler(&r);
}

static esp_err_t call_method(int fd, const char* method) {
    char req[256];
    snprintf(req, sizeof(req), "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"%s\"}", method);
    return deliver_frame(fd, HTTPD_WS_TYPE_TEXT, req, strlen(req));
}

/* Parse the JSON-RPC error code from the last frame the server sent back.
 * Returns 0 if the reply carried a result rather than an error. */
static int last_reply_error_code(void) {
    const fake_sent_frame_t* f = fake_last_sent_frame();
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "server sent no reply frame");
    TEST_ASSERT_EQUAL(HTTPD_WS_TYPE_TEXT, f->type);
    cJSON* j = cJSON_Parse((const char*)f->payload);
    TEST_ASSERT_NOT_NULL_MESSAGE(j, "reply was not valid JSON");
    cJSON* err = cJSON_GetObjectItem(j, "error");
    int code = 0;
    if (err)
        code = cJSON_GetObjectItem(err, "code")->valueint;
    else
        TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(j, "result"));
    cJSON_Delete(j);
    return code;
}

void setUp(void) {
    fake_httpd_reset();
    rpc_init();
    rpc_register("bramble.getVersion", handler_ok);  /* on the unauth allowlist */
    rpc_register("bramble.ping", handler_ok);        /* on the unauth allowlist */
    rpc_register("bramble.getMessages", handler_ok); /* privileged */
    rpc_register("bramble.sendMessage", handler_ok); /* privileged */

    snprintf(g_nvs_token, sizeof(g_nvs_token), "%s", GOOD_TOKEN);
    g_token_rc = 0;
    g_nvs_origins[0] = '\0';
    g_reboot_calls = 0;
    g_now_us = 1000000;

    /* Reset the module's statics between tests. */
    s_client_count = 0;
    s_server_running = false;
    s_server = NULL;
    s_token_unavailable = false;
    s_notify_burst_count = 0;
    s_notify_last_send_us = 0;
    s_notify_drops = 0;
    s_notify_last_drop_log_us = 0;

    TEST_ASSERT_EQUAL(0, ws_server_start());
}

void tearDown(void) {}

/* ════════════════════════════════════════════════════════════════════════
 * 1. Auth enforcement on every frame
 * ════════════════════════════════════════════════════════════════════════ */

/* THE invariant. An unauthenticated connection is admitted (that is the
 * pairing path) but must not reach anything off the allowlist. */
void test_unauth_connection_cannot_invoke_privileged_method(void) {
    TEST_ASSERT_EQUAL(ESP_OK, handshake_unauth(7));
    TEST_ASSERT_FALSE(client_is_authed(7));

    TEST_ASSERT_EQUAL(ESP_OK, call_method(7, "bramble.getMessages"));
    TEST_ASSERT_EQUAL(RPC_ERR_UNAUTHORIZED, last_reply_error_code());
}

void test_unauth_connection_can_invoke_allowlisted_method(void) {
    TEST_ASSERT_EQUAL(ESP_OK, handshake_unauth(7));
    TEST_ASSERT_EQUAL(ESP_OK, call_method(7, "bramble.getVersion"));
    TEST_ASSERT_EQUAL(0, last_reply_error_code());
}

/* An unauthenticated caller must not be able to enumerate the method table:
 * unknown methods answer Unauthorized, not Method-not-found. */
void test_unauth_connection_cannot_enumerate_methods(void) {
    TEST_ASSERT_EQUAL(ESP_OK, handshake_unauth(7));
    TEST_ASSERT_EQUAL(ESP_OK, call_method(7, "bramble.noSuchMethod"));
    TEST_ASSERT_EQUAL(RPC_ERR_UNAUTHORIZED, last_reply_error_code());
}

void test_authed_connection_reaches_privileged_methods(void) {
    TEST_ASSERT_EQUAL(ESP_OK, handshake_authed(9));
    TEST_ASSERT_TRUE(client_is_authed(9));

    TEST_ASSERT_EQUAL(ESP_OK, call_method(9, "bramble.getMessages"));
    TEST_ASSERT_EQUAL(0, last_reply_error_code());
}

/* A browser cannot set Authorization, so it carries the token in the
 * subprotocol offer. Same credential, same privilege. */
void test_subprotocol_token_authenticates(void) {
    TEST_ASSERT_EQUAL(ESP_OK, handshake(11, NULL, "bramble.v1.auth." GOOD_TOKEN ", bramble.v1",
                                        "http://" DEVICE_HOST, DEVICE_HOST));
    TEST_ASSERT_TRUE(client_is_authed(11));
    TEST_ASSERT_EQUAL(ESP_OK, call_method(11, "bramble.sendMessage"));
    TEST_ASSERT_EQUAL(0, last_reply_error_code());
}

/* Wrong credentials are not downgraded to the pairing allowlist: the
 * upgrade is refused outright with an RFC 6455 1008 close frame, because
 * ESP-IDF has already sent 101 by the time the handler runs. */
void test_bad_credentials_are_rejected_with_a_1008_close_frame(void) {
    TEST_ASSERT_EQUAL(ESP_FAIL, handshake(13, "Bearer wrong-token-entirely", NULL,
                                          "http://" DEVICE_HOST, DEVICE_HOST));

    const fake_sent_frame_t* f = fake_last_sent_frame();
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL(HTTPD_WS_TYPE_CLOSE, f->type);
    TEST_ASSERT_EQUAL(1008, (f->payload[0] << 8) | f->payload[1]);

    /* And it is not left in the client table. */
    TEST_ASSERT_EQUAL(0, s_client_count);
    TEST_ASSERT_FALSE(client_is_authed(13));
}

/* A valid token overrides the Origin allowlist: a cross-site page cannot
 * read the token, so a token-bearing foreign origin is the user's own
 * client. Documented policy in ws_handler(); pinned here so it cannot be
 * silently reversed. */
void test_valid_token_skips_the_origin_check(void) {
    TEST_ASSERT_EQUAL(
        ESP_OK, handshake(15, "Bearer " GOOD_TOKEN, NULL, "https://evil.example", DEVICE_HOST));
    TEST_ASSERT_TRUE(client_is_authed(15));
}

/* Without a token, the Origin allowlist is the CSWSH defense and applies. */
void test_foreign_origin_without_token_is_rejected(void) {
    TEST_ASSERT_EQUAL(ESP_FAIL, handshake(17, NULL, NULL, "https://evil.example", DEVICE_HOST));
    TEST_ASSERT_EQUAL(HTTPD_WS_TYPE_CLOSE, fake_last_sent_frame()->type);
    TEST_ASSERT_EQUAL(0, s_client_count);
}

/* An Origin too long to fit the buffer cannot match anything allowable, so
 * it must be rejected rather than compared against a truncated value. */
void test_oversized_origin_is_rejected(void) {
    char huge[WS_ORIGINS_MAX + 64];
    memset(huge, 'a', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';
    memcpy(huge, "https://", 8);

    TEST_ASSERT_EQUAL(ESP_FAIL, handshake(19, NULL, NULL, huge, DEVICE_HOST));
    TEST_ASSERT_EQUAL(HTTPD_WS_TYPE_CLOSE, fake_last_sent_frame()->type);
    TEST_ASSERT_EQUAL(0, s_client_count);
}

/* ════════════════════════════════════════════════════════════════════════
 * 2. Per-connection auth state isolation
 * ════════════════════════════════════════════════════════════════════════ */

/* Two live connections, one authenticated and one not. Each frame must be
 * gated by ITS OWN connection's state. */
void test_auth_state_does_not_leak_between_connections(void) {
    TEST_ASSERT_EQUAL(ESP_OK, handshake_authed(21));
    TEST_ASSERT_EQUAL(ESP_OK, handshake_unauth(22));

    TEST_ASSERT_TRUE(client_is_authed(21));
    TEST_ASSERT_FALSE(client_is_authed(22));

    TEST_ASSERT_EQUAL(ESP_OK, call_method(22, "bramble.getMessages"));
    TEST_ASSERT_EQUAL(RPC_ERR_UNAUTHORIZED, last_reply_error_code());

    TEST_ASSERT_EQUAL(ESP_OK, call_method(21, "bramble.getMessages"));
    TEST_ASSERT_EQUAL(0, last_reply_error_code());

    /* The privileged call on 21 must not have promoted 22. */
    TEST_ASSERT_FALSE(client_is_authed(22));
    TEST_ASSERT_EQUAL(ESP_OK, call_method(22, "bramble.sendMessage"));
    TEST_ASSERT_EQUAL(RPC_ERR_UNAUTHORIZED, last_reply_error_code());
}

/* File descriptors are recycled by the kernel. An authenticated connection
 * that closes must not leave its privilege behind for whoever gets that fd
 * next. */
void test_closed_authed_fd_does_not_leak_privilege_to_its_reuse(void) {
    TEST_ASSERT_EQUAL(ESP_OK, handshake_authed(25));
    TEST_ASSERT_TRUE(client_is_authed(25));

    /* Peer closes. */
    TEST_ASSERT_EQUAL(ESP_OK, deliver_frame(25, HTTPD_WS_TYPE_CLOSE, NULL, 0));
    TEST_ASSERT_EQUAL(0, s_client_count);
    TEST_ASSERT_FALSE(client_is_authed(25));

    /* A new, credential-free client lands on the same fd. */
    TEST_ASSERT_EQUAL(ESP_OK, handshake_unauth(25));
    TEST_ASSERT_FALSE(client_is_authed(25));
    TEST_ASSERT_EQUAL(ESP_OK, call_method(25, "bramble.getMessages"));
    TEST_ASSERT_EQUAL(RPC_ERR_UNAUTHORIZED, last_reply_error_code());
}

/* Belt and braces for the same hazard: even if the close was missed and the
 * table entry survived, re-handshaking the fd must overwrite its auth flag
 * rather than keeping the stale one. */
void test_rehandshake_overwrites_stale_auth_flag(void) {
    client_add(27, true);
    TEST_ASSERT_TRUE(client_is_authed(27));

    TEST_ASSERT_EQUAL(ESP_OK, handshake_unauth(27));
    TEST_ASSERT_FALSE(client_is_authed(27));
    TEST_ASSERT_EQUAL(1, s_client_count);
}

/* Removing one client must not disturb the auth flags of the others. The
 * table compacts by moving the last entry into the hole, so this pins that
 * the moved entry keeps its own state. */
void test_client_removal_preserves_other_entries_auth_state(void) {
    client_add(30, false);
    client_add(31, true);
    client_add(32, false);
    client_add(33, true);

    client_remove(31); /* 33 gets swapped into slot 1 */

    TEST_ASSERT_EQUAL(3, s_client_count);
    TEST_ASSERT_FALSE(client_is_authed(30));
    TEST_ASSERT_FALSE(client_is_authed(32));
    TEST_ASSERT_TRUE(client_is_authed(33));
    TEST_ASSERT_FALSE(client_is_authed(31)); /* gone: untracked reads false */
}

/* ════════════════════════════════════════════════════════════════════════
 * 3. Fail-closed paths
 * ════════════════════════════════════════════════════════════════════════ */

/* The client table is fixed at MAX_WS_CLIENTS. An overflow connection is
 * untracked, and an untracked fd MUST read as unauthenticated: the failure
 * mode is loss of privilege, never accidental grant. */
void test_client_table_overflow_fails_closed(void) {
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        TEST_ASSERT_EQUAL(ESP_OK, handshake_authed(40 + i));
        TEST_ASSERT_TRUE(client_is_authed(40 + i));
    }
    TEST_ASSERT_EQUAL(MAX_WS_CLIENTS, s_client_count);

    /* One more authenticated client than the table holds. */
    TEST_ASSERT_EQUAL(ESP_OK, handshake_authed(40 + MAX_WS_CLIENTS));
    TEST_ASSERT_EQUAL(MAX_WS_CLIENTS, s_client_count);
    TEST_ASSERT_FALSE(client_is_authed(40 + MAX_WS_CLIENTS));

    TEST_ASSERT_EQUAL(ESP_OK, call_method(40 + MAX_WS_CLIENTS, "bramble.getMessages"));
    TEST_ASSERT_EQUAL(RPC_ERR_UNAUTHORIZED, last_reply_error_code());
}

/* An fd that never handshook at all is unauthenticated. */
void test_untracked_fd_is_unauthenticated(void) {
    TEST_ASSERT_EQUAL(ESP_OK, call_method(99, "bramble.getMessages"));
    TEST_ASSERT_EQUAL(RPC_ERR_UNAUTHORIZED, last_reply_error_code());
}

/* NVS could not provide or persist a token. That is NOT the auth-opt-out
 * state: nothing can match, so nobody gets privilege. */
void test_token_unavailable_fails_closed(void) {
    g_token_rc = -1;
    s_server_running = false;
    ws_server_load_token();

    TEST_ASSERT_TRUE(s_token_unavailable);
    TEST_ASSERT_FALSE(ws_server_auth_disabled());

    /* No credentials: admitted on the allowlist only. */
    TEST_ASSERT_EQUAL(ESP_OK, handshake_unauth(50));
    TEST_ASSERT_FALSE(client_is_authed(50));
    TEST_ASSERT_EQUAL(ESP_OK, call_method(50, "bramble.getMessages"));
    TEST_ASSERT_EQUAL(RPC_ERR_UNAUTHORIZED, last_reply_error_code());

    /* Any credential, including the empty string, is rejected. */
    TEST_ASSERT_EQUAL(ESP_FAIL,
                      handshake(51, "Bearer anything", NULL, "http://" DEVICE_HOST, DEVICE_HOST));
    TEST_ASSERT_EQUAL(HTTPD_WS_TYPE_CLOSE, fake_last_sent_frame()->type);
}

/* The explicit opt-out (empty token in NVS) really does open access, and is
 * distinguishable from the unavailable state above. */
void test_explicit_auth_optout_opens_access_but_keeps_origin_check(void) {
    g_nvs_token[0] = '\0';
    s_server_running = false;
    ws_server_load_token();

    TEST_ASSERT_TRUE(ws_server_auth_disabled());
    TEST_ASSERT_FALSE(s_token_unavailable);

    TEST_ASSERT_EQUAL(ESP_OK, handshake_unauth(53));
    TEST_ASSERT_TRUE(client_is_authed(53));
    TEST_ASSERT_EQUAL(ESP_OK, call_method(53, "bramble.getMessages"));
    TEST_ASSERT_EQUAL(0, last_reply_error_code());

    /* Origin is the only remaining CSWSH defense on such a device, so it
     * must still apply even though every admitted client is privileged. */
    TEST_ASSERT_EQUAL(ESP_FAIL, handshake(54, NULL, NULL, "https://evil.example", DEVICE_HOST));
    TEST_ASSERT_EQUAL(HTTPD_WS_TYPE_CLOSE, fake_last_sent_frame()->type);
}

/* ════════════════════════════════════════════════════════════════════════
 * 4. Framing and connection lifecycle
 * ════════════════════════════════════════════════════════════════════════ */

/* WS_BUF_SIZE bounds the receive buffer. An oversize frame is dropped
 * before allocation and before dispatch. */
void test_oversize_frame_is_dropped_without_dispatch(void) {
    TEST_ASSERT_EQUAL(ESP_OK, handshake_authed(60));

    httpd_req_t r = make_req(HTTP_POST, 60);
    r.fake_rx_type = HTTPD_WS_TYPE_TEXT;
    r.fake_rx_len = WS_BUF_SIZE; /* one over the WS_BUF_SIZE - 1 limit */
    r.fake_rx_payload = NULL;

    TEST_ASSERT_EQUAL(ESP_FAIL, ws_handler(&r));
    TEST_ASSERT_EQUAL(0, fake_sent_frame_count());
}

/* The largest frame that still fits must be accepted, so the bound is not
 * off by one in the strict direction. */
void test_largest_accepted_frame_is_dispatched(void) {
    TEST_ASSERT_EQUAL(ESP_OK, handshake_authed(61));

    char big[WS_BUF_SIZE];
    int n = snprintf(big, sizeof(big),
                     "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"bramble.getMessages\",\"params\":"
                     "{\"pad\":\"");
    memset(big + n, 'x', WS_BUF_SIZE - 1 - n - 3);
    memcpy(big + WS_BUF_SIZE - 1 - 3, "\"}}", 3);

    TEST_ASSERT_EQUAL(ESP_OK, deliver_frame(61, HTTPD_WS_TYPE_TEXT, big, WS_BUF_SIZE - 1));
    TEST_ASSERT_EQUAL(1, fake_sent_frame_count());
    TEST_ASSERT_EQUAL(0, last_reply_error_code());
}

void test_empty_text_frame_is_ignored(void) {
    TEST_ASSERT_EQUAL(ESP_OK, handshake_authed(62));
    TEST_ASSERT_EQUAL(ESP_OK, deliver_frame(62, HTTPD_WS_TYPE_TEXT, NULL, 0));
    TEST_ASSERT_EQUAL(0, fake_sent_frame_count());
}

void test_ping_is_answered_with_a_pong_echoing_the_payload(void) {
    TEST_ASSERT_EQUAL(ESP_OK, handshake_authed(63));
    const char* body = "keepalive";
    TEST_ASSERT_EQUAL(ESP_OK, deliver_frame(63, HTTPD_WS_TYPE_PING, body, strlen(body)));

    const fake_sent_frame_t* f = fake_last_sent_frame();
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL(HTTPD_WS_TYPE_PONG, f->type);
    TEST_ASSERT_EQUAL(strlen(body), f->len);
    TEST_ASSERT_EQUAL_STRING(body, (const char*)f->payload);
}

void test_pong_and_binary_frames_are_ignored_without_dispatch(void) {
    TEST_ASSERT_EQUAL(ESP_OK, handshake_authed(64));

    TEST_ASSERT_EQUAL(ESP_OK, deliver_frame(64, HTTPD_WS_TYPE_PONG, "x", 1));
    TEST_ASSERT_EQUAL(0, fake_sent_frame_count());

    const char* json = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"bramble.getMessages\"}";
    TEST_ASSERT_EQUAL(ESP_OK, deliver_frame(64, HTTPD_WS_TYPE_BINARY, json, strlen(json)));
    TEST_ASSERT_EQUAL(0, fake_sent_frame_count());

    /* Ignoring them must not have disturbed the connection. */
    TEST_ASSERT_TRUE(client_is_authed(64));
    TEST_ASSERT_EQUAL(1, s_client_count);
}

/* A malformed payload is a dispatcher-level parse error, not a dropped
 * connection: the client gets an answer and stays tracked. */
void test_malformed_json_gets_a_parse_error_and_keeps_the_connection(void) {
    TEST_ASSERT_EQUAL(ESP_OK, handshake_authed(65));
    const char* junk = "{not json";
    TEST_ASSERT_EQUAL(ESP_OK, deliver_frame(65, HTTPD_WS_TYPE_TEXT, junk, strlen(junk)));

    TEST_ASSERT_EQUAL(RPC_ERR_PARSE, last_reply_error_code());
    TEST_ASSERT_EQUAL(1, s_client_count);
    TEST_ASSERT_TRUE(client_is_authed(65));
}

/* A failed length probe aborts the frame without touching the table. */
void test_recv_probe_failure_is_propagated(void) {
    TEST_ASSERT_EQUAL(ESP_OK, handshake_authed(66));

    httpd_req_t r = make_req(HTTP_POST, 66);
    r.fake_rx_type = HTTPD_WS_TYPE_TEXT;
    r.fake_rx_len = 10;
    r.fake_rx_probe_err = ESP_FAIL;

    TEST_ASSERT_EQUAL(ESP_FAIL, ws_handler(&r));
    TEST_ASSERT_EQUAL(0, fake_sent_frame_count());
    TEST_ASSERT_EQUAL(1, s_client_count);
}

/* A failed payload read must free the buffer it already allocated. ASan is
 * the real assertion here. */
void test_recv_payload_failure_does_not_leak(void) {
    TEST_ASSERT_EQUAL(ESP_OK, handshake_authed(67));

    httpd_req_t r = make_req(HTTP_POST, 67);
    r.fake_rx_type = HTTPD_WS_TYPE_TEXT;
    r.fake_rx_len = 64;
    r.fake_rx_read_err = ESP_FAIL;

    TEST_ASSERT_EQUAL(ESP_FAIL, ws_handler(&r));
}

void test_close_frame_removes_the_client(void) {
    TEST_ASSERT_EQUAL(ESP_OK, handshake_authed(68));
    TEST_ASSERT_EQUAL(1, s_client_count);

    TEST_ASSERT_EQUAL(ESP_OK, deliver_frame(68, HTTPD_WS_TYPE_CLOSE, NULL, 0));
    TEST_ASSERT_EQUAL(0, s_client_count);
}

/* If the reply cannot be written the peer is gone, so the entry must be
 * reclaimed rather than left to accumulate against MAX_WS_CLIENTS. */
void test_failed_reply_send_reclaims_the_client_slot(void) {
    TEST_ASSERT_EQUAL(ESP_OK, handshake_authed(69));
    TEST_ASSERT_EQUAL(1, s_client_count);

    fake_httpd_fail_sends(1);
    TEST_ASSERT_EQUAL(ESP_OK, call_method(69, "bramble.getMessages"));
    TEST_ASSERT_EQUAL(0, s_client_count);
}

/* httpd calls close_fn for clean closes and for LRU purges alike; both must
 * drop the tracking entry. */
void test_socket_close_callback_removes_the_client(void) {
    int pipefd[2];
    TEST_ASSERT_EQUAL(0, pipe(pipefd));

    client_add(pipefd[0], true);
    TEST_ASSERT_EQUAL(1, s_client_count);

    ws_close_fn(s_server, pipefd[0]); /* closes the fd itself */
    TEST_ASSERT_EQUAL(0, s_client_count);

    close(pipefd[1]);
}

void test_server_start_is_idempotent(void) {
    /* setUp already started it once. */
    TEST_ASSERT_EQUAL(1, fake_httpd_start_count());
    TEST_ASSERT_TRUE(ws_server_is_running());

    TEST_ASSERT_EQUAL(0, ws_server_start());
    TEST_ASSERT_EQUAL(1, fake_httpd_start_count());

    ws_server_stop();
    TEST_ASSERT_FALSE(ws_server_is_running());
    TEST_ASSERT_EQUAL(1, fake_httpd_stop_count());
    TEST_ASSERT_EQUAL(0, s_client_count);
}

/* The httpd task runs rpc_dispatch, and trust-anchor RPCs Ed25519-verify on
 * it. 4096 overflowed and rebooted the node; 8192 is the fix. Pin it. */
void test_server_config_keeps_the_enlarged_httpd_stack(void) {
    const httpd_config_t* cfg = fake_httpd_last_config();
    TEST_ASSERT_EQUAL(8192, cfg->stack_size);
    TEST_ASSERT_TRUE(cfg->lru_purge_enable);
    TEST_ASSERT_NOT_NULL(cfg->close_fn);
    TEST_ASSERT_EQUAL(MAX_WS_CLIENTS + 2, cfg->max_open_sockets);
}

/* ════════════════════════════════════════════════════════════════════════
 * 5. Notification fan-out and backpressure
 * ════════════════════════════════════════════════════════════════════════ */

static void notify(const char* json) { ws_notify_cb(json, strlen(json), NULL); }

/* Notifications carry decrypted content. They go only to authenticated
 * connections, never to a pairing-allowlist one. */
void test_notifications_reach_only_authenticated_clients(void) {
    TEST_ASSERT_EQUAL(ESP_OK, handshake_authed(70));
    TEST_ASSERT_EQUAL(ESP_OK, handshake_unauth(71));

    notify("{\"method\":\"bramble.onMessage\"}");

    TEST_ASSERT_EQUAL(1, fake_sent_frame_count());
    TEST_ASSERT_EQUAL(70, fake_sent_frame(0)->sockfd);
    TEST_ASSERT_TRUE(fake_sent_frame(0)->async);
}

void test_no_notification_when_only_unauthenticated_clients_are_connected(void) {
    TEST_ASSERT_EQUAL(ESP_OK, handshake_unauth(72));
    notify("{\"method\":\"bramble.onMessage\"}");
    TEST_ASSERT_EQUAL(0, fake_sent_frame_count());
}

/* On an auth-opt-out device every admitted connection is authorized. */
void test_auth_optout_delivers_notifications_to_every_client(void) {
    g_nvs_token[0] = '\0';
    s_server_running = false;
    ws_server_load_token();

    TEST_ASSERT_EQUAL(ESP_OK, handshake_unauth(73));
    TEST_ASSERT_EQUAL(ESP_OK, handshake_unauth(74));

    notify("{\"method\":\"bramble.onMessage\"}");
    TEST_ASSERT_EQUAL(2, fake_sent_frame_count());
}

/* Sustained mesh traffic can generate 50+ notifications/sec, which
 * overwhelmed the Wi-Fi TX queue and caused AP disassociation. The throttle
 * allows a burst then clamps. */
void test_notify_burst_is_capped_then_throttled(void) {
    TEST_ASSERT_EQUAL(ESP_OK, handshake_authed(75));

    for (int i = 0; i < WS_NOTIFY_BURST_MAX + 5; i++) {
        notify("{\"method\":\"bramble.onMessage\"}");
    }
    TEST_ASSERT_EQUAL(WS_NOTIFY_BURST_MAX, fake_sent_frame_count());
    TEST_ASSERT_GREATER_THAN(0, s_notify_drops);
}

/* Once the rate window has passed the allowance is restored, so throttling
 * is transient and not a permanent mute. */
void test_notify_allowance_recovers_after_the_rate_window(void) {
    TEST_ASSERT_EQUAL(ESP_OK, handshake_authed(76));

    for (int i = 0; i < WS_NOTIFY_BURST_MAX + 5; i++) {
        notify("{\"method\":\"bramble.onMessage\"}");
    }
    TEST_ASSERT_EQUAL(WS_NOTIFY_BURST_MAX, fake_sent_frame_count());

    g_now_us += WS_NOTIFY_MIN_INTERVAL_MS * 1000ULL;
    notify("{\"method\":\"bramble.onMessage\"}");
    TEST_ASSERT_EQUAL(WS_NOTIFY_BURST_MAX + 1, fake_sent_frame_count());
}

/* A notification that cannot be delivered means the peer is gone. */
void test_failed_notify_send_reclaims_the_client_slot(void) {
    TEST_ASSERT_EQUAL(ESP_OK, handshake_authed(77));
    fake_httpd_fail_sends(1);

    notify("{\"method\":\"bramble.onMessage\"}");
    TEST_ASSERT_EQUAL(0, s_client_count);
}

void test_notify_with_no_clients_is_a_noop(void) {
    notify("{\"method\":\"bramble.onMessage\"}");
    TEST_ASSERT_EQUAL(0, fake_sent_frame_count());
}

/* ════════════════════════════════════════════════════════════════════════
 * 6. The /config setup portal gate
 * ════════════════════════════════════════════════════════════════════════ */

static esp_err_t config_post(const char* body, const char* origin, const char* referer,
                             const char* authz) {
    httpd_req_t r = make_req(HTTP_POST, 80);
    r.fake_body = body;
    if (origin)
        fake_req_add_header(&r, "Origin", origin);
    if (referer)
        fake_req_add_header(&r, "Referer", referer);
    if (authz)
        fake_req_add_header(&r, "Authorization", authz);
    fake_req_add_header(&r, "Host", DEVICE_HOST);
    return config_post_handler(&r);
}

/* The setup form cannot send headers, so the token rides as a form field.
 * It is the same credential and must work. */
void test_config_post_accepts_the_token_as_a_form_field(void) {
    TEST_ASSERT_EQUAL(ESP_OK,
                      config_post("ssid=Home&pass=hunter2&token=" GOOD_TOKEN, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL(1, g_reboot_calls);
}

/* Without a token the request is unauthorized, even same-origin. */
void test_config_post_without_credentials_is_unauthorized(void) {
    TEST_ASSERT_EQUAL(ESP_FAIL,
                      config_post("ssid=Home&pass=hunter2", "http://" DEVICE_HOST, NULL, NULL));
    TEST_ASSERT_EQUAL(0, g_reboot_calls);
    TEST_ASSERT_EQUAL_STRING("401 Unauthorized", fake_last_http_resp()->status);
}

/* A wrong form token must not be accepted. */
void test_config_post_with_a_wrong_form_token_is_unauthorized(void) {
    TEST_ASSERT_EQUAL(ESP_FAIL,
                      config_post("ssid=Home&token=nope", "http://" DEVICE_HOST, NULL, NULL));
    TEST_ASSERT_EQUAL(0, g_reboot_calls);
}

/* CSRF: /config is a CORS-simple form target, so a foreign page could
 * auto-submit Wi-Fi credentials and reboot the device onto an attacker AP.
 * Cross-origin posts are refused before the token is even consulted. */
void test_config_post_from_a_foreign_origin_is_forbidden(void) {
    TEST_ASSERT_EQUAL(ESP_FAIL,
                      config_post("ssid=Evil&pass=x", "https://evil.example", NULL, NULL));
    TEST_ASSERT_EQUAL(0, g_reboot_calls);
    TEST_ASSERT_TRUE(fake_last_http_resp()->is_err);
    TEST_ASSERT_EQUAL(HTTPD_403_FORBIDDEN, fake_last_http_resp()->err_code);
}

/* A token-bearing client skips the CSRF gate, same rationale as the WS
 * upgrade: a cross-site page cannot read the token. */
void test_config_post_with_a_valid_token_skips_the_csrf_gate(void) {
    TEST_ASSERT_EQUAL(ESP_OK, config_post("ssid=Home&pass=x", "https://evil.example", NULL,
                                          "Bearer " GOOD_TOKEN));
    TEST_ASSERT_EQUAL(1, g_reboot_calls);
}

/* Form values are URL-decoded, and an empty SSID is a bad request rather
 * than a reboot into nothing. */
void test_config_post_requires_an_ssid(void) {
    TEST_ASSERT_EQUAL(ESP_FAIL, config_post("pass=x&token=" GOOD_TOKEN, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL(0, g_reboot_calls);
    TEST_ASSERT_TRUE(fake_last_http_resp()->is_err);
    TEST_ASSERT_EQUAL(HTTPD_400_BAD_REQUEST, fake_last_http_resp()->err_code);
}

void test_config_post_with_an_empty_body_is_a_bad_request(void) {
    TEST_ASSERT_EQUAL(ESP_FAIL, config_post(NULL, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL(0, g_reboot_calls);
}

/* ── Extra origins from NVS ──────────────────────────────────────────── */

/* A hosted webapp origin enrolled via authenticated RPC is honoured on an
 * otherwise credential-free upgrade. */
void test_configured_extra_origin_is_allowed_without_a_token(void) {
    snprintf(g_nvs_origins, sizeof(g_nvs_origins), "https://app.example.com");
    ws_server_load_origins();
    TEST_ASSERT_EQUAL_STRING("https://app.example.com", ws_server_get_extra_origins());

    TEST_ASSERT_EQUAL(ESP_OK, handshake(85, NULL, NULL, "https://app.example.com", DEVICE_HOST));
    TEST_ASSERT_EQUAL(1, s_client_count);
    TEST_ASSERT_FALSE(client_is_authed(85)); /* allowed in, still unprivileged */
}

int main(void) {
    UNITY_BEGIN();

    /* 1. Auth enforcement */
    RUN_TEST(test_unauth_connection_cannot_invoke_privileged_method);
    RUN_TEST(test_unauth_connection_can_invoke_allowlisted_method);
    RUN_TEST(test_unauth_connection_cannot_enumerate_methods);
    RUN_TEST(test_authed_connection_reaches_privileged_methods);
    RUN_TEST(test_subprotocol_token_authenticates);
    RUN_TEST(test_bad_credentials_are_rejected_with_a_1008_close_frame);
    RUN_TEST(test_valid_token_skips_the_origin_check);
    RUN_TEST(test_foreign_origin_without_token_is_rejected);
    RUN_TEST(test_oversized_origin_is_rejected);

    /* 2. Per-connection auth state isolation */
    RUN_TEST(test_auth_state_does_not_leak_between_connections);
    RUN_TEST(test_closed_authed_fd_does_not_leak_privilege_to_its_reuse);
    RUN_TEST(test_rehandshake_overwrites_stale_auth_flag);
    RUN_TEST(test_client_removal_preserves_other_entries_auth_state);

    /* 3. Fail-closed paths */
    RUN_TEST(test_client_table_overflow_fails_closed);
    RUN_TEST(test_untracked_fd_is_unauthenticated);
    RUN_TEST(test_token_unavailable_fails_closed);
    RUN_TEST(test_explicit_auth_optout_opens_access_but_keeps_origin_check);

    /* 4. Framing and lifecycle */
    RUN_TEST(test_oversize_frame_is_dropped_without_dispatch);
    RUN_TEST(test_largest_accepted_frame_is_dispatched);
    RUN_TEST(test_empty_text_frame_is_ignored);
    RUN_TEST(test_ping_is_answered_with_a_pong_echoing_the_payload);
    RUN_TEST(test_pong_and_binary_frames_are_ignored_without_dispatch);
    RUN_TEST(test_malformed_json_gets_a_parse_error_and_keeps_the_connection);
    RUN_TEST(test_recv_probe_failure_is_propagated);
    RUN_TEST(test_recv_payload_failure_does_not_leak);
    RUN_TEST(test_close_frame_removes_the_client);
    RUN_TEST(test_failed_reply_send_reclaims_the_client_slot);
    RUN_TEST(test_socket_close_callback_removes_the_client);
    RUN_TEST(test_server_start_is_idempotent);
    RUN_TEST(test_server_config_keeps_the_enlarged_httpd_stack);

    /* 5. Notifications and backpressure */
    RUN_TEST(test_notifications_reach_only_authenticated_clients);
    RUN_TEST(test_no_notification_when_only_unauthenticated_clients_are_connected);
    RUN_TEST(test_auth_optout_delivers_notifications_to_every_client);
    RUN_TEST(test_notify_burst_is_capped_then_throttled);
    RUN_TEST(test_notify_allowance_recovers_after_the_rate_window);
    RUN_TEST(test_failed_notify_send_reclaims_the_client_slot);
    RUN_TEST(test_notify_with_no_clients_is_a_noop);

    /* 6. /config portal gate */
    RUN_TEST(test_config_post_accepts_the_token_as_a_form_field);
    RUN_TEST(test_config_post_without_credentials_is_unauthorized);
    RUN_TEST(test_config_post_with_a_wrong_form_token_is_unauthorized);
    RUN_TEST(test_config_post_from_a_foreign_origin_is_forbidden);
    RUN_TEST(test_config_post_with_a_valid_token_skips_the_csrf_gate);
    RUN_TEST(test_config_post_requires_an_ssid);
    RUN_TEST(test_config_post_with_an_empty_body_is_a_bad_request);
    RUN_TEST(test_configured_extra_origin_is_allowed_without_a_token);

    return UNITY_END();
}
