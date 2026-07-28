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

/* Magic then version, the shape msg_store_lfs.c's msg_file_header_t already
 * uses. The magic earns its four bytes here: without it any >=6-byte blob
 * whose first bytes happen to read as the version is accepted, and for this
 * record that means a bond that loads and then silently fails to decrypt,
 * which is exactly what the version check exists to prevent. Bump the version
 * whenever the on-flash layout changes; mismatches are dropped rather than
 * reinterpreted, because a re-pair is far easier to diagnose than a spliced
 * LTK. */
#define BOND_BLOB_MAGIC 0x424F4E44 /* "BOND" */
#define BOND_BLOB_VERSION 1

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t elem_size;
    uint16_t count;
} bond_blob_hdr_t;

/* One stack buffer serves all three records, so size it for the largest.
 * peer_secs is not in the comparison because it is the same type and the same
 * capacity as our_secs; both are ble_store_value_sec[BLE_STORE_MAX_BONDS]. */
#define BOND_BLOB_MAX_PAYLOAD                                                                      \
    (sizeof(ble_store_config_our_secs) > sizeof(ble_store_config_cccds)                            \
         ? sizeof(ble_store_config_our_secs)                                                       \
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
            .magic = BOND_BLOB_MAGIC,
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
    if (hdr.magic != BOND_BLOB_MAGIC || hdr.version != BOND_BLOB_VERSION ||
        hdr.elem_size != elem_size) {
        ESP_LOGW(TAG, "%s: dropping unrecognised record (magic %08x, v%u, elem %u)", key,
                 (unsigned)hdr.magic, (unsigned)hdr.version, (unsigned)hdr.elem_size);
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
 * the same breath as the encryption handshake, so writing flash inside them
 * blocks the one task that has to keep servicing the host queue while the
 * link is at its most timing-sensitive. That is what the deferral fixes, and
 * it is specific to these hooks: no other writer in the tree runs on the
 * host task.
 *
 * It does NOT fix the other half of the problem. NVMC programming stalls the
 * CPU for as long as it runs, which threatens radio timing no matter which
 * task started the write, and the message store, the replay table and the
 * identity pins all write flash from other tasks while a link may be up.
 * A general answer belongs in the port's block device (nrf/src/lfs_nvmc.c
 * already slices erases and could slice programs the same way, and
 * shim/nvs_lfs.c funnels every NVS write through one mutex, so there is
 * exactly one choke point to put it in). Until that exists, this is one
 * writer keeping itself out of the way, not flash-versus-radio coordination.
 *
 * The values themselves already live in ble_store_config's RAM arrays, which
 * is what NimBLE reads back, so nothing depends on the write having landed.
 */
#define BOND_DIRTY_OUR_SECS (1u << 0)
#define BOND_DIRTY_PEER_SECS (1u << 1)
#define BOND_DIRTY_CCCDS (1u << 2)

/* One row per record, so a record's key, element size and capacity are stated
 * once instead of once in the writer and again in the loader. A capacity that
 * disagreed between the two would be invisible by inspection. */
static const struct bond_record {
    uint32_t dirty_bit;
    const char* key;
    void* array;
    size_t elem_size;
    int* count;
    int max;
} s_bond_records[] = {
    {BOND_DIRTY_OUR_SECS, BLE_KEY_OUR_SECS, ble_store_config_our_secs,
     sizeof(ble_store_config_our_secs[0]), &ble_store_config_num_our_secs,
     MYNEWT_VAL(BLE_STORE_MAX_BONDS)},
    {BOND_DIRTY_PEER_SECS, BLE_KEY_PEER_SECS, ble_store_config_peer_secs,
     sizeof(ble_store_config_peer_secs[0]), &ble_store_config_num_peer_secs,
     MYNEWT_VAL(BLE_STORE_MAX_BONDS)},
    {BOND_DIRTY_CCCDS, BLE_KEY_CCCDS, ble_store_config_cccds, sizeof(ble_store_config_cccds[0]),
     &ble_store_config_num_cccds, MYNEWT_VAL(BLE_STORE_MAX_CCCDS)},
};

#define BOND_RECORD_COUNT (sizeof(s_bond_records) / sizeof(s_bond_records[0]))

/* How long the flush waits after the last change. This is a settle timer, not
 * a radio-idle check: it observes nothing about the link, it just puts the
 * write far enough past the handshake. Long enough for pairing and the
 * client's first RPC exchange to finish, short enough that a user who pairs
 * and immediately unplugs the node keeps the bond. */
#define BOND_FLUSH_SETTLE_MS 3000

static volatile uint32_t s_dirty;
static TaskHandle_t s_writer;

/* Change detection, so an unchanged record is not rewritten. NimBLE calls the
 * persist hooks unconditionally: ble_gatts_bonding_restored fires on every
 * reconnect of an already-bonded peer, so without this a phone that reconnects
 * hourly rewrites a byte-identical CCCD blob hourly. Each rewrite is a
 * littlefs commit, and roughly every 30-40 commits forces a metadata
 * compaction, which erases a page as ~81 partial-erase slices with the CPU
 * stalled inside each one. Not integrity: a 32-bit FNV-1a is plenty to answer
 * "did these bytes change", which is why this is not the CRC replay_window.c
 * uses to validate its records. */
static uint32_t s_last_hash[BOND_RECORD_COUNT];
static bool s_hash_valid[BOND_RECORD_COUNT];

static uint32_t bond_hash(const uint8_t* data, size_t len, int count) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h = (h ^ data[i]) * 16777619u;
    }
    /* Fold the count in so emptying a record cannot collide with its
     * previous contents. */
    return (h ^ (uint32_t)count) * 16777619u;
}

static void mark_dirty(uint32_t bit) {
    taskENTER_CRITICAL();
    s_dirty |= bit;
    taskEXIT_CRITICAL();
    if (s_writer) {
        xTaskNotifyGive(s_writer);
    }
}

/*
 * Copies a record out from under NimBLE before it goes to flash. The arrays
 * belong to the host task and it mutates them there; reading them straight
 * into a flash write can tear, and a power cut inside the settle window would
 * then leave a record of the right version and the right length holding a
 * spliced LTK. load_array cannot tell that from a good one, so it would come
 * back as a bond that silently refuses to decrypt. Copying under the same
 * lock that clears the dirty bit costs a couple of microseconds.
 */
static int snapshot_record(void* dst, const void* src, size_t elem_size, const int* count_src,
                           int max) {
    taskENTER_CRITICAL();
    int n = *count_src;
    if (n < 0) {
        n = 0;
    } else if (n > max) {
        n = max;
    }
    memcpy(dst, src, (size_t)n * elem_size);
    taskEXIT_CRITICAL();
    return n;
}

static void bond_writer_task(void* arg) {
    (void)arg;
    uint8_t snap[BOND_BLOB_MAX_PAYLOAD];
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
        for (size_t i = 0; i < BOND_RECORD_COUNT; i++) {
            const struct bond_record* r = &s_bond_records[i];
            if (!(dirty & r->dirty_bit)) {
                continue;
            }
            int n = snapshot_record(snap, r->array, r->elem_size, r->count, r->max);
            uint32_t h = bond_hash(snap, (size_t)n * r->elem_size, n);
            if (s_hash_valid[i] && h == s_last_hash[i]) {
                continue; /* byte-identical; do not spend a flash commit */
            }
            int one = persist_array(r->key, snap, r->elem_size, n);
            rc |= one;
            if (one == 0) {
                s_last_hash[i] = h;
                s_hash_valid[i] = true;
            }
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

    for (size_t i = 0; i < BOND_RECORD_COUNT; i++) {
        const struct bond_record* r = &s_bond_records[i];
        load_array(h, r->key, r->array, r->elem_size, r->max, r->count);
    }
    nvs_close(h);

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
     * Priority 1 keeps flash work below everything that matters. The depth is
     * in WORDS, not bytes: this file includes the kernel's <task.h> directly
     * rather than the shim that gives shared ESP-IDF code its byte semantics,
     * so 1024 here is 4KB. Measured peak through a real bond flush on the
     * WM1110 was 360 words (1440 bytes), leaving the rest as margin for the
     * LittleFS compaction paths that flush did not exercise.
     */
    if (xTaskCreate(bond_writer_task, "ble_bond", 1024, NULL, 1, &s_writer) != pdPASS) {
        ESP_LOGE(TAG, "bond writer task not created; bonds will not persist");
        s_writer = NULL;
        return;
    }
    /* Anything marked dirty before the writer existed got no notification, so
     * hand it one now rather than leaving that record waiting for the next
     * unrelated change. Nothing can pair this early today (advertising starts
     * on sync, after this), which is exactly why the invariant is worth making
     * explicit instead of leaving it resting on call order. */
    if (s_dirty != 0) {
        xTaskNotifyGive(s_writer);
    }
}
