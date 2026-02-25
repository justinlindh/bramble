#include "ws_server.h"
#include "rpc_dispatcher.h"
#include "wifi_manager.h"
#include "mesh_task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "cJSON.h"
#include "sdkconfig.h"

#define MAX_WS_CLIENTS 4
#define WS_BUF_SIZE    2048

static const char *TAG = "ws";
static httpd_handle_t s_server = NULL;
static int s_client_fds[MAX_WS_CLIENTS];
static int s_client_count = 0;
static bool s_server_running = false;

/* ── Client tracking ─────────────────────────────────────────────────── */

static void client_add(int fd)
{
    for (int i = 0; i < s_client_count; i++) {
        if (s_client_fds[i] == fd) return; /* already tracked */
    }
    if (s_client_count < MAX_WS_CLIENTS) {
        s_client_fds[s_client_count++] = fd;
        ESP_LOGI(TAG, "WS client connected fd=%d (total=%d)", fd, s_client_count);
    } else {
        ESP_LOGW(TAG, "WS client table full, dropping fd=%d", fd);
    }
}

static void client_remove(int fd)
{
    for (int i = 0; i < s_client_count; i++) {
        if (s_client_fds[i] == fd) {
            s_client_fds[i] = s_client_fds[--s_client_count];
            ESP_LOGI(TAG, "WS client removed fd=%d (total=%d)", fd, s_client_count);
            return;
        }
    }
}

/* ── Auth check ──────────────────────────────────────────────────────── */

/**
 * Check auth token from query param (?token=X) or Authorization: Bearer header.
 * Returns true if auth passes (token matches or no token configured).
 */
static bool auth_check(httpd_req_t *req)
{
#ifdef CONFIG_BRAMBLE_WS_AUTH_TOKEN
    const char *token = CONFIG_BRAMBLE_WS_AUTH_TOKEN;
    if (token[0] == '\0') return true;  /* no token configured */

    /* Check query parameter */
    size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len > 0) {
        char *query = malloc(query_len + 1);
        if (query && httpd_req_get_url_query_str(req, query, query_len + 1) == ESP_OK) {
            char val[128] = {0};
            if (httpd_query_key_value(query, "token", val, sizeof(val)) == ESP_OK) {
                if (strcmp(val, token) == 0) {
                    free(query);
                    return true;
                }
            }
        }
        free(query);
    }

    /* Check Authorization: Bearer header */
    size_t hdr_len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (hdr_len > 0) {
        char *hdr = malloc(hdr_len + 1);
        if (hdr && httpd_req_get_hdr_value_str(req, "Authorization", hdr, hdr_len + 1) == ESP_OK) {
            const char *prefix = "Bearer ";
            if (strncmp(hdr, prefix, 7) == 0 && strcmp(hdr + 7, token) == 0) {
                free(hdr);
                return true;
            }
        }
        free(hdr);
    }

    return false;
#else
    return true;
#endif
}

static esp_err_t send_401(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    const char *body = "{\"error\":\"unauthorized\",\"message\":\"Valid token required\"}";
    httpd_resp_send(req, body, strlen(body));
    return ESP_FAIL;
}

/* ── Notification transport ──────────────────────────────────────────── */

static void ws_notify_cb(const char *json, size_t len, void *ctx)
{
    if (!s_server || s_client_count == 0) return;

    httpd_ws_frame_t frame = {
        .type    = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)json,
        .len     = len,
        .final   = true,
    };

    for (int i = s_client_count - 1; i >= 0; i--) {
        esp_err_t err = httpd_ws_send_frame_async(s_server, s_client_fds[i], &frame);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Notify send failed fd=%d err=%d — removing", s_client_fds[i], err);
            client_remove(s_client_fds[i]);
        }
    }
}

/* ── WebSocket URI handler ───────────────────────────────────────────── */

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* Auth check on WebSocket upgrade */
        if (!auth_check(req)) {
            ESP_LOGW(TAG, "WS auth failed");
            return send_401(req);
        }
        /* Handshake: record the new client */
        int fd = httpd_req_to_sockfd(req);
        client_add(fd);
        ESP_LOGI(TAG, "WS handshake fd=%d", fd);
        return ESP_OK;
    }

    /* Receive frame */
    httpd_ws_frame_t rx_frame = {0};
    uint8_t *buf = NULL;

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

    buf = (uint8_t *)malloc(rx_frame.len + 1);
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

    ESP_LOGD(TAG, "WS RX: %s", (char *)buf);

    /* Dispatch RPC */
    char *resp_buf = (char *)malloc(WS_BUF_SIZE);
    if (!resp_buf) {
        ESP_LOGE(TAG, "OOM allocating WS response buffer");
        free(buf);
        return ESP_ERR_NO_MEM;
    }

    int resp_len = rpc_dispatch((char *)buf, resp_buf, WS_BUF_SIZE);
    free(buf);

    if (resp_len > 0) {
        httpd_ws_frame_t tx_frame = {
            .type    = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)resp_buf,
            .len     = (size_t)resp_len,
            .final   = true,
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

static const char *CONFIG_HTML =
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Bramble WiFi Setup</title>"
    "<style>"
    "body{font-family:sans-serif;max-width:400px;margin:40px auto;padding:0 20px;background:#1a1a1a;color:#e0e0e0}"
    "h1{color:#4ade80;font-size:1.4em}input{width:100%%;padding:8px;margin:4px 0 12px;box-sizing:border-box;"
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

static esp_err_t config_page_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, CONFIG_HTML, strlen(CONFIG_HTML));
    return ESP_OK;
}

static esp_err_t config_status_handler(httpd_req_t *req)
{
    wifi_status_t st;
    wifi_manager_get_status(&st);
    const char *mode_str = st.mode == BRAMBLE_WIFI_STATION ? "Station" :
                           st.mode == BRAMBLE_WIFI_AP ? "AP" : "Off";

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "mode", mode_str);
    cJSON_AddStringToObject(root, "ssid", st.ssid);
    cJSON_AddStringToObject(root, "ip", st.ip_addr);
    char *json = cJSON_PrintUnformatted(root);
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
static int url_decode(char *dst, const char *src, int src_len)
{
    int di = 0;
    for (int i = 0; i < src_len; i++) {
        if (src[i] == '+') {
            dst[di++] = ' ';
        } else if (src[i] == '%' && i + 2 < src_len) {
            char hex[3] = { src[i+1], src[i+2], 0 };
            dst[di++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            dst[di++] = src[i];
        }
    }
    dst[di] = '\0';
    return di;
}

static esp_err_t config_post_handler(httpd_req_t *req)
{
    if (!auth_check(req)) {
        ESP_LOGW(TAG, "Config POST auth failed");
        return send_401(req);
    }

    char body[256] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[len] = '\0';

    /* Parse form: ssid=xxx&pass=xxx */
    char ssid[33] = {0};
    char pass[65] = {0};

    char *ssid_start = strstr(body, "ssid=");
    char *pass_start = strstr(body, "pass=");

    if (ssid_start) {
        ssid_start += 5;
        char *end = strchr(ssid_start, '&');
        int slen = end ? (int)(end - ssid_start) : (int)strlen(ssid_start);
        if (slen > 32) slen = 32;
        url_decode(ssid, ssid_start, slen);
    }

    if (pass_start) {
        pass_start += 5;
        char *end = strchr(pass_start, '&');
        int plen = end ? (int)(end - pass_start) : (int)strlen(pass_start);
        if (plen > 64) plen = 64;
        url_decode(pass, pass_start, plen);
    }

    if (ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID required");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "WiFi config received: SSID=%s", ssid);
    wifi_manager_nvs_set_creds(ssid, pass);

    const char *resp =
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
    .uri     = "/",
    .method  = HTTP_GET,
    .handler = config_page_handler,
};

static const httpd_uri_t config_status_uri = {
    .uri     = "/config/status",
    .method  = HTTP_GET,
    .handler = config_status_handler,
};

static const httpd_uri_t config_post_uri = {
    .uri     = "/config",
    .method  = HTTP_POST,
    .handler = config_post_handler,
};

/* Called by httpd when any socket is closed (clean or LRU purge) */
static void ws_close_fn(httpd_handle_t hd, int sockfd)
{
    client_remove(sockfd);
    close(sockfd);
}

static const httpd_uri_t ws_uri = {
    .uri          = "/ws",
    .method       = HTTP_GET,
    .handler      = ws_handler,
    .is_websocket = true,
    .handle_ws_control_frames = true,
};

/* ── Public API ──────────────────────────────────────────────────────── */

int ws_server_start(void)
{
    /* Idempotent: no-op if already running */
    if (s_server_running) {
        ESP_LOGD(TAG, "WebSocket server already running");
        return 0;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_open_sockets = MAX_WS_CLIENTS + 2; /* +2 for HTTP clients */
    config.lru_purge_enable = true;  /* close stale connections when slots full */
    config.close_fn = ws_close_fn;   /* clean up tracked client FDs on close */

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

void ws_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        s_client_count = 0;
        s_server_running = false;
        ESP_LOGI(TAG, "WebSocket server stopped");
    }
}

bool ws_server_is_running(void)
{
    return s_server_running;
}
