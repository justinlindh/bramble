/*
 * BLE GATT server using NimBLE — Nordic UART Service (NUS)
 *
 * The webapp BLETransport.ts connects to NUS and exchanges JSON-RPC
 * over the TX/RX characteristics, same protocol as WebSocket.
 *
 * NUS UUIDs (standard):
 *   Service:  6e400001-b5a3-f393-e0a9-e50e24dcca9e
 *   TX (app→device, write): 6e400002-...
 *   RX (device→app, notify): 6e400003-...
 */

#include "ble_server.h"
#include "rpc_dispatcher.h"
#include "ct_strcmp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_nimble_hci.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "nvs_flash.h"
#include "ws_server.h"
#include <stdio.h>
#include <string.h>

#define TAG "ble"

/* NUS UUIDs — must match webapp BLETransport.ts */
static const ble_uuid128_t NUS_SERVICE_UUID = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);

/* TX char: app writes to device (we receive) */
static const ble_uuid128_t NUS_TX_UUID = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);

/* RX char: device notifies app (we send) */
static const ble_uuid128_t NUS_RX_UUID = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

#define BLE_RPC_BUF_SIZE 2048
#define BLE_MTU_DEFAULT 20 /* ATT_MTU(23) - 3 byte ATT header */

static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_rx_attr_handle; /* handle for RX characteristic (notify) */
static bool s_rx_notify_enabled = false;
static uint16_t s_mtu = BLE_MTU_DEFAULT + 3; /* negotiated ATT MTU */
static char s_line_buf[BLE_RPC_BUF_SIZE];
static size_t s_line_len = 0;
static char s_device_name[32] = "Bramble";
static bool s_ble_authenticated = false;

/* Deferred RPC processing — can't call notify from GATT access context */
static QueueHandle_t s_rpc_queue = NULL;
typedef struct {
    char data[BLE_RPC_BUF_SIZE];
    size_t len;
} ble_rpc_msg_t;

static bool ble_authenticate_first_write(const char* line) {
    const char* expected = ws_server_get_token();
    if (expected[0] == '\0') {
        s_ble_authenticated = true;
        return true;
    }

    if (ct_strcmp(line, expected) == 0) {
        s_ble_authenticated = true;
        ESP_LOGI(TAG, "BLE auth handshake accepted");
        return true;
    }

    return false;
}

/* ── RPC notification callback (registered with rpc_dispatcher) ────── */

static void ble_notify_cb(const char* json, size_t len, void* ctx) {
    (void)ctx;
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || !s_rx_notify_enabled) {
        ESP_LOGW(TAG, "Cannot notify: conn=%d notify=%d", s_conn_handle, s_rx_notify_enabled);
        return;
    }

    /*
     * Send JSON + newline as a single mbuf chain.
     * NimBLE handles L2CAP fragmentation internally based on negotiated MTU,
     * so we don't need to chunk manually — just build one mbuf with the full payload.
     */
    ESP_LOGI(TAG, "Sending notify %u bytes (mtu=%u)", (unsigned)(len + 1), s_mtu);

    struct os_mbuf* om = ble_hs_mbuf_from_flat(json, len);
    if (!om) {
        ESP_LOGW(TAG, "Failed to allocate mbuf for notify");
        return;
    }
    /* Append newline */
    uint8_t nl = '\n';
    os_mbuf_append(om, &nl, 1);

    int rc = ble_gatts_notify_custom(s_conn_handle, s_rx_attr_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "Notify failed: %d (%u bytes)", rc, (unsigned)(len + 1));
    }
}

/* ── Process incoming data (JSON-RPC lines) ──────────────────────────── */

/* BLE RPC processing task — runs in its own context, safe to call notify */
static void ble_rpc_task(void* param) {
    (void)param;
    ble_rpc_msg_t msg;
    while (1) {
        if (xQueueReceive(s_rpc_queue, &msg, portMAX_DELAY) == pdTRUE) {
            msg.data[msg.len] = '\0';
            ESP_LOGI(TAG, "BLE RPC request (%u bytes): %.80s", (unsigned)msg.len, msg.data);

            char resp[BLE_RPC_BUF_SIZE];
            if (!s_ble_authenticated) {
                if (!ble_authenticate_first_write(msg.data)) {
                    const char* unauthorized =
                        "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32603,\"message\":\"unauthorized: first BLE write must be auth token\"},\"id\":null}";
                    ESP_LOGW(TAG, "Rejected unauthenticated BLE client (invalid first write)");
                    ble_notify_cb(unauthorized, strlen(unauthorized), NULL);
                    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
                        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                    }
                    continue;
                }

                const char* ok = "{\"jsonrpc\":\"2.0\",\"result\":{\"ok\":true},\"id\":null}";
                ble_notify_cb(ok, strlen(ok), NULL);
                continue;
            }

            int resp_len = rpc_dispatch(msg.data, resp, sizeof(resp));
            ESP_LOGI(TAG, "BLE RPC response (%d bytes)", resp_len);
            if (resp_len > 0) {
                ble_notify_cb(resp, (size_t)resp_len, NULL);
            }
        }
    }
}

static void process_ble_data(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char c = (char)data[i];
        if (c == '\n' || c == '\r') {
            if (s_line_len > 0) {
                /* Queue the complete line for processing in the RPC task */
                ble_rpc_msg_t msg;
                if (s_line_len < sizeof(msg.data)) {
                    memcpy(msg.data, s_line_buf, s_line_len);
                    msg.len = s_line_len;
                    xQueueSend(s_rpc_queue, &msg, 0);
                }
                s_line_len = 0;
            }
        } else if (s_line_len < BLE_RPC_BUF_SIZE - 1) {
            s_line_buf[s_line_len++] = c;
        }
    }
}

/* ── GATT characteristic access callbacks ────────────────────────────── */

static int nus_tx_access(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt* ctxt, void* arg) {
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len > 0 && len < BLE_RPC_BUF_SIZE) {
            uint8_t buf[BLE_RPC_BUF_SIZE];
            int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, len, NULL);
            if (rc == 0) {
                process_ble_data(buf, len);
            }
        }
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static int nus_rx_access(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt* ctxt, void* arg) {
    (void)conn_handle;
    (void)attr_handle;
    (void)ctxt;
    (void)arg;
    /* RX is notify-only, no read/write from client */
    return 0;
}

/* ── GATT service definition ─────────────────────────────────────────── */

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &NUS_SERVICE_UUID.u,
        .characteristics =
            (struct ble_gatt_chr_def[]){
                {
                    /* TX: app writes to device */
                    .uuid = &NUS_TX_UUID.u,
                    .access_cb = nus_tx_access,
                    .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
                },
                {
                    /* RX: device notifies app */
                    .uuid = &NUS_RX_UUID.u,
                    .access_cb = nus_rx_access,
                    .val_handle = &s_rx_attr_handle,
                    .flags = BLE_GATT_CHR_F_NOTIFY,
                },
                {0}, /* sentinel */
            },
    },
    {0}, /* sentinel */
};

/* ── GAP event handler ───────────────────────────────────────────────── */

static int gap_event_handler(struct ble_gap_event* event, void* arg);

static void start_advertising(void) {
    struct ble_gap_adv_params adv_params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = 0x20, /* 20ms */
        .itvl_max = 0x40, /* 40ms */
    };

    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t*)s_device_name;
    fields.name_len = strlen(s_device_name);
    fields.name_is_complete = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.tx_pwr_lvl_is_present = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields failed: %d", rc);
        return;
    }

    /* Include NUS UUID in scan response so Web Bluetooth can filter */
    struct ble_hs_adv_fields rsp_fields = {0};
    rsp_fields.uuids128 = (ble_uuid128_t*)&NUS_SERVICE_UUID;
    rsp_fields.num_uuids128 = 1;
    rsp_fields.uuids128_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_rsp_set_fields failed: %d", rc);
        return;
    }

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &adv_params,
                           gap_event_handler, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_start failed: %d", rc);
    } else {
        ESP_LOGI(TAG, "BLE advertising started as '%s'", s_device_name);
    }
}

static int gap_event_handler(struct ble_gap_event* event, void* arg) {
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_ble_authenticated = (ws_server_get_token()[0] == '\0');
            ESP_LOGI(TAG, "BLE client connected (handle=%d, auth=%s)", s_conn_handle,
                     s_ble_authenticated ? "open" : "required");

            /* Request higher MTU for better throughput */
            ble_att_set_preferred_mtu(256);
            ble_gattc_exchange_mtu(s_conn_handle, NULL, NULL);
        } else {
            ESP_LOGW(TAG, "BLE connect failed: status=%d", event->connect.status);
            start_advertising();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE client disconnected (reason=%d)", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_rx_notify_enabled = false;
        s_ble_authenticated = false;
        s_mtu = BLE_MTU_DEFAULT + 3;
        s_line_len = 0;
        start_advertising();
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_rx_attr_handle) {
            s_rx_notify_enabled = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "RX notifications %s", s_rx_notify_enabled ? "enabled" : "disabled");
        }
        break;

    case BLE_GAP_EVENT_MTU:
        s_mtu = event->mtu.value;
        ESP_LOGI(TAG, "MTU updated: conn=%d, mtu=%d", event->mtu.conn_handle, event->mtu.value);
        break;

    default:
        break;
    }
    return 0;
}

/* ── NimBLE host task ────────────────────────────────────────────────── */

static void ble_host_task(void* param) {
    (void)param;
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run(); /* blocks until nimble_port_stop() */
    nimble_port_freertos_deinit();
}

static void on_sync(void) {
    /* Use best address type available */
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure_addr failed: %d", rc);
        return;
    }
    start_advertising();
}

static void on_reset(int reason) { ESP_LOGW(TAG, "BLE host reset: reason=%d", reason); }

/* ── Public API ──────────────────────────────────────────────────────── */

int ble_server_init(void) {
    /* Read node name for BLE device name */
    nvs_handle_t nvs;
    if (nvs_open("bramble", NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(s_device_name);
        if (nvs_get_str(nvs, "node_name", s_device_name, &len) != ESP_OK) {
            strncpy(s_device_name, "Bramble", sizeof(s_device_name));
        }
        nvs_close(nvs);
    }

    /* Create RPC processing queue and task */
    s_rpc_queue = xQueueCreate(4, sizeof(ble_rpc_msg_t));
    if (!s_rpc_queue) {
        ESP_LOGE(TAG, "Failed to create RPC queue");
        return -1;
    }
    xTaskCreate(ble_rpc_task, "ble_rpc", 8192, NULL, 5, NULL);

    int rc = nimble_port_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "nimble_port_init failed: %d", rc);
        return -1;
    }

    /* Configure NimBLE host */
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;

    /* Register GATT services */
    ble_svc_gap_init();
    ble_svc_gatt_init();

    rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts_count_cfg failed: %d", rc);
        return -1;
    }

    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts_add_svcs failed: %d", rc);
        return -1;
    }

    /* Set device name */
    ble_svc_gap_device_name_set(s_device_name);

    /* Register notification callback with RPC dispatcher */
    rpc_register_notify_transport(ble_notify_cb, NULL);

    ESP_LOGI(TAG, "BLE server initialized (name=%s)", s_device_name);
    return 0;
}

int ble_server_start(void) {
    nimble_port_freertos_init(ble_host_task);
    ESP_LOGI(TAG, "BLE server started");
    return 0;
}

void ble_server_stop(void) {
    int rc = nimble_port_stop();
    if (rc == 0) {
        nimble_port_deinit();
        ESP_LOGI(TAG, "BLE server stopped");
    }
}

bool ble_server_connected(void) { return s_conn_handle != BLE_HS_CONN_HANDLE_NONE; }

int ble_server_notify(const char* json, size_t len) {
    ble_notify_cb(json, len, NULL);
    return 0;
}
