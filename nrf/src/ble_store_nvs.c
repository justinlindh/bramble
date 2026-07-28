/*
 * Bond persistence for the nRF52840 target.
 *
 * NimBLE's ble_store_config keeps bonds (LTK, IRK, CCCD subscriptions) in RAM
 * and calls the three hooks below whenever that state changes, leaving the
 * backing store to the port. Upstream's backend is a mynewt conf-subsystem
 * client that this build does not have; the ESP fleet uses esp-nimble's NVS
 * backend. This is the same thing over the LittleFS-backed NVS shim, so a
 * paired phone survives a reset here exactly as it does on the ESP boards.
 *
 * Without it the node forgets every LTK on reset while the peer keeps its
 * half, so reconnects fail with an encryption error until the user manually
 * forgets the device.
 *
 * The blobs hold key material in plaintext flash, which is the same exposure
 * the ESP fleet has in NVS: readback protection (APPROTECT here, flash
 * encryption there) is what protects it, not the store.
 */
#include <string.h>

#include <FreeRTOS.h>
#include <task.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_keys.h"

#include "host/ble_hs.h"

#include "ble_store_config_priv.h"

static const char* TAG = "ble_store";

#define BOND_KEY_OUR_SECS "our_secs"
#define BOND_KEY_PEER_SECS "peer_secs"
#define BOND_KEY_CCCDS "cccds"

/* Bumped whenever the on-flash layout changes. Records written by an older
 * layout are dropped rather than reinterpreted: a misparsed LTK is a bond
 * that fails to decrypt, which is far harder to diagnose than a re-pair. */
#define BOND_BLOB_VERSION 1

typedef struct {
    uint16_t version;
    uint16_t elem_size;
    uint16_t count;
} bond_blob_hdr_t;

/* One stack buffer serves all three records, so size it for the largest. */
#define BOND_BLOB_MAX_PAYLOAD                                     \
    (sizeof(ble_store_config_our_secs) > sizeof(ble_store_config_cccds) \
         ? sizeof(ble_store_config_our_secs)                       \
         : sizeof(ble_store_config_cccds))

static esp_err_t bond_open(nvs_open_mode_t mode, nvs_handle_t* out) {
    return nvs_open(NVS_NS_BLE_BOND, mode, out);
}

static int persist_array(const char* key, const void* array, size_t elem_size, int count) {
    nvs_handle_t h;
    if (bond_open(NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "%s: nvs_open failed", key);
        return BLE_HS_ESTORE_FAIL;
    }

    int rc = 0;
    if (count <= 0) {
        /* Erasing the last bond must clear the record, not leave the previous
         * contents behind for the next boot to load. */
        esp_err_t err = nvs_erase_key(h, key);
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
            rc = BLE_HS_ESTORE_FAIL;
        }
    } else {
        uint8_t buf[sizeof(bond_blob_hdr_t) + BOND_BLOB_MAX_PAYLOAD];
        size_t payload = elem_size * (size_t)count;
        if (payload > BOND_BLOB_MAX_PAYLOAD) {
            nvs_close(h);
            ESP_LOGE(TAG, "%s: %u bytes exceeds the blob budget", key, (unsigned)payload);
            return BLE_HS_ESTORE_FAIL;
        }
        bond_blob_hdr_t hdr = {
            .version = BOND_BLOB_VERSION,
            .elem_size = (uint16_t)elem_size,
            .count = (uint16_t)count,
        };
        memcpy(buf, &hdr, sizeof(hdr));
        memcpy(buf + sizeof(hdr), array, payload);
        if (nvs_set_blob(h, key, buf, sizeof(hdr) + payload) != ESP_OK) {
            rc = BLE_HS_ESTORE_FAIL;
        }
    }

    if (rc == 0 && nvs_commit(h) != ESP_OK) {
        rc = BLE_HS_ESTORE_FAIL;
    }
    nvs_close(h);
    if (rc != 0) {
        ESP_LOGE(TAG, "%s: persist failed", key);
    }
    return rc;
}

static void load_array(nvs_handle_t h, const char* key, void* array, size_t elem_size, int max,
                       int* out_count) {
    *out_count = 0;

    uint8_t buf[sizeof(bond_blob_hdr_t) + BOND_BLOB_MAX_PAYLOAD];
    size_t len = sizeof(buf);
    if (nvs_get_blob(h, key, buf, &len) != ESP_OK || len < sizeof(bond_blob_hdr_t)) {
        return;
    }

    bond_blob_hdr_t hdr;
    memcpy(&hdr, buf, sizeof(hdr));
    if (hdr.version != BOND_BLOB_VERSION || hdr.elem_size != elem_size) {
        ESP_LOGW(TAG, "%s: dropping record from an older layout (v%u, elem %u)", key,
                 (unsigned)hdr.version, (unsigned)hdr.elem_size);
        return;
    }
    if (hdr.count > max || sizeof(hdr) + (size_t)hdr.count * elem_size != len) {
        ESP_LOGW(TAG, "%s: dropping truncated or oversized record", key);
        return;
    }

    memcpy(array, buf + sizeof(hdr), (size_t)hdr.count * elem_size);
    *out_count = hdr.count;
}

/*
 * The hooks below are called by NimBLE's host task at the end of pairing, in
 * the same breath as the encryption handshake. Writing flash there is fatal
 * to the link: NVMC programming stalls the CPU, the link layer misses its
 * connection events, and the peer drops the connection with a MIC failure or
 * a supervision timeout. So the hooks only mark the record dirty; a
 * low-priority writer flushes it once the radio has gone quiet. The values
 * themselves already live in ble_store_config's RAM arrays, which is what
 * NimBLE reads back, so nothing depends on the write having landed yet.
 *
 * mesh_persist.c defers its replay-table writes for the same reason.
 */
#define BOND_DIRTY_OUR_SECS (1u << 0)
#define BOND_DIRTY_PEER_SECS (1u << 1)
#define BOND_DIRTY_CCCDS (1u << 2)

/* How long the flush waits after the last change. Long enough for pairing
 * and the client's first RPC exchange to finish, short enough that a user
 * who pairs and immediately unplugs the node keeps the bond. */
#define BOND_FLUSH_SETTLE_MS 3000

static volatile uint32_t s_dirty;
static TaskHandle_t s_writer;

static void mark_dirty(uint32_t bit) {
    taskENTER_CRITICAL();
    s_dirty |= bit;
    taskEXIT_CRITICAL();
    if (s_writer) {
        xTaskNotifyGive(s_writer);
    }
}

static void bond_writer_task(void* arg) {
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        /* Coalesce: pairing dirties all three records in quick succession,
         * and each flush costs a flash write. Wait for quiet, restarting the
         * wait whenever something else changes. */
        while (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(BOND_FLUSH_SETTLE_MS)) > 0) {
        }

        taskENTER_CRITICAL();
        uint32_t dirty = s_dirty;
        s_dirty = 0;
        taskEXIT_CRITICAL();

        int rc = 0;
        if (dirty & BOND_DIRTY_OUR_SECS) {
            rc |= persist_array(BOND_KEY_OUR_SECS, ble_store_config_our_secs,
                                sizeof(ble_store_config_our_secs[0]),
                                ble_store_config_num_our_secs);
        }
        if (dirty & BOND_DIRTY_PEER_SECS) {
            rc |= persist_array(BOND_KEY_PEER_SECS, ble_store_config_peer_secs,
                                sizeof(ble_store_config_peer_secs[0]),
                                ble_store_config_num_peer_secs);
        }
        if (dirty & BOND_DIRTY_CCCDS) {
            rc |= persist_array(BOND_KEY_CCCDS, ble_store_config_cccds,
                                sizeof(ble_store_config_cccds[0]), ble_store_config_num_cccds);
        }
        if (dirty && rc == 0) {
            ESP_LOGI(TAG, "bonds flushed to flash");
        }
    }
}

int ble_store_config_persist_our_secs(void) {
    mark_dirty(BOND_DIRTY_OUR_SECS);
    return 0;
}

int ble_store_config_persist_peer_secs(void) {
    mark_dirty(BOND_DIRTY_PEER_SECS);
    return 0;
}

int ble_store_config_persist_cccds(void) {
    mark_dirty(BOND_DIRTY_CCCDS);
    return 0;
}

void ble_store_config_conf_init(void) {
    nvs_handle_t h;
    if (bond_open(NVS_READONLY, &h) != ESP_OK) {
        /* First boot, or a settings partition that has not been formatted
         * yet. Nothing stored is not an error; the node simply has no bonds. */
        return;
    }

    load_array(h, BOND_KEY_OUR_SECS, ble_store_config_our_secs,
               sizeof(ble_store_config_our_secs[0]), MYNEWT_VAL(BLE_STORE_MAX_BONDS),
               &ble_store_config_num_our_secs);
    load_array(h, BOND_KEY_PEER_SECS, ble_store_config_peer_secs,
               sizeof(ble_store_config_peer_secs[0]), MYNEWT_VAL(BLE_STORE_MAX_BONDS),
               &ble_store_config_num_peer_secs);
    load_array(h, BOND_KEY_CCCDS, ble_store_config_cccds, sizeof(ble_store_config_cccds[0]),
               MYNEWT_VAL(BLE_STORE_MAX_CCCDS), &ble_store_config_num_cccds);
    nvs_close(h);

    /* Priority 1: below everything that matters, because its whole job is to
     * touch flash only when the system has nothing better to do. */
    if (xTaskCreate(bond_writer_task, "ble_bond", 2048, NULL, 1, &s_writer) != pdPASS) {
        ESP_LOGE(TAG, "bond writer task not created; bonds will not persist");
        s_writer = NULL;
    }

    ESP_LOGI(TAG, "bonds restored: %d peer, %d cccd", ble_store_config_num_peer_secs,
             ble_store_config_num_cccds);
}

void ble_store_nvs_start_writer(void) {
    if (s_writer != NULL) {
        return;
    }
    /*
     * Started here rather than from ble_store_config_init: NimBLE calls that
     * during host bring-up, when the heap is at its tightest of the whole
     * boot (creating even a 2KB task there tripped the malloc-failed hook and
     * halted the node). By the time BLE is up the heap has settled.
     *
     * Priority 1 keeps flash work below everything that matters.
     */
    if (xTaskCreate(bond_writer_task, "ble_bond", 2048, NULL, 1, &s_writer) != pdPASS) {
        ESP_LOGE(TAG, "bond writer task not created; bonds will not persist");
        s_writer = NULL;
    }
}
