/*
 * Message-store persistence on LittleFS, for the nRF52840 target.
 *
 * Implements the same msg_store_spiffs.h contract as the ESP SPIFFS backend
 * and keeps its two hard-won invariants:
 *
 *  1. The record count comes from the FILE SIZE, never from the header. A
 *     record append and its header update are separate flash writes, so a
 *     crash can leave records the header does not count; deriving from size
 *     keeps every fully written record visible. A torn trailing record is
 *     truncated so the next append cannot land misaligned.
 *  2. Every write is followed by an explicit sync. On SPIFFS a missing fsync
 *     made the header update evaporate and every boot restore zero messages;
 *     littlefs buffers in exactly the same way, so lfs_file_sync sits at the
 *     same points.
 *
 * Difference from the SPIFFS backend: rollover streams records through a
 * small stack buffer instead of mallocing the whole keep-set (which would be
 * ~32KB here, larger than this target's entire libc heap).
 */
#ifdef BRAMBLE_PLATFORM_NRF

#include "include/msg_store.h"
#include "include/msg_store_spiffs.h"

#include <string.h>

#include "esp_log.h"
#include "lfs.h"
#include "lfs_nvmc.h"
#include "nvs_lfs_mount.h"

#define TAG "msg_lfs"
#define MSG_FILE_PATH "/messages.bin"
#define MSG_FILE_MAGIC 0x4252414D /* "BRAM" */
#define MSG_FILE_VERSION 1

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t record_size;
    uint32_t record_count;
    uint32_t next_id;
} __attribute__((packed)) msg_file_header_t;

static msg_file_header_t s_header;
static bool s_initialized;
static lfs_file_t s_file;
static uint8_t s_file_buffer[LFS_NVMC_CACHE_SIZE];
static const struct lfs_file_config s_file_cfg = {.buffer = s_file_buffer};

/* One record (712 bytes) of scratch, static rather than stack because this
 * target has 256KB in total and no PSRAM. Shared by the streaming rollover
 * and the in-place update: every public entry point holds nvs_lfs_lock() for
 * its whole body, so the two can never be in flight at once. */
static stored_msg_t s_record_scratch;

static lfs_t* fs(void) { return nvs_lfs_handle(); }

static int write_header(void) {
    if (lfs_file_seek(fs(), &s_file, 0, LFS_SEEK_SET) < 0) {
        return -1;
    }
    if (lfs_file_write(fs(), &s_file, &s_header, sizeof(s_header)) !=
        (lfs_ssize_t)sizeof(s_header)) {
        return -1;
    }
    return lfs_file_sync(fs(), &s_file) == LFS_ERR_OK ? 0 : -1;
}

static int init_unlocked(void) {
    if (fs() == NULL) {
        ESP_LOGW(TAG, "filesystem not mounted, persistence disabled");
        return -1;
    }

    int rc = lfs_file_opencfg(fs(), &s_file, MSG_FILE_PATH, LFS_O_RDWR, &s_file_cfg);
    if (rc == LFS_ERR_OK) {
        bool header_ok = lfs_file_read(fs(), &s_file, &s_header, sizeof(s_header)) ==
                             (lfs_ssize_t)sizeof(s_header) &&
                         s_header.magic == MSG_FILE_MAGIC && s_header.version == MSG_FILE_VERSION &&
                         s_header.record_size == sizeof(stored_msg_t);
        if (!header_ok) {
            ESP_LOGW(TAG, "corrupted message file, clearing");
            lfs_file_close(fs(), &s_file);
            lfs_remove(fs(), MSG_FILE_PATH);
        } else {
            /* Invariant 1: trust the file size, not the header. */
            lfs_soff_t size = lfs_file_size(fs(), &s_file);
            lfs_soff_t payload =
                size > (lfs_soff_t)sizeof(s_header) ? size - (lfs_soff_t)sizeof(s_header) : 0;
            uint32_t actual = (uint32_t)(payload / (lfs_soff_t)sizeof(stored_msg_t));
            lfs_soff_t aligned = (lfs_soff_t)sizeof(s_header) +
                                 (lfs_soff_t)actual * (lfs_soff_t)sizeof(stored_msg_t);
            if (size != aligned) {
                ESP_LOGW(TAG, "truncating torn trailing record (%ld -> %ld bytes)", (long)size,
                         (long)aligned);
                if (lfs_file_truncate(fs(), &s_file, aligned) != LFS_ERR_OK) {
                    ESP_LOGW(TAG, "truncate failed");
                }
            }
            if (actual != s_header.record_count) {
                ESP_LOGW(TAG, "header counted %lu records, file holds %lu; using the file",
                         (unsigned long)s_header.record_count, (unsigned long)actual);
                s_header.record_count = actual;
            }
            ESP_LOGI(TAG, "loaded message file: %lu messages",
                     (unsigned long)s_header.record_count);
            s_initialized = true;
            return 0;
        }
    }

    if (lfs_file_opencfg(fs(), &s_file, MSG_FILE_PATH, LFS_O_RDWR | LFS_O_CREAT | LFS_O_TRUNC,
                         &s_file_cfg) != LFS_ERR_OK) {
        ESP_LOGE(TAG, "failed to create message file");
        return -1;
    }
    memset(&s_header, 0, sizeof(s_header));
    s_header.magic = MSG_FILE_MAGIC;
    s_header.version = MSG_FILE_VERSION;
    s_header.record_size = sizeof(stored_msg_t);
    s_header.next_id = 1;
    if (write_header() != 0) {
        ESP_LOGE(TAG, "failed to write header");
        lfs_file_close(fs(), &s_file);
        return -1;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "created new message file");
    return 0;
}

static int save_unlocked(const stored_msg_t* msg) {
    if (!s_initialized || msg == NULL) {
        return -1;
    }
    if (lfs_file_seek(fs(), &s_file, 0, LFS_SEEK_END) < 0) {
        return -1;
    }
    if (lfs_file_write(fs(), &s_file, msg, sizeof(*msg)) != (lfs_ssize_t)sizeof(*msg)) {
        ESP_LOGE(TAG, "failed to write message");
        return -1;
    }
    /* Invariant 2: the record must be durable before the header claims it. */
    if (lfs_file_sync(fs(), &s_file) != LFS_ERR_OK) {
        return -1;
    }
    s_header.record_count++;
    s_header.next_id++;
    return write_header();
}

static int update_unlocked(int from_end, const stored_msg_t* msg) {
    if (!s_initialized || msg == NULL) {
        return -1;
    }
    if (from_end < 0 || (uint32_t)from_end >= s_header.record_count) {
        return -1;
    }

    lfs_soff_t index = (lfs_soff_t)s_header.record_count - 1 - (lfs_soff_t)from_end;
    lfs_soff_t offset =
        (lfs_soff_t)sizeof(msg_file_header_t) + index * (lfs_soff_t)sizeof(stored_msg_t);

    /* Read the record back and confirm it is the message the caller means
     * before overwriting it: see msg_store_spiffs_update's contract. */
    if (lfs_file_seek(fs(), &s_file, offset, LFS_SEEK_SET) < 0 ||
        lfs_file_read(fs(), &s_file, &s_record_scratch, sizeof(s_record_scratch)) !=
            (lfs_ssize_t)sizeof(s_record_scratch)) {
        ESP_LOGW(TAG, "update: record %ld unreadable", (long)index);
        return -1;
    }
    if (!msg_store_record_matches(&s_record_scratch, msg)) {
        ESP_LOGW(TAG, "update: record %ld is a different message, skipping", (long)index);
        return -1;
    }

    uint32_t persisted_ts = s_record_scratch.timestamp_s;
    s_record_scratch = *msg;
    s_record_scratch.timestamp_s = persisted_ts;

    if (lfs_file_seek(fs(), &s_file, offset, LFS_SEEK_SET) < 0 ||
        lfs_file_write(fs(), &s_file, &s_record_scratch, sizeof(s_record_scratch)) !=
            (lfs_ssize_t)sizeof(s_record_scratch)) {
        ESP_LOGE(TAG, "update: failed to rewrite record %ld", (long)index);
        return -1;
    }
    /* The record count does not change, so the header stays as it is; only
     * this record has to become durable (invariant 2). */
    return lfs_file_sync(fs(), &s_file) == LFS_ERR_OK ? 0 : -1;
}

int msg_store_spiffs_get_count(void) { return s_initialized ? (int)s_header.record_count : 0; }

static int load_recent_unlocked(stored_msg_t* msgs, int max_count) {
    if (!s_initialized || msgs == NULL || max_count <= 0) {
        return 0;
    }
    int total = (int)s_header.record_count;
    int to_load = total < max_count ? total : max_count;
    if (to_load == 0) {
        return 0;
    }
    int skip_count = total - to_load;
    lfs_soff_t offset =
        (lfs_soff_t)sizeof(msg_file_header_t) + (lfs_soff_t)skip_count * sizeof(stored_msg_t);
    if (lfs_file_seek(fs(), &s_file, offset, LFS_SEEK_SET) < 0) {
        return 0;
    }
    lfs_size_t want = (lfs_size_t)to_load * sizeof(stored_msg_t);
    lfs_ssize_t got = lfs_file_read(fs(), &s_file, msgs, want);
    int loaded = got > 0 ? (int)((lfs_size_t)got / sizeof(stored_msg_t)) : 0;
    ESP_LOGI(TAG, "loaded %d recent messages (requested %d, available %lu)", loaded, max_count,
             (unsigned long)s_header.record_count);
    return loaded;
}

static void rollover_unlocked(int max_messages, int keep_pct) {
    if (!s_initialized || (int)s_header.record_count <= max_messages) {
        return;
    }
    if (keep_pct < 50 || keep_pct > 90) {
        ESP_LOGW(TAG, "invalid keep_pct %d, using 75", keep_pct);
        keep_pct = 75;
    }
    int keep_count = (max_messages * keep_pct) / 100;
    int skip_count = (int)s_header.record_count - keep_count;
    ESP_LOGI(TAG, "rolling over message file: %lu -> %d messages",
             (unsigned long)s_header.record_count, keep_count);

    /* Stream record by record into a fresh file: the SPIFFS backend mallocs
     * the whole keep-set, which would be ~32KB here. One record of scratch
     * (s_record_scratch) is the price of not needing a heap at all. */
    lfs_file_t out;
    static uint8_t out_buffer[LFS_NVMC_CACHE_SIZE];
    static const struct lfs_file_config out_cfg = {.buffer = out_buffer};
    if (lfs_file_opencfg(fs(), &out, MSG_FILE_PATH ".tmp", LFS_O_RDWR | LFS_O_CREAT | LFS_O_TRUNC,
                         &out_cfg) != LFS_ERR_OK) {
        ESP_LOGE(TAG, "rollover could not open temp file");
        return;
    }
    msg_file_header_t new_header = s_header;
    new_header.record_count = 0;
    bool ok = lfs_file_write(fs(), &out, &new_header, sizeof(new_header)) ==
              (lfs_ssize_t)sizeof(new_header);

    for (int i = 0; ok && i < keep_count; i++) {
        lfs_soff_t offset = (lfs_soff_t)sizeof(msg_file_header_t) +
                            (lfs_soff_t)(skip_count + i) * sizeof(stored_msg_t);
        if (lfs_file_seek(fs(), &s_file, offset, LFS_SEEK_SET) < 0 ||
            lfs_file_read(fs(), &s_file, &s_record_scratch, sizeof(s_record_scratch)) !=
                (lfs_ssize_t)sizeof(s_record_scratch)) {
            ok = false;
            break;
        }
        ok = lfs_file_write(fs(), &out, &s_record_scratch, sizeof(s_record_scratch)) ==
             (lfs_ssize_t)sizeof(s_record_scratch);
        new_header.record_count++;
    }
    if (ok) {
        ok = lfs_file_seek(fs(), &out, 0, LFS_SEEK_SET) >= 0 &&
             lfs_file_write(fs(), &out, &new_header, sizeof(new_header)) ==
                 (lfs_ssize_t)sizeof(new_header) &&
             lfs_file_sync(fs(), &out) == LFS_ERR_OK;
    }
    lfs_file_close(fs(), &out);
    if (!ok) {
        ESP_LOGE(TAG, "rollover failed, keeping the original file");
        lfs_remove(fs(), MSG_FILE_PATH ".tmp");
        return;
    }
    /* Swap: the temp file is complete and synced before the original dies. */
    lfs_file_close(fs(), &s_file);
    lfs_remove(fs(), MSG_FILE_PATH);
    if (lfs_rename(fs(), MSG_FILE_PATH ".tmp", MSG_FILE_PATH) != LFS_ERR_OK) {
        ESP_LOGE(TAG, "rollover rename failed, persistence disabled until reboot");
        s_initialized = false;
        return;
    }
    if (lfs_file_opencfg(fs(), &s_file, MSG_FILE_PATH, LFS_O_RDWR, &s_file_cfg) != LFS_ERR_OK) {
        ESP_LOGE(TAG, "rollover could not reopen the store");
        s_initialized = false;
        return;
    }
    s_header = new_header;
    ESP_LOGI(TAG, "rollover complete: %lu messages retained", (unsigned long)s_header.record_count);
}

static void clear_unlocked(void) {
    if (s_initialized) {
        lfs_file_close(fs(), &s_file);
        s_initialized = false;
    }
    if (fs() != NULL) {
        lfs_remove(fs(), MSG_FILE_PATH);
    }
    ESP_LOGI(TAG, "cleared message file");
}

/*
 * Public entry points: every one takes the filesystem lock for its whole
 * body. littlefs is not thread-safe, this file shares one lfs_t (and one set
 * of block-device cache buffers, see nrf/src/lfs_nvmc.c) with the NVS shim,
 * and three tasks reach that filesystem concurrently on this target: the
 * mesh task appends messages here, the BLE RPC task writes settings through
 * the locked nvs_* API, and the bond writer flushes on its own schedule.
 * Found by review, not by the bench: the exit-gate soak never happened to
 * interleave a message append with a settings write.
 */
int msg_store_spiffs_init(void) {
    nvs_lfs_lock();
    int rc = init_unlocked();
    nvs_lfs_unlock();
    return rc;
}

int msg_store_spiffs_save(const stored_msg_t* msg) {
    nvs_lfs_lock();
    int rc = save_unlocked(msg);
    nvs_lfs_unlock();
    return rc;
}

int msg_store_spiffs_update(int from_end, const stored_msg_t* msg) {
    nvs_lfs_lock();
    int rc = update_unlocked(from_end, msg);
    nvs_lfs_unlock();
    return rc;
}

int msg_store_spiffs_load_recent(stored_msg_t* msgs, int max_count) {
    nvs_lfs_lock();
    int rc = load_recent_unlocked(msgs, max_count);
    nvs_lfs_unlock();
    return rc;
}

void msg_store_spiffs_rollover(int max_messages, int keep_pct) {
    nvs_lfs_lock();
    rollover_unlocked(max_messages, keep_pct);
    nvs_lfs_unlock();
}

void msg_store_spiffs_clear(void) {
    nvs_lfs_lock();
    clear_unlocked();
    nvs_lfs_unlock();
}

#endif /* BRAMBLE_PLATFORM_NRF */
