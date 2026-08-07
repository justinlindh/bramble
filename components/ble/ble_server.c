/*
 * BLE GATT server using NimBLE: Nordic UART Service (NUS)
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
#include "ble_redact.h"
#include "ble_link_sec.h"
#include "rpc_dispatcher.h"
#include "rpc_auth.h"
#include "ct_strcmp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/timers.h"
/* ESP-IDF's HCI shim: on that platform nimble_port_init() also brings up the
 * ESP controller through it. Bare-metal builds link NimBLE's own nRF52
 * controller instead, so there is no HCI transport to initialize (and no
 * symbol from this header is used anywhere in the file). */
#ifdef ESP_PLATFORM
#include "esp_nimble_hci.h"
#include "esp_bt.h"
#endif
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "nvs_flash.h"
#include "nvs_keys.h"
#include "mesh_task.h"
#include "ws_server.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define TAG "ble"

/*
 * Bond persistence. ESP-IDF ships NimBLE's NVS-backed key store but does not
 * export its header from the bt component, so upstream's own examples (and
 * therefore we) forward declare the initializer. Backed by
 * CONFIG_BT_NIMBLE_NVS_PERSIST=y.
 */
void ble_store_config_init(void);

/* NUS UUIDs, must match webapp BLETransport.ts */
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

/* Longest name that still fits an advertising PDU beside the flags and TX
 * power fields; see start_advertising. */
#define BLE_ADV_NAME_MAX 23

/* Boot-progress probe. Consoleless boards (nRF T1000-E) override this to
 * record the advertising result in a host-readable flash page; everywhere
 * else it stays a no-op. Stage value 0x10 is BT_ADV in nrf boot_trace.h. */
__attribute__((weak)) void bramble_boot_probe(unsigned stage, int rc) {
    (void)stage;
    (void)rc;
}
static bool s_ble_authenticated = false;
static uint8_t s_auth_fail_count = 0;

/* S21: rate-limit BLE auth attempts */
#define BLE_AUTH_MAX_FAILS 5
#define BLE_AUTH_THROTTLE_MS 100

/* Deferred RPC processing: can't call notify from GATT access context */
static QueueHandle_t s_rpc_queue = NULL;
typedef struct {
    char data[BLE_RPC_BUF_SIZE];
    size_t len;
} ble_rpc_msg_t;

/* True only when the given connection has an encrypted link. A conn handle we
 * cannot look up counts as unencrypted: fail closed. */
static bool conn_is_encrypted(uint16_t conn_handle) {
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn_handle, &desc) != 0) {
        return false;
    }
    return desc.sec_state.encrypted;
}

static bool ble_authenticate_first_write(const char* line) {
    if (ws_server_auth_disabled()) {
        /* Explicit opt-out only; a missing token fails closed */
        s_ble_authenticated = true;
        return true;
    }

    const char* expected = ws_server_get_token();
    if (expected[0] == '\0') {
        /* Token unavailable (NVS failure): nothing can match */
        return false;
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

    /* Never transmit on a cleartext link, not even an auth error reply: a
     * client that subscribed without pairing gets silence (issue #73). */
    if (!ble_link_payload_permitted(conn_is_encrypted(s_conn_handle))) {
        ESP_LOGW(TAG, "Suppressing notify on unencrypted link (conn=%d)", s_conn_handle);
        return;
    }

    /*
     * ATT notifications do NOT fragment: the payload of one notification is
     * capped at ATT_MTU-3 and NimBLE silently truncates anything longer.
     * (L2CAP fragmentation below that is transparent, which is what the
     * previous single-mbuf version wrongly relied on.) Split the JSON line
     * across as many notifications as needed; every client reassembles by
     * newline, and notifications are delivered in order per connection.
     */
    size_t max_chunk = (s_mtu > 3) ? (size_t)(s_mtu - 3) : 20;
    size_t total = len + 1; /* JSON + trailing newline */
    ESP_LOGI(TAG, "Sending notify %u bytes in %u-byte chunks (mtu=%u)", (unsigned)total,
             (unsigned)max_chunk, s_mtu);

    size_t off = 0;
    while (off < total) {
        size_t n = total - off;
        if (n > max_chunk)
            n = max_chunk;

        /*
         * Bounded retry covering BOTH the allocation and the send. The msys
         * pool refills as the stack drains queued notifications, and an
         * ENOMEM out of notify_custom means the same "momentarily empty" as a
         * NULL allocation: a 253-byte chunk needs two 292-byte blocks, so a
         * response of a few chunks can outrun a 12-block pool and then
         * recover a few milliseconds later. Retrying the send matters as much
         * as retrying the alloc, because giving up mid-response truncates the
         * JSON stream, which is the corruption the chunking exists to avoid.
         * Observed on the nRF bench: an 895-byte getNeighbors reply died at
         * 759/896 with rc=6.
         *
         * Each attempt must build a fresh mbuf: ble_att_clt_tx_notify
         * consumes the one it is given on every path, failures included.
         * Only ENOMEM is worth retrying; ENOTCONN and friends will not
         * improve with time.
         */
        int rc = BLE_HS_ENOMEM;
        for (int attempt = 0; attempt < 25 && rc == BLE_HS_ENOMEM; attempt++) {
            struct os_mbuf* om;
            if (off + n <= len) {
                om = ble_hs_mbuf_from_flat(json + off, n);
            } else if (off < len) {
                om = ble_hs_mbuf_from_flat(json + off, len - off);
                if (om != NULL) {
                    uint8_t nl = '\n';
                    os_mbuf_append(om, &nl, 1);
                }
            } else {
                uint8_t nl = '\n';
                om = ble_hs_mbuf_from_flat(&nl, 1);
            }
            if (om == NULL) {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            rc = ble_gatts_notify_custom(s_conn_handle, s_rx_attr_handle, om);
            if (rc == BLE_HS_ENOMEM) {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        }
        if (rc != 0) {
            ESP_LOGW(TAG, "Notify chunk failed: %d at %u/%u; dropping rest", rc, (unsigned)off,
                     (unsigned)total);
            return;
        }
        off += n;
    }
}

/* Registered with the RPC dispatcher for server-push notifications.
 * Notifications carry decrypted content; an unauthenticated BLE client
 * must not receive them (rpc_auth_notify_allowed, host-tested). Direct
 * RPC responses bypass this wrapper via ble_notify_cb so the pairing
 * allowlist and auth error replies still reach the caller. */
static void ble_notify_transport_cb(const char* json, size_t len, void* ctx) {
    if (!rpc_auth_notify_allowed(s_ble_authenticated, ws_server_auth_disabled())) {
        return;
    }
    ble_notify_cb(json, len, ctx);
}

/* ── Process incoming data (JSON-RPC lines) ──────────────────────────── */

/* BLE RPC processing task: runs in its own context, safe to call notify */
static void ble_rpc_task(void* param) {
    (void)param;
    ble_rpc_msg_t msg;
    while (1) {
        if (xQueueReceive(s_rpc_queue, &msg, portMAX_DELAY) == pdTRUE) {
            msg.data[msg.len] = '\0';
            /* Never echo pre-auth payloads: the first write is the bare token
             * (NEW-SEC-5). Post-auth bodies go to DEBUG only. */
            if (ble_rpc_body_loggable(s_ble_authenticated)) {
                ESP_LOGD(TAG, "BLE RPC request (%u bytes): %.80s", (unsigned)msg.len, msg.data);
            } else {
                ESP_LOGI(TAG, "BLE RPC request (%u bytes)", (unsigned)msg.len);
            }

            char resp[BLE_RPC_BUF_SIZE];
            if (!s_ble_authenticated) {
                /* Pre-auth JSON-RPC lines are dispatched UNAUTHENTICATED:
                 * rpc_dispatch_authed() limits them to the tiny pairing
                 * allowlist (rpc_auth.c). Anything that is not JSON is
                 * treated as a token handshake attempt. */
                if (msg.data[0] == '{') {
                    int resp_len = rpc_dispatch_authed(msg.data, resp, sizeof(resp), false);
                    if (resp_len > 0) {
                        ble_notify_cb(resp, (size_t)resp_len, NULL);
                    }
                    continue;
                }
                if (!ble_authenticate_first_write(msg.data)) {
                    s_auth_fail_count++;
                    ESP_LOGW(TAG, "BLE auth failed (attempt %u/%u)", s_auth_fail_count,
                             BLE_AUTH_MAX_FAILS);
                    const char* unauthorized =
                        "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32603,\"message\":"
                        "\"unauthorized: first BLE write must be auth token\"},\"id\":null}";
                    ble_notify_cb(unauthorized, strlen(unauthorized), NULL);
                    /* Throttle guessing: 100ms delay before next attempt */
                    vTaskDelay(pdMS_TO_TICKS(BLE_AUTH_THROTTLE_MS));
                    if (s_auth_fail_count >= BLE_AUTH_MAX_FAILS) {
                        ESP_LOGW(TAG, "BLE auth: max failures reached, disconnecting client");
                        if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
                            ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                        }
                        s_auth_fail_count = 0;
                    }
                    continue;
                }

                s_auth_fail_count = 0; /* reset on successful auth */
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
    (void)attr_handle;
    (void)arg;

    /* The BLE_GATT_CHR_F_WRITE_ENC flags below already make the ATT server
     * reject unencrypted writes with "insufficient authentication". This is
     * the second, independent check: the auth token arrives on this
     * characteristic, so a permission regression must not be able to leak it
     * silently (issue #73). */
    if (!ble_link_payload_permitted(conn_is_encrypted(conn_handle))) {
        ESP_LOGW(TAG, "Rejecting BLE write on unencrypted link (conn=%d)", conn_handle);
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    }

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
                    /* _ENC: writes require an encrypted link. The RPC auth
                     * token is the first write on this characteristic, so
                     * cleartext is not an option (issue #73). */
                    .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP |
                             BLE_GATT_CHR_F_WRITE_ENC,
                },
                {
                    /* RX: device notifies app */
                    .uuid = &NUS_RX_UUID.u,
                    .access_cb = nus_rx_access,
                    .val_handle = &s_rx_attr_handle,
                    /* There is no NOTIFY_ENC flag in NimBLE, and NimBLE
                     * registers the CCCD with plain read/write permissions
                     * regardless of the characteristic's flags, so a client
                     * can always subscribe. The outbound direction is instead
                     * gated in ble_notify_cb, which refuses to transmit on an
                     * unencrypted link. */
                    .flags = BLE_GATT_CHR_F_NOTIFY,
                },
                {0}, /* sentinel */
            },
    },
    {0}, /* sentinel */
};

/* ── GAP event handler ───────────────────────────────────────────────── */

static int gap_event_handler(struct ble_gap_event* event, void* arg);
static void start_advertising(void);

/* Advertising restart retry. start_advertising used to be fire-and-forget:
 * called once per disconnect, and if the host returned any transient error
 * (busy during a host reset, EPREEMPTED, momentary pool pressure) the
 * failure was logged and the device simply never advertised again while the
 * mesh kept running. On a consoleless board that is indistinguishable from
 * a dead node, and it is reachable in practice: a client that drops the
 * link mid-pairing can race the restart against SMP teardown. Reproduced on
 * the bench (T1000-E, killed bleak sessions) and seen once on the dev kit
 * after 11.7h. Any failed start now arms a one-shot retry timer; the timer
 * re-arms until advertising is up or a connection exists. */
#define BLE_ADV_RETRY_MS 1000

static TimerHandle_t s_adv_retry_timer;

static void adv_retry_cb(TimerHandle_t t) {
    (void)t;
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE && !ble_gap_adv_active()) {
        start_advertising();
    }
}

static void schedule_adv_retry(void) {
    /* Created once in ble_server_init, so no lazy-create race between the
     * host task and the timer task; NULL only if creation failed at init,
     * in which case the old fire-and-forget behavior is what remains. */
    if (s_adv_retry_timer != NULL) {
        xTimerStart(s_adv_retry_timer, 0);
    }
}

static void start_advertising(void) {
    struct ble_gap_adv_params adv_params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = 0x20, /* 20ms */
        .itvl_max = 0x40, /* 40ms */
    };

    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    /* The advertising PDU is 31 bytes: flags (3) and TX power (3) leave
     * 23 for the name after its own 2 byte header. setNodeName accepts up to
     * BRAMBLE_NODE_NAME_MAX (32), and an over-long name made
     * ble_gap_adv_set_fields return BLE_HS_EMSGSIZE, which aborted this
     * function and left the node advertising NOTHING: undiscoverable until
     * someone renamed it over a transport they could no longer reach. Send
     * the shortened-name form instead, which is exactly what it is for; the
     * complete name is still readable from the GAP characteristic. */
    size_t name_len = strlen(s_device_name);
    fields.name = (uint8_t*)s_device_name;
    fields.name_is_complete = name_len <= BLE_ADV_NAME_MAX;
    fields.name_len = (uint8_t)(fields.name_is_complete ? name_len : BLE_ADV_NAME_MAX);
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.tx_pwr_lvl_is_present = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields failed: %d", rc);
        schedule_adv_retry();
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
        schedule_adv_retry();
        return;
    }

    /* S22: advertise with random address, not permanent public MAC */
    rc = ble_gap_adv_start(BLE_OWN_ADDR_RANDOM, NULL, BLE_HS_FOREVER, &adv_params,
                           gap_event_handler, NULL);
    bramble_boot_probe(0x10, rc);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "adv_start failed: %d", rc);
        schedule_adv_retry();
    } else if (rc == 0) {
        ESP_LOGI(TAG, "BLE advertising started as '%s' (random addr)", s_device_name);
    }
}

static int gap_event_handler(struct ble_gap_event* event, void* arg) {
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_ble_authenticated = ws_server_auth_disabled();
            ESP_LOGI(TAG, "BLE client connected (handle=%d, auth=%s)", s_conn_handle,
                     s_ble_authenticated ? "open" : "required");

            /* Request higher MTU for better throughput */
            ble_att_set_preferred_mtu(256);
            ble_gattc_exchange_mtu(s_conn_handle, NULL, NULL);

            /*
             * Ask the central to encrypt immediately rather than waiting for
             * it to trip over an "insufficient authentication" error on the
             * first write. Already-bonded peers re-key from the stored LTK
             * with no user interaction; new peers get the OS pairing prompt
             * before any RPC traffic is attempted, which is what keeps the
             * client's auth handshake from failing its first attempt.
             * BLE_HS_EALREADY means encryption is already up.
             */
            int sec_rc = ble_gap_security_initiate(s_conn_handle);
            if (sec_rc != 0 && sec_rc != BLE_HS_EALREADY) {
                ESP_LOGW(TAG, "security_initiate failed: %d", sec_rc);
            }
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
        s_auth_fail_count = 0;
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

    case BLE_GAP_EVENT_ENC_CHANGE: {
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
            ESP_LOGI(TAG, "Encryption change: status=%d encrypted=%d authenticated=%d bonded=%d",
                     event->enc_change.status, desc.sec_state.encrypted,
                     desc.sec_state.authenticated, desc.sec_state.bonded);
        } else {
            ESP_LOGW(TAG, "Encryption change: status=%d (conn lookup failed)",
                     event->enc_change.status);
        }
        break;
    }

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /*
         * The peer wants to pair again while we still hold a bond for it,
         * which is what happens after the client side forgets the device (an
         * OS "forget", a browser profile reset, a reinstall). Dropping the
         * stale bond and letting pairing continue is the difference between
         * "re-pair and it works" and "re-pair fails forever until the node is
         * factory reset". The old LTK is useless to us at this point anyway.
         */
        {
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
            ESP_LOGI(TAG, "Repeat pairing: dropped stale bond, continuing");
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        /* NO_INPUT_OUTPUT means Just Works, so the stack should never ask us
         * for a passkey. If it does, the IO capability configuration and the
         * board's actual capabilities have diverged: log loudly rather than
         * silently accepting something we cannot show the user. */
        ESP_LOGE(TAG, "Unexpected passkey action %d on a Just Works configuration",
                 event->passkey.params.action);
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
    nimble_port_run(); /* blocks until the stack is stopped */
#ifdef ESP_PLATFORM
    nimble_port_freertos_deinit();
#else
    /* Upstream NimBLE has no freertos_deinit; the port's own task function
     * returns only at shutdown, so end the task explicitly. */
    vTaskDelete(NULL);
#endif
}

static void on_sync(void) {
    /*
     * S22: prefer a static random address so we do not advertise with the
     * permanent public MAC (which enables passive location tracking).
     * prefer_random=1 generates a static random address if one isn't set.
     *
     * Full RPA (Resolvable Private Address) with periodic rotation requires
     * bonding/IRK infrastructure that isn't implemented yet.  Static random
     * is a meaningful improvement in the interim; it stops passive MAC-based
     * tracking by scanners that never connect.
     */
    int rc = ble_hs_util_ensure_addr(1);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure_addr failed: %d", rc);
        return;
    }
    start_advertising();
}

static void on_reset(int reason) { ESP_LOGW(TAG, "BLE host reset: reason=%d", reason); }

/* ── Public API ──────────────────────────────────────────────────────── */

bool ble_server_supported(void) { return true; }

/* Default BLE name when the operator has not named the node: "Bramble-XXXX"
 * carrying the low 16 bits of the node address, the same short form the mDNS
 * hostname (bramble-%04x) and the webapp's formatAddrShort already use. A
 * fleet of unnamed nodes otherwise advertises one indistinguishable
 * "Bramble", and on a consoleless board (T1000-E) the advertisement is the
 * only place the address is legible before connecting. Identity is started
 * before the transport on both platforms, so the lookup normally succeeds;
 * the bare name remains the fallback if it ever does not. */
static void set_default_device_name(void) {
    uint32_t addr = 0;
    uint8_t pubkey[32];
    if (mesh_get_identity(&addr, pubkey) == 0) {
        snprintf(s_device_name, sizeof(s_device_name), "Bramble-%04" PRIX32, addr & 0xFFFFu);
    } else {
        strncpy(s_device_name, "Bramble", sizeof(s_device_name) - 1);
        s_device_name[sizeof(s_device_name) - 1] = '\0';
    }
}

int ble_server_init(void) {
    /* Read node name for BLE device name */
    bool named = false;
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS_BRAMBLE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(s_device_name);
        named = nvs_get_str(nvs, "node_name", s_device_name, &len) == ESP_OK && s_device_name[0];
        nvs_close(nvs);
    }
    if (!named) {
        set_default_device_name();
    }

    /* Advertising restart retry timer; see schedule_adv_retry. Created
     * here once so no two tasks ever race the creation. */
    s_adv_retry_timer =
        xTimerCreate("ble_adv_retry", pdMS_TO_TICKS(BLE_ADV_RETRY_MS), pdFALSE, NULL, adv_retry_cb);

    /* Create RPC processing queue and task */
    s_rpc_queue = xQueueCreate(4, sizeof(ble_rpc_msg_t));
    if (!s_rpc_queue) {
        ESP_LOGE(TAG, "Failed to create RPC queue");
        return -1;
    }
    /* 16KB: a real phone session (MTU 256, sustained polls + sends + notify
     * chunking) overflowed the previous 8192 after ~40 minutes - panic
     * "stack overflow in task ble_rpc" on the bench V4, 2026-07-07. RPC
     * handlers (cJSON trees, crypto, msg store) execute on this task, so
     * size it for their worst case plus interrupt frames, not the average. */
    xTaskCreate(ble_rpc_task, "ble_rpc", 16384, NULL, 5, NULL);

#ifdef ESP_PLATFORM
    /* On ESP-IDF this also starts the Bluetooth controller and can fail. */
    int rc = nimble_port_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "nimble_port_init failed: %d", rc);
        return -1;
    }
#else
    /* Upstream initializes host plus the built-in controller and returns
     * void; failures assert inside the stack. */
    nimble_port_init();
    int rc = 0;
#endif

    /* Configure NimBLE host */
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;

    /*
     * Security manager (issue #73).
     *
     * sm_sc=1 selects LE Secure Connections: pairing is an ECDH P-256 key
     * agreement, so a passive observer that records the entire pairing
     * exchange still cannot derive the LTK. CONFIG_BT_NIMBLE_SM_LEGACY=n in
     * the board defaults compiles legacy pairing out, because legacy Just
     * Works is trivially recoverable from a sniffed exchange and a
     * downgrade to it would silently undo this whole change.
     *
     * sm_io_cap=NO_IO gives Just Works. That is a deliberate choice, not an
     * oversight: a passkey or numeric-comparison flow needs a display AND a
     * confirm input on the peripheral, and the supported boards do not all
     * have both (see docs/ and components/board_config). A flow that works
     * on one profile and bricks BLE on another is worse than a uniform one.
     * The residual is that an active MITM present at first-pairing time can
     * still interpose; the passive sniffer in issue #73 cannot. Follow-up
     * tracked separately.
     *
     * Bonding plus ENC|ID key distribution: the peer keeps the LTK so the
     * pairing prompt is once per device, and exchanging IRKs is the
     * groundwork for resolvable private addresses (the RPA gap noted in
     * on_sync).
     */
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    /* Persist bonds in NVS (CONFIG_BT_NIMBLE_NVS_PERSIST=y) so a reboot does
     * not force every paired client to pair again. */
    ble_store_config_init();

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
    rpc_register_notify_transport(ble_notify_transport_cb, NULL);

    ESP_LOGI(TAG, "BLE server initialized (name=%s)", s_device_name);
    return 0;
}

int ble_server_start(void) {
#ifdef ESP_PLATFORM
    /* Default BLE TX is ~3 dBm, far below the 20 dBm the SoftAP runs at; a
     * pager-class device wants the link budget. +9 dBm is the S3 ceiling for
     * advertising and connections. Failure is non-fatal: worst case the link
     * runs at default power. */
    esp_err_t pw = esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);
    esp_err_t pw_adv = esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P9);
    if (pw != ESP_OK || pw_adv != ESP_OK)
        ESP_LOGW(TAG, "BLE TX power set failed (default=%d adv=%d)", (int)pw, (int)pw_adv);
#endif
    nimble_port_freertos_init(ble_host_task);
    ESP_LOGI(TAG, "BLE server started");
    return 0;
}

void ble_server_stop(void) {
    /* A retry armed just before shutdown must not fire into a deinitialized
     * host. Delete waits for the timer command queue, so after this returns
     * the callback can no longer run. */
    if (s_adv_retry_timer != NULL) {
        xTimerDelete(s_adv_retry_timer, portMAX_DELAY);
        s_adv_retry_timer = NULL;
    }
#ifdef ESP_PLATFORM
    int rc = nimble_port_stop();
    if (rc == 0) {
        nimble_port_deinit();
    }
#else
    /* Upstream NimBLE exposes no stop/deinit in the porting layer; this
     * target never tears the stack down (there is no Wi-Fi mode to switch
     * to), so the honest thing is to say so rather than pretend. */
    ESP_LOGW(TAG, "ble_server_stop is not supported on this platform");
#endif
}

bool ble_server_connected(void) { return s_conn_handle != BLE_HS_CONN_HANDLE_NONE; }

int ble_server_notify(const char* json, size_t len) {
    /* Public push API: same auth gating as dispatcher notifications */
    ble_notify_transport_cb(json, len, NULL);
    return 0;
}
