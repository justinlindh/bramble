/* Host fake for esp_http_server. See esp_http_server.h for the contract. */

#include "esp_http_server.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>

struct fake_httpd {
    int marker;
};

static struct fake_httpd s_instance = {.marker = 0xB0};

static fake_sent_frame_t s_sent[FAKE_MAX_SENT];
static int s_sent_count;
static fake_http_resp_t s_resp[8];
static int s_resp_count;
static int s_send_fail_remaining;
static int s_start_count;
static int s_stop_count;
static int s_uri_count;
static httpd_config_t s_last_config;

void fake_httpd_reset(void) {
    memset(s_sent, 0, sizeof(s_sent));
    s_sent_count = 0;
    memset(s_resp, 0, sizeof(s_resp));
    s_resp_count = 0;
    s_send_fail_remaining = 0;
    s_start_count = 0;
    s_stop_count = 0;
    s_uri_count = 0;
    memset(&s_last_config, 0, sizeof(s_last_config));
}

void fake_req_add_header(httpd_req_t* r, const char* name, const char* value) {
    if (r->fake_header_count >= FAKE_MAX_HEADERS)
        return;
    fake_header_t* h = &r->fake_headers[r->fake_header_count++];
    snprintf(h->name, sizeof(h->name), "%s", name);
    snprintf(h->value, sizeof(h->value), "%s", value);
}

int fake_sent_frame_count(void) { return s_sent_count; }

const fake_sent_frame_t* fake_sent_frame(int index) {
    if (index < 0 || index >= s_sent_count)
        return NULL;
    return &s_sent[index];
}

const fake_sent_frame_t* fake_last_sent_frame(void) {
    return s_sent_count > 0 ? &s_sent[s_sent_count - 1] : NULL;
}

int fake_http_resp_count(void) { return s_resp_count; }

const fake_http_resp_t* fake_last_http_resp(void) {
    return s_resp_count > 0 ? &s_resp[s_resp_count - 1] : NULL;
}

void fake_httpd_fail_sends(int n) { s_send_fail_remaining = n; }

int fake_httpd_start_count(void) { return s_start_count; }
int fake_httpd_stop_count(void) { return s_stop_count; }
int fake_httpd_registered_uri_count(void) { return s_uri_count; }
const httpd_config_t* fake_httpd_last_config(void) { return &s_last_config; }

/* ── Lifecycle ───────────────────────────────────────────────────────── */

esp_err_t httpd_start(httpd_handle_t* handle, const httpd_config_t* config) {
    s_start_count++;
    s_last_config = *config;
    if (handle)
        *handle = &s_instance;
    return ESP_OK;
}

esp_err_t httpd_stop(httpd_handle_t handle) {
    (void)handle;
    s_stop_count++;
    return ESP_OK;
}

esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t* uri) {
    (void)handle;
    (void)uri;
    s_uri_count++;
    return ESP_OK;
}

/* ── Request accessors ───────────────────────────────────────────────── */

int httpd_req_to_sockfd(httpd_req_t* r) { return r->fake_sockfd; }

esp_err_t httpd_req_get_hdr_value_str(httpd_req_t* r, const char* field, char* val, size_t len) {
    for (int i = 0; i < r->fake_header_count; i++) {
        if (strcasecmp(r->fake_headers[i].name, field) != 0)
            continue;
        size_t vlen = strlen(r->fake_headers[i].value);
        /* IDF needs room for the NUL, and truncates rather than writing. */
        if (vlen + 1 > len)
            return ESP_ERR_HTTPD_RESULT_TRUNC;
        memcpy(val, r->fake_headers[i].value, vlen + 1);
        return ESP_OK;
    }
    return ESP_ERR_HTTPD_NOT_FOUND;
}

int httpd_req_recv(httpd_req_t* r, char* buf, size_t buf_len) {
    if (!r->fake_body)
        return 0;
    size_t n = strlen(r->fake_body);
    if (n > buf_len)
        n = buf_len;
    memcpy(buf, r->fake_body, n);
    return (int)n;
}

/* ── Plain HTTP responses ────────────────────────────────────────────── */

static fake_http_resp_t* resp_slot(void) {
    if (s_resp_count >= (int)(sizeof(s_resp) / sizeof(s_resp[0])))
        return &s_resp[s_resp_count - 1];
    return &s_resp[s_resp_count];
}

esp_err_t httpd_resp_set_status(httpd_req_t* r, const char* status) {
    (void)r;
    snprintf(resp_slot()->status, sizeof(resp_slot()->status), "%s", status);
    return ESP_OK;
}

esp_err_t httpd_resp_set_type(httpd_req_t* r, const char* type) {
    (void)r;
    (void)type;
    return ESP_OK;
}

esp_err_t httpd_resp_send(httpd_req_t* r, const char* buf, ssize_t buf_len) {
    (void)r;
    fake_http_resp_t* slot = resp_slot();
    size_t n = buf_len < 0 ? strlen(buf) : (size_t)buf_len;
    if (n > sizeof(slot->body) - 1)
        n = sizeof(slot->body) - 1;
    memcpy(slot->body, buf, n);
    slot->body[n] = '\0';
    slot->is_err = false;
    if (s_resp_count < (int)(sizeof(s_resp) / sizeof(s_resp[0])))
        s_resp_count++;
    return ESP_OK;
}

esp_err_t httpd_resp_send_err(httpd_req_t* r, httpd_err_code_t error, const char* msg) {
    (void)r;
    fake_http_resp_t* slot = resp_slot();
    slot->is_err = true;
    slot->err_code = error;
    snprintf(slot->body, sizeof(slot->body), "%s", msg ? msg : "");
    if (s_resp_count < (int)(sizeof(s_resp) / sizeof(s_resp[0])))
        s_resp_count++;
    return ESP_OK;
}

/* ── WebSocket frames ────────────────────────────────────────────────── */

esp_err_t httpd_ws_recv_frame(httpd_req_t* req, httpd_ws_frame_t* pkt, size_t max_len) {
    if (max_len == 0) {
        if (req->fake_rx_probe_err != ESP_OK)
            return req->fake_rx_probe_err;
        pkt->type = req->fake_rx_type;
        pkt->len = req->fake_rx_len;
        pkt->final = true;
        return ESP_OK;
    }
    if (req->fake_rx_read_err != ESP_OK)
        return req->fake_rx_read_err;
    size_t n = req->fake_rx_len;
    if (n > max_len)
        n = max_len;
    if (n > 0 && req->fake_rx_payload)
        memcpy(pkt->payload, req->fake_rx_payload, n);
    pkt->len = n;
    pkt->type = req->fake_rx_type;
    pkt->final = true;
    return ESP_OK;
}

static esp_err_t capture_send(httpd_ws_frame_t* pkt, int sockfd, bool async) {
    if (s_send_fail_remaining > 0) {
        s_send_fail_remaining--;
        return ESP_FAIL;
    }
    if (s_sent_count >= FAKE_MAX_SENT)
        return ESP_OK; /* silently drop past capacity; tests assert on counts */
    fake_sent_frame_t* f = &s_sent[s_sent_count++];
    f->type = pkt->type;
    f->sockfd = sockfd;
    f->async = async;
    size_t n = pkt->len;
    if (n > FAKE_SENT_PAYLOAD_MAX - 1)
        n = FAKE_SENT_PAYLOAD_MAX - 1;
    if (n > 0 && pkt->payload)
        memcpy(f->payload, pkt->payload, n);
    f->payload[n] = '\0';
    f->len = pkt->len;
    return ESP_OK;
}

esp_err_t httpd_ws_send_frame(httpd_req_t* req, httpd_ws_frame_t* pkt) {
    return capture_send(pkt, req->fake_sockfd, false);
}

esp_err_t httpd_ws_send_frame_async(httpd_handle_t hd, int fd, httpd_ws_frame_t* pkt) {
    (void)hd;
    return capture_send(pkt, fd, true);
}
