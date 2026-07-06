#include "ws_server.h"
#include "ct_strcmp.h"
#include "ws_origin.h"
#include "ws_auth_credential.h"
#include "rpc_dispatcher.h"
#include "rpc_auth.h"
#include "wifi_manager.h"
#include "mesh_task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>
#include "cJSON.h"
#include "sdkconfig.h"
#include "identity.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "nvs_keys.h"
#include "freertos/FreeRTOS.h"

#define MAX_WS_CLIENTS 4
#define WS_BUF_SIZE 2048
#define AUTH_TOKEN_MAX 128

static const char* TAG = "ws";
static httpd_handle_t s_server = NULL;
typedef struct {
    int fd;
    bool authed;
} ws_client_t;
static ws_client_t s_clients[MAX_WS_CLIENTS];
static int s_client_count = 0;
static portMUX_TYPE s_client_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_server_running = false;
static char s_auth_token[AUTH_TOKEN_MAX] = {0};
/* True when NVS could not provide or persist a token. Fail CLOSED: full
 * RPC access is impossible until the token store recovers; only the
 * unauthenticated pairing allowlist is served. */
static bool s_token_unavailable = false;
/* Comma-separated extra allowed origins (NVS-backed, set via the
 * authenticated bramble.setAllowedOrigins RPC). */
#define WS_ORIGINS_MAX 256
static char s_extra_origins[WS_ORIGINS_MAX] = {0};

/* ── Client tracking ─────────────────────────────────────────────────── */

static void client_add(int fd, bool authed) {
    bool added = false;
    bool full = false;
    int total = 0;

    taskENTER_CRITICAL(&s_client_lock);
    for (int i = 0; i < s_client_count; i++) {
        if (s_clients[i].fd == fd) {
            s_clients[i].authed = authed;
            total = s_client_count;
            taskEXIT_CRITICAL(&s_client_lock);
            return; /* already tracked */
        }
    }
    if (s_client_count < MAX_WS_CLIENTS) {
        s_clients[s_client_count].fd = fd;
        s_clients[s_client_count].authed = authed;
        s_client_count++;
        added = true;
        total = s_client_count;
    } else {
        full = true;
    }
    taskEXIT_CRITICAL(&s_client_lock);

    if (added) {
        ESP_LOGI(TAG, "WS client connected fd=%d authed=%d (total=%d)", fd, (int)authed, total);
    } else if (full) {
        /* Untracked clients are treated as unauthenticated (fail closed)
         * by client_is_authed() below. */
        ESP_LOGW(TAG, "WS client table full, dropping fd=%d", fd);
    }
}

/* Untracked fds report unauthenticated: fail closed. */
static bool client_is_authed(int fd) {
    bool authed = false;
    taskENTER_CRITICAL(&s_client_lock);
    for (int i = 0; i < s_client_count; i++) {
        if (s_clients[i].fd == fd) {
            authed = s_clients[i].authed;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_client_lock);
    return authed;
}

static void client_remove(int fd) {
    bool removed = false;
    int total = 0;

    taskENTER_CRITICAL(&s_client_lock);
    for (int i = 0; i < s_client_count; i++) {
        if (s_clients[i].fd == fd) {
            s_clients[i] = s_clients[--s_client_count];
            removed = true;
            total = s_client_count;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_client_lock);

    if (removed) {
        ESP_LOGI(TAG, "WS client removed fd=%d (total=%d)", fd, total);
    }
}

/* ── Auth check ──────────────────────────────────────────────────────── */

/* Token comparison delegates to ct_strcmp (fixed-bound, length-independent
 * constant-time compare; see main/ct_strcmp.h). */

typedef enum {
    WS_AUTH_OK = 0,   /* valid credentials, or auth explicitly disabled */
    WS_AUTH_NO_CREDS, /* no credentials supplied */
    WS_AUTH_BAD,      /* credentials supplied and wrong */
} ws_auth_result_t;

static ws_auth_result_t auth_eval(httpd_req_t* req) {
    /* Explicit opt-out (auth_off in NVS) loads as an empty token. The
     * token-unavailable failure mode does NOT take this branch: that state
     * fails closed below. */
    if (!s_token_unavailable && s_auth_token[0] == '\0')
        return WS_AUTH_OK;

    /* Header-based credentials only. The ?token= query-string path was removed
     * (NEW-SEC-6): URLs leak via logs, proxies, and history. Browsers use the
     * Sec-WebSocket-Protocol subprotocol offer instead. */
    char authz[AUTH_TOKEN_MAX + 16] = {0};
    char subproto[AUTH_TOKEN_MAX + 32] = {0};
    (void)httpd_req_get_hdr_value_str(req, "Authorization", authz, sizeof(authz));
    (void)httpd_req_get_hdr_value_str(req, "Sec-WebSocket-Protocol", subproto, sizeof(subproto));

    char token[AUTH_TOKEN_MAX] = {0};
    if (!ws_auth_extract_token(authz[0] ? authz : NULL, subproto[0] ? subproto : NULL, token,
                               sizeof(token))) {
        return WS_AUTH_NO_CREDS;
    }

    bool ok = !s_token_unavailable && ct_strcmp(token, s_auth_token) == 0;
    return ok ? WS_AUTH_OK : WS_AUTH_BAD;
}

static esp_err_t send_401_http(httpd_req_t* req) {
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    const char* body = "{\"error\":\"unauthorized\",\"message\":\"Valid token required\"}";
    httpd_resp_send(req, body, strlen(body));
    return ESP_FAIL;
}

/**
 * Reject an already-upgraded WebSocket connection with a close frame.
 * Must be used instead of send_401_http() inside ws_handler(), because
 * ESP-IDF sends 101 Switching Protocols BEFORE invoking the handler —
 * sending raw HTTP on an upgraded connection breaks the WS protocol.
 */
static esp_err_t send_ws_policy_reject(httpd_req_t* req, const char* reason) {
    /* RFC 6455 §5.5.1: Close frame payload = 2-byte status code + optional reason.
     * 1008 = Policy Violation (§7.4.1). */
    size_t reason_len = strlen(reason);
    uint8_t close_payload[2 + 64];
    close_payload[0] = (uint8_t)(1008 >> 8);   /* status code high byte */
    close_payload[1] = (uint8_t)(1008 & 0xFF); /* status code low byte */
    if (reason_len > sizeof(close_payload) - 2)
        reason_len = sizeof(close_payload) - 2;
    memcpy(close_payload + 2, reason, reason_len);

    httpd_ws_frame_t close_frame = {
        .type = HTTPD_WS_TYPE_CLOSE,
        .payload = close_payload,
        .len = 2 + reason_len,
        .final = true,
    };
    httpd_ws_send_frame(req, &close_frame);
    /* Remove client tracking so we don't try to send notifications to it */
    int fd = httpd_req_to_sockfd(req);
    client_remove(fd);
    return ESP_FAIL;
}

/* ── Notification transport ──────────────────────────────────────────── */

/* Rate-limit outbound WS notifications to avoid saturating the Wi-Fi TX
 * queue on boards with reduced buffer counts (T-Deck Plus: 16 TX buffers).
 * Under sustained mesh traffic, delivery events + GPS + telemetry can
 * generate 50+ notifications/sec, overwhelming the httpd async send path
 * and causing AP disassociation.
 *
 * Strategy: allow bursts up to WS_NOTIFY_BURST_MAX, then enforce a
 * minimum interval of WS_NOTIFY_MIN_INTERVAL_MS between sends.
 * Dropped notifications are counted and logged periodically. */
#define WS_NOTIFY_BURST_MAX 8
#define WS_NOTIFY_MIN_INTERVAL_MS 50
#define WS_NOTIFY_DROP_LOG_INTERVAL_MS 10000

static uint32_t s_notify_burst_count = 0;
static uint64_t s_notify_last_send_us = 0;
static uint32_t s_notify_drops = 0;
static uint64_t s_notify_last_drop_log_us = 0;

static void ws_notify_cb(const char* json, size_t len, void* ctx) {
    int client_fds[MAX_WS_CLIENTS];
    int client_count = 0;

    /* Notifications carry decrypted content; deliver ONLY to
     * authenticated connections (all of them on an auth-opt-out device).
     * Selection logic is the host-tested rpc_auth_notify_filter(). */
    bool open_access = ws_server_auth_disabled();

    taskENTER_CRITICAL(&s_client_lock);
    if (!s_server || s_client_count == 0) {
        taskEXIT_CRITICAL(&s_client_lock);
        return;
    }

    rpc_notify_client_t snapshot[MAX_WS_CLIENTS];
    for (int i = 0; i < s_client_count; i++) {
        snapshot[i].fd = s_clients[i].fd;
        snapshot[i].authenticated = s_clients[i].authed;
    }
    int snapshot_count = s_client_count;
    taskEXIT_CRITICAL(&s_client_lock);

    client_count =
        rpc_auth_notify_filter(snapshot, snapshot_count, open_access, client_fds, MAX_WS_CLIENTS);
    if (client_count == 0) {
        return;
    }

    uint64_t now_us = (uint64_t)esp_timer_get_time();
    uint64_t elapsed_us = now_us - s_notify_last_send_us;

    if (elapsed_us >= (WS_NOTIFY_MIN_INTERVAL_MS * 1000ULL)) {
        /* Enough time has passed — reset burst counter */
        s_notify_burst_count = 0;
    }

    if (s_notify_burst_count >= WS_NOTIFY_BURST_MAX) {
        /* Over burst limit and within the rate window — drop */
        s_notify_drops++;
        if (now_us - s_notify_last_drop_log_us >= (WS_NOTIFY_DROP_LOG_INTERVAL_MS * 1000ULL)) {
            ESP_LOGW(TAG, "WS notify throttled: %" PRIu32 " dropped in last %.1fs", s_notify_drops,
                     (float)(now_us - s_notify_last_drop_log_us) / 1000000.0f);
            s_notify_drops = 0;
            s_notify_last_drop_log_us = now_us;
        }
        return;
    }

    s_notify_burst_count++;
    s_notify_last_send_us = now_us;

    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t*)json,
        .len = len,
        .final = true,
    };

    for (int i = client_count - 1; i >= 0; i--) {
        esp_err_t err = httpd_ws_send_frame_async(s_server, client_fds[i], &frame);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Notify send failed fd=%d err=%d — removing", client_fds[i], err);
            client_remove(client_fds[i]);
        }
    }
}

/* ── WebSocket URI handler ───────────────────────────────────────────── */

static esp_err_t ws_handler(httpd_req_t* req) {
    if (req->method == HTTP_GET) {
        /* Auth + Origin evaluation on WebSocket upgrade.
         * Note: ESP-IDF has already sent 101 Switching Protocols at this
         * point, so rejections use a WS close frame (1008), not the
         * HTTP 403 that would otherwise be correct.
         *
         * Policy (auth required by default):
         *   - wrong credentials: reject
         *   - presented, valid credentials: full access, Origin check
         *     SKIPPED. The token is the stronger credential and a
         *     cross-site attacker page cannot read it, so a token-bearing
         *     cross-origin page is the user's own client, not CSWSH.
         *     Requiring origin enrollment before a token-holding webapp
         *     could ever connect would resurrect the onboarding friction
         *     that led to the old open-by-default regression.
         *   - no credentials (including devices with auth explicitly
         *     disabled): browser Origin allowlist applies (same-origin or
         *     configured extras; see main/ws_origin.h). On the opt-out
         *     device this is the remaining CSWSH defense; otherwise it
         *     keeps foreign pages off even the pairing allowlist. */
        ws_auth_result_t ar = auth_eval(req);
        if (ar == WS_AUTH_BAD) {
            ESP_LOGW(TAG, "WS auth failed (bad credentials), sending close frame");
            return send_ws_policy_reject(req, "unauthorized");
        }

        bool token_proven = (ar == WS_AUTH_OK) && !ws_server_auth_disabled();
        if (!token_proven) {
            char origin[WS_ORIGINS_MAX] = {0};
            char host[96] = {0};
            if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) ==
                ESP_ERR_HTTPD_RESULT_TRUNC) {
                /* Oversized Origin cannot match anything we would allow */
                ESP_LOGW(TAG, "WS Origin header oversized, rejecting");
                return send_ws_policy_reject(req, "origin not allowed");
            }
            (void)httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
            if (!ws_origin_allowed(origin, host, s_extra_origins)) {
                ESP_LOGW(TAG, "WS Origin '%s' not allowed (host '%s'), rejecting", origin, host);
                return send_ws_policy_reject(req, "origin not allowed");
            }
        }

        int fd = httpd_req_to_sockfd(req);
        client_add(fd, ar == WS_AUTH_OK);
        ESP_LOGI(TAG, "WS handshake fd=%d %s", fd,
                 ar == WS_AUTH_OK ? "(authenticated)" : "(unauthenticated, allowlist only)");
        return ESP_OK;
    }

    /* Receive frame */
    httpd_ws_frame_t rx_frame = {0};
    uint8_t* buf = NULL;

    /* First call to get frame length */
    esp_err_t ret = httpd_ws_recv_frame(req, &rx_frame, 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ws_recv_frame (len probe) failed: %d", ret);
        return ret;
    }

    if (rx_frame.len == 0 && rx_frame.type == HTTPD_WS_TYPE_TEXT) {
        return ESP_OK;
    }

    if (rx_frame.len > WS_BUF_SIZE - 1) {
        ESP_LOGW(TAG, "WS frame too large (%zu bytes), dropping", rx_frame.len);
        return ESP_FAIL;
    }

    buf = (uint8_t*)malloc(rx_frame.len + 1);
    if (!buf) {
        ESP_LOGE(TAG, "OOM allocating WS rx buffer");
        return ESP_ERR_NO_MEM;
    }

    rx_frame.payload = buf;
    ret = httpd_ws_recv_frame(req, &rx_frame, rx_frame.len);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ws_recv_frame failed: %d", ret);
        free(buf);
        return ret;
    }
    buf[rx_frame.len] = '\0';

    if (rx_frame.type == HTTPD_WS_TYPE_CLOSE) {
        int fd = httpd_req_to_sockfd(req);
        client_remove(fd);
        free(buf);
        return ESP_OK;
    }

    if (rx_frame.type == HTTPD_WS_TYPE_PING) {
        httpd_ws_frame_t pong = {
            .final = true,
            .fragmented = false,
            .type = HTTPD_WS_TYPE_PONG,
            .payload = rx_frame.payload,
            .len = rx_frame.len,
        };
        ret = httpd_ws_send_frame(req, &pong);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "ws_send_pong failed: %d", ret);
        }
        free(buf);
        return ret;
    }

    if (rx_frame.type == HTTPD_WS_TYPE_PONG) {
        free(buf);
        return ESP_OK;
    }

    if (rx_frame.type != HTTPD_WS_TYPE_TEXT) {
        free(buf);
        return ESP_OK; /* ignore binary/other control frames */
    }

    ESP_LOGD(TAG, "WS RX: %s", (char*)buf);

    /* Dispatch RPC */
    char* resp_buf = (char*)malloc(WS_BUF_SIZE);
    if (!resp_buf) {
        ESP_LOGE(TAG, "OOM allocating WS response buffer");
        free(buf);
        return ESP_ERR_NO_MEM;
    }

    int resp_len = rpc_dispatch_authed((char*)buf, resp_buf, WS_BUF_SIZE,
                                       client_is_authed(httpd_req_to_sockfd(req)));
    free(buf);

    if (resp_len > 0) {
        httpd_ws_frame_t tx_frame = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t*)resp_buf,
            .len = (size_t)resp_len,
            .final = true,
        };
        ret = httpd_ws_send_frame(req, &tx_frame);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "ws_send_frame failed: %d", ret);
            /* If send fails, the client may be gone — remove it */
            int fd = httpd_req_to_sockfd(req);
            client_remove(fd);
        }
    }

    free(resp_buf);
    return ESP_OK;
}

/* ── WiFi config web page ────────────────────────────────────────────── */

static const char* CONFIG_HTML =
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Bramble WiFi Setup</title>"
    "<style>"
    "body{font-family:sans-serif;max-width:400px;margin:40px auto;padding:0 "
    "20px;background:#1a1a1a;color:#e0e0e0}"
    "h1{color:#4ade80;font-size:1.4em}input{width:100%%;padding:8px;margin:4px 0 "
    "12px;box-sizing:border-box;"
    "background:#2a2a2a;color:#e0e0e0;border:1px solid #444;border-radius:4px}"
    "button{background:#4ade80;color:#1a1a1a;border:none;padding:10px 20px;border-radius:4px;"
    "cursor:pointer;font-weight:bold;width:100%%}button:hover{background:#22c55e}"
    ".status{background:#2a2a2a;padding:12px;border-radius:4px;margin-bottom:16px;font-size:0.9em}"
    ".ok{color:#4ade80}.warn{color:#facc15}"
    "</style></head><body>"
    "<h1>&#x1f310; Bramble WiFi Setup</h1>"
    "<div class='status' id='st'>Loading...</div>"
    "<form method='POST' action='/config'>"
    "<label>WiFi Network (SSID)</label>"
    "<input name='ssid' required placeholder='Your WiFi name'>"
    "<label>Password</label>"
    "<input name='pass' type='password' placeholder='WiFi password'>"
    "<label>Device Token</label>"
    "<input name='token' type='password' placeholder='From boot log or: bramble pair'>"
    "<button type='submit'>Save &amp; Reboot</button>"
    "</form>"
    "<script>"
    "fetch('/config/status').then(r=>r.json()).then(d=>{"
    "let s=document.getElementById('st');"
    "let m=d.mode=='Station'?'<span class=ok>Connected</span>':'<span class=warn>AP Mode</span>';"
    "s.innerHTML='Mode: '+m+'<br>SSID: '+(d.ssid||'(none)')+'<br>IP: '+d.ip;"
    "}).catch(()=>{document.getElementById('st').textContent='Status unavailable';})"
    "</script>"
    "</body></html>";

static esp_err_t config_page_handler(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, CONFIG_HTML, strlen(CONFIG_HTML));
    return ESP_OK;
}

static esp_err_t config_status_handler(httpd_req_t* req) {
    wifi_status_t st;
    wifi_manager_get_status(&st);
    const char* mode_str = st.mode == BRAMBLE_WIFI_STATION ? "Station"
                           : st.mode == BRAMBLE_WIFI_AP    ? "AP"
                                                           : "Off";

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "mode", mode_str);
    cJSON_AddStringToObject(root, "ssid", st.ssid);
    cJSON_AddStringToObject(root, "ip", st.ip_addr);
    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON alloc failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    return ESP_OK;
}

/* URL-decode in place. Returns decoded length. */
static int url_decode(char* dst, const char* src, int src_len) {
    int di = 0;
    for (int i = 0; i < src_len; i++) {
        if (src[i] == '+') {
            dst[di++] = ' ';
        } else if (src[i] == '%' && i + 2 < src_len) {
            char hex[3] = {src[i + 1], src[i + 2], 0};
            dst[di++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            dst[di++] = src[i];
        }
    }
    dst[di] = '\0';
    return di;
}

static esp_err_t config_post_handler(httpd_req_t* req) {
    char body[512] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[len] = '\0';

    /* Parse form: ssid=xxx&pass=xxx&token=xxx */
    char ssid[33] = {0};
    char pass[65] = {0};
    char form_token[AUTH_TOKEN_MAX] = {0};

    char* ssid_start = strstr(body, "ssid=");
    char* pass_start = strstr(body, "pass=");
    char* token_start = strstr(body, "token=");

    if (token_start) {
        token_start += 6;
        char* end = strchr(token_start, '&');
        int tlen = end ? (int)(end - token_start) : (int)strlen(token_start);
        if (tlen > (int)sizeof(form_token) - 1)
            tlen = (int)sizeof(form_token) - 1;
        url_decode(form_token, token_start, tlen);
    }

    /* The setup portal form cannot send an Authorization header, so the
     * device token is accepted as a form field too. Same credential,
     * same constant-time comparison. token_proven means real credentials
     * were presented; the auth-opt-out open state does NOT count. */
    bool open_access = ws_server_auth_disabled();
    bool token_proven = !open_access && (auth_eval(req) == WS_AUTH_OK);
    if (!token_proven && form_token[0] != '\0' && !s_token_unavailable &&
        ct_strcmp(form_token, s_auth_token) == 0) {
        token_proven = true;
    }

    if (!token_proven) {
        /* CSRF gate: /config is a CORS-simple form target, so without
         * this a foreign page could auto-submit Wi-Fi credentials and
         * reboot the device onto an attacker AP. Browser-originated
         * posts must be same-origin (or allowlisted); header-free
         * clients (curl) pass and are gated by the token below. This is
         * the remaining defense on auth-opt-out devices. */
        char origin[WS_ORIGINS_MAX] = {0};
        char referer[WS_ORIGINS_MAX] = {0};
        char host[96] = {0};
        if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) ==
            ESP_ERR_HTTPD_RESULT_TRUNC) {
            httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Origin not allowed");
            return ESP_FAIL;
        }
        if (httpd_req_get_hdr_value_str(req, "Referer", referer, sizeof(referer)) ==
            ESP_ERR_HTTPD_RESULT_TRUNC) {
            /* oversized referer cannot be same-origin: treat as foreign */
            httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Origin not allowed");
            return ESP_FAIL;
        }
        (void)httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
        if (!ws_config_post_allowed(origin, referer, host, s_extra_origins)) {
            ESP_LOGW(TAG, "Config POST cross-origin (origin '%s' referer '%s'), rejecting", origin,
                     referer);
            httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Origin not allowed");
            return ESP_FAIL;
        }

        if (!open_access) {
            ESP_LOGW(TAG, "Config POST auth failed");
            return send_401_http(req);
        }
    }

    if (ssid_start) {
        ssid_start += 5;
        char* end = strchr(ssid_start, '&');
        int slen = end ? (int)(end - ssid_start) : (int)strlen(ssid_start);
        if (slen > 32)
            slen = 32;
        url_decode(ssid, ssid_start, slen);
    }

    if (pass_start) {
        pass_start += 5;
        char* end = strchr(pass_start, '&');
        int plen = end ? (int)(end - pass_start) : (int)strlen(pass_start);
        if (plen > 64)
            plen = 64;
        url_decode(pass, pass_start, plen);
    }

    if (ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID required");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "WiFi config received: SSID=%s", ssid);
    wifi_manager_nvs_set_creds(ssid, pass);

    const char* resp =
        "<!DOCTYPE html><html><head>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<style>body{font-family:sans-serif;max-width:400px;margin:40px auto;padding:0 20px;"
        "background:#1a1a1a;color:#e0e0e0;text-align:center}"
        "h1{color:#4ade80}</style></head><body>"
        "<h1>&#x2705; Saved!</h1>"
        "<p>WiFi credentials saved. Rebooting in 3 seconds...</p>"
        "<p>Connect to your WiFi network and find the device at its new IP.</p>"
        "</body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, resp, strlen(resp));

    /* Reboot after a short delay to let the response flush */
    mesh_reboot_delayed(3000);
    return ESP_OK;
}

static const httpd_uri_t config_page_uri = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = config_page_handler,
};

static const httpd_uri_t config_status_uri = {
    .uri = "/config/status",
    .method = HTTP_GET,
    .handler = config_status_handler,
};

static const httpd_uri_t config_post_uri = {
    .uri = "/config",
    .method = HTTP_POST,
    .handler = config_post_handler,
};

/* Called by httpd when any socket is closed (clean or LRU purge) */
static void ws_close_fn(httpd_handle_t hd, int sockfd) {
    client_remove(sockfd);
    close(sockfd);
}

static const httpd_uri_t ws_uri = {
    .uri = "/ws",
    .method = HTTP_GET,
    .handler = ws_handler,
    .is_websocket = true,
    .handle_ws_control_frames = true,
    .supported_subprotocol = "bramble.v1",
};

/* ── Public API ──────────────────────────────────────────────────────── */

void ws_server_load_token(void) {
    int rc = identity_ensure_ws_auth_token(s_auth_token, sizeof(s_auth_token));
    if (rc < 0) {
        /* NVS could not provide or persist a token. Fail CLOSED: no
         * credentials can match, so only the pairing allowlist is
         * reachable until the token store recovers. */
        s_auth_token[0] = '\0';
        s_token_unavailable = true;
        ESP_LOGE(TAG, "Auth token unavailable (NVS error); RPC limited to pairing allowlist");
        return;
    }
    s_token_unavailable = false;
    if (s_auth_token[0] != '\0') {
        ESP_LOGI(TAG, "RPC auth enabled (per-device token)");
    } else {
        ESP_LOGW(TAG, "RPC auth disabled by explicit opt-out (open access)");
    }
}

void ws_server_load_origins(void) {
    s_extra_origins[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NVS_NS_BRAMBLE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = sizeof(s_extra_origins);
    if (nvs_get_str(h, NVS_KEY_WS_ORIGINS, s_extra_origins, &len) != ESP_OK) {
        s_extra_origins[0] = '\0';
    }
    nvs_close(h);
}

const char* ws_server_get_extra_origins(void) { return s_extra_origins; }

const char* ws_server_get_token(void) { return s_auth_token; }

bool ws_server_auth_disabled(void) { return !s_token_unavailable && s_auth_token[0] == '\0'; }

int ws_server_start(void) {
    ws_server_load_token();
    ws_server_load_origins();

    /* Idempotent: no-op if already running */
    if (s_server_running) {
        ESP_LOGD(TAG, "WebSocket server already running");
        return 0;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_open_sockets = MAX_WS_CLIENTS + 2; /* +2 for HTTP clients */
    config.lru_purge_enable = true;               /* close stale connections when slots full */
    config.close_fn = ws_close_fn;                /* clean up tracked client FDs on close */
    /* rpc_dispatch runs on this httpd task. Trust-anchor RPCs
     * (setEndorsement, and getAnchorStatus once anchored) run an Ed25519
     * verify, which overflowed the default 4096-byte httpd stack and rebooted
     * the node (WS closed 1006, no response) - the same class of failure as
     * the DM handshake worker. Serial RPC has a larger console-task stack and
     * was unaffected. 8192 gives the verify headroom. */
    config.stack_size = 8192;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %d", err);
        return -1;
    }

    httpd_register_uri_handler(s_server, &ws_uri);
    httpd_register_uri_handler(s_server, &config_page_uri);
    httpd_register_uri_handler(s_server, &config_status_uri);
    httpd_register_uri_handler(s_server, &config_post_uri);
    ESP_LOGI(TAG, "WebSocket server started on port 80 (/ws + /config)");

    /* Register push notification transport */
    if (rpc_register_notify_transport(ws_notify_cb, NULL) != 0) {
        ESP_LOGW(TAG, "Failed to register WS notify transport (table full?)");
    }

    s_server_running = true;
    return 0;
}

void ws_server_stop(void) {
    if (s_server) {
        httpd_stop(s_server);
        taskENTER_CRITICAL(&s_client_lock);
        s_server = NULL;
        s_client_count = 0;
        taskEXIT_CRITICAL(&s_client_lock);
        s_server_running = false;
        ESP_LOGI(TAG, "WebSocket server stopped");
    }
}

bool ws_server_is_running(void) { return s_server_running; }
