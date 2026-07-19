#ifndef ESP_HTTP_SERVER_H_FAKE
#define ESP_HTTP_SERVER_H_FAKE

/*
 * Host fake for ESP-IDF's esp_http_server, sized to exactly the surface
 * main/ws_server.c uses. It is NOT a WebSocket implementation: it does not
 * mask, fragment, or frame anything on a wire. It records what ws_server.c
 * hands to httpd and replays what a test told it to deliver, so the
 * handler's own decisions (auth gating, frame-type routing, client-table
 * bookkeeping, notification throttling) can be asserted on the host.
 *
 * Anything the fake does not model is called out in test/test_ws_server.c
 * under "Not covered".
 */

#include "esp_stubs.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ESP_ERR_HTTPD_BASE 0x8000
#define ESP_ERR_HTTPD_RESULT_TRUNC (ESP_ERR_HTTPD_BASE + 1)
#define ESP_ERR_HTTPD_NOT_FOUND (ESP_ERR_HTTPD_BASE + 2)
#define ESP_ERR_NO_MEM 0x101

typedef enum {
    HTTP_GET = 1,
    HTTP_POST = 3,
} httpd_method_t;

typedef enum {
    HTTPD_400_BAD_REQUEST = 0,
    HTTPD_403_FORBIDDEN,
    HTTPD_500_INTERNAL_SERVER_ERROR,
} httpd_err_code_t;

typedef enum {
    HTTPD_WS_TYPE_CONTINUE = 0x0,
    HTTPD_WS_TYPE_TEXT = 0x1,
    HTTPD_WS_TYPE_BINARY = 0x2,
    HTTPD_WS_TYPE_CLOSE = 0x8,
    HTTPD_WS_TYPE_PING = 0x9,
    HTTPD_WS_TYPE_PONG = 0xA,
} httpd_ws_type_t;

typedef struct {
    bool final;
    bool fragmented;
    httpd_ws_type_t type;
    uint8_t* payload;
    size_t len;
} httpd_ws_frame_t;

typedef struct fake_httpd* httpd_handle_t;

#define FAKE_MAX_HEADERS 8
#define FAKE_HDR_NAME_MAX 40
#define FAKE_HDR_VALUE_MAX 512

typedef struct {
    char name[FAKE_HDR_NAME_MAX];
    char value[FAKE_HDR_VALUE_MAX];
} fake_header_t;

/*
 * Only `method` is read by production code. The fake_* members are the
 * test's control surface and have no counterpart in the real struct.
 */
typedef struct {
    httpd_handle_t handle;
    int method;

    int fake_sockfd;
    fake_header_t fake_headers[FAKE_MAX_HEADERS];
    int fake_header_count;

    /* Frame the next httpd_ws_recv_frame() pair should deliver. */
    httpd_ws_type_t fake_rx_type;
    const uint8_t* fake_rx_payload;
    size_t fake_rx_len;
    /* Force the length-probe or the payload read to fail. */
    esp_err_t fake_rx_probe_err;
    esp_err_t fake_rx_read_err;

    /* Body served to httpd_req_recv() (the /config POST path). */
    const char* fake_body;
} httpd_req_t;

typedef esp_err_t (*httpd_uri_handler_t)(httpd_req_t* r);
typedef void (*httpd_close_func_t)(httpd_handle_t hd, int sockfd);

typedef struct {
    const char* uri;
    httpd_method_t method;
    httpd_uri_handler_t handler;
    void* user_ctx;
    bool is_websocket;
    bool handle_ws_control_frames;
    const char* supported_subprotocol;
} httpd_uri_t;

typedef struct {
    uint16_t server_port;
    uint16_t max_open_sockets;
    bool lru_purge_enable;
    httpd_close_func_t close_fn;
    size_t stack_size;
} httpd_config_t;

/* A function rather than the braced-initializer macro the real ESP-IDF header
 * uses. Two clang-format versions disagree about how to lay out a multi-line
 * designated-initializer list inside a macro (local 22.x accepts one form, the
 * CI-pinned version demands another), and the strict format check gates every
 * PR. Plain statements have no such ambiguity. The call site
 * `httpd_config_t config = HTTPD_DEFAULT_CONFIG();` is unaffected. */
static inline httpd_config_t httpd_default_config(void) {
    httpd_config_t cfg;
    cfg.server_port = 80;
    cfg.max_open_sockets = 7;
    cfg.lru_purge_enable = false;
    cfg.close_fn = NULL;
    cfg.stack_size = 4096;
    return cfg;
}

#define HTTPD_DEFAULT_CONFIG() httpd_default_config()

/* ── Production surface ──────────────────────────────────────────────── */

esp_err_t httpd_start(httpd_handle_t* handle, const httpd_config_t* config);
esp_err_t httpd_stop(httpd_handle_t handle);
esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t* uri);

int httpd_req_to_sockfd(httpd_req_t* r);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t* r, const char* field, char* val, size_t len);
int httpd_req_recv(httpd_req_t* r, char* buf, size_t buf_len);

esp_err_t httpd_resp_set_status(httpd_req_t* r, const char* status);
esp_err_t httpd_resp_set_type(httpd_req_t* r, const char* type);
esp_err_t httpd_resp_send(httpd_req_t* r, const char* buf, ssize_t buf_len);
esp_err_t httpd_resp_send_err(httpd_req_t* r, httpd_err_code_t error, const char* msg);

esp_err_t httpd_ws_recv_frame(httpd_req_t* req, httpd_ws_frame_t* pkt, size_t max_len);
esp_err_t httpd_ws_send_frame(httpd_req_t* req, httpd_ws_frame_t* pkt);
esp_err_t httpd_ws_send_frame_async(httpd_handle_t hd, int fd, httpd_ws_frame_t* pkt);

/* ── Test control surface ────────────────────────────────────────────── */

#define FAKE_MAX_SENT 64
#define FAKE_SENT_PAYLOAD_MAX 4096

typedef struct {
    httpd_ws_type_t type;
    int sockfd; /* -1 for the synchronous (in-handler) send path */
    bool async;
    size_t len;
    uint8_t payload[FAKE_SENT_PAYLOAD_MAX];
} fake_sent_frame_t;

typedef struct {
    char status[64];
    char body[1024];
    bool is_err;
    httpd_err_code_t err_code;
} fake_http_resp_t;

/* Reset every captured buffer and every injected failure. */
void fake_httpd_reset(void);

/* Add a request header. Name matching is case-insensitive, as in IDF. */
void fake_req_add_header(httpd_req_t* r, const char* name, const char* value);

/* Captured WebSocket frames, oldest first. */
int fake_sent_frame_count(void);
const fake_sent_frame_t* fake_sent_frame(int index);
const fake_sent_frame_t* fake_last_sent_frame(void);

/* Captured plain-HTTP responses (the /config handlers). */
int fake_http_resp_count(void);
const fake_http_resp_t* fake_last_http_resp(void);

/* Make the next `n` httpd_ws_send_frame*() calls return ESP_FAIL. */
void fake_httpd_fail_sends(int n);

/* httpd_start()/httpd_stop() bookkeeping. */
int fake_httpd_start_count(void);
int fake_httpd_stop_count(void);
int fake_httpd_registered_uri_count(void);
const httpd_config_t* fake_httpd_last_config(void);

#endif /* ESP_HTTP_SERVER_H_FAKE */
