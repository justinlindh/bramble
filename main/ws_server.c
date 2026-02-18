#include "ws_server.h"
#include "rpc_dispatcher.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include <string.h>

#define MAX_WS_CLIENTS 4
#define WS_BUF_SIZE    2048

static const char *TAG = "ws";
static httpd_handle_t s_server = NULL;
static int s_client_fds[MAX_WS_CLIENTS];
static int s_client_count = 0;

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

    if (rx_frame.len == 0) {
        /* Ping or empty frame */
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

    if (rx_frame.type != HTTPD_WS_TYPE_TEXT) {
        free(buf);
        return ESP_OK; /* ignore binary / ping frames */
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
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_open_sockets = MAX_WS_CLIENTS + 2; /* +2 for HTTP clients */

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %d", err);
        return -1;
    }

    httpd_register_uri_handler(s_server, &ws_uri);
    ESP_LOGI(TAG, "WebSocket server started on port 80 at /ws");

    /* Register push notification transport */
    if (rpc_register_notify_transport(ws_notify_cb, NULL) != 0) {
        ESP_LOGW(TAG, "Failed to register WS notify transport (table full?)");
    }

    return 0;
}

void ws_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        s_client_count = 0;
        ESP_LOGI(TAG, "WebSocket server stopped");
    }
}
