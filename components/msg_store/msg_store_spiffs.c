#include "include/msg_store_spiffs.h"
#include "include/msg_store.h"
#include <string.h>
#include <inttypes.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_spiffs.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define TAG "msg_spiffs"
#define MSG_FILE_PATH "/spiffs/messages.bin"
#define MSG_FILE_MAGIC 0x4252414D /* "BRAM" */
#define MSG_FILE_VERSION 1

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t record_size;
    uint32_t record_count;
    uint32_t next_id;
} __attribute__((packed)) msg_file_header_t;

static FILE* s_msg_file = NULL;
static msg_file_header_t s_header;
static bool s_initialized = false;

/* One record of scratch for the read-modify-write in msg_store_spiffs_update.
 * Static, not stack: a record is ~700 bytes and the callers run on the mesh
 * task alongside the radio. Writers are single-task, so no two uses overlap. */
static stored_msg_t s_record_scratch;

int msg_store_spiffs_init(void) {
    /* Check if SPIFFS is mounted. Must be esp_spiffs_mounted(), never
     * stat("/spiffs"): SPIFFS is flat, the VFS has no root directory entry,
     * so stat() on the bare mountpoint fails EVEN WHEN MOUNTED. That check
     * disabled persistence on every real-hardware boot since the feature
     * shipped (boot log: "SPIFFS mounted: used=0" immediately followed by
     * "SPIFFS not mounted, persistence disabled"). NULL = the default
     * partition label main.c registers with. */
    if (!esp_spiffs_mounted(NULL)) {
        ESP_LOGW(TAG, "SPIFFS not mounted, persistence disabled");
        return -1;
    }

    /* Try to open existing file */
    s_msg_file = fopen(MSG_FILE_PATH, "r+b");
    if (s_msg_file) {
        /* Read and validate header */
        if (fread(&s_header, sizeof(s_header), 1, s_msg_file) != 1 ||
            s_header.magic != MSG_FILE_MAGIC || s_header.version != MSG_FILE_VERSION ||
            s_header.record_size != sizeof(stored_msg_t)) {
            ESP_LOGW(TAG, "Corrupted message file, clearing");
            fclose(s_msg_file);
            s_msg_file = NULL;
            unlink(MSG_FILE_PATH);
        } else {
            /* The record count is derived from the FILE SIZE, never trusted
             * from the header: a record append and its header update are two
             * separate flash writes, so a crash (or the pre-fsync firmware)
             * can leave records the header does not count. Counting from the
             * size keeps every fully-written record visible, and load_recent's
             * offset math stays aligned with reality. A torn trailing record
             * (partial append) is truncated away so the next append cannot
             * land misaligned after it. */
            long size = 0;
            if (fseek(s_msg_file, 0, SEEK_END) == 0)
                size = ftell(s_msg_file);
            long payload = (size > (long)sizeof(s_header)) ? size - (long)sizeof(s_header) : 0;
            uint32_t actual = (uint32_t)(payload / (long)sizeof(stored_msg_t));
            long aligned = (long)sizeof(s_header) + (long)actual * (long)sizeof(stored_msg_t);
            if (size != aligned) {
                ESP_LOGW(TAG, "Truncating torn trailing record (%ld -> %ld bytes)", size, aligned);
                if (ftruncate(fileno(s_msg_file), aligned) != 0)
                    ESP_LOGW(TAG, "ftruncate failed");
            }
            if (actual != s_header.record_count) {
                ESP_LOGW(TAG,
                         "Header counted %" PRIu32 " records, file holds %" PRIu32
                         "; using the file",
                         s_header.record_count, actual);
                s_header.record_count = actual;
            }
            ESP_LOGI(TAG, "Loaded message file: %" PRIu32 " messages", s_header.record_count);
            s_initialized = true;
            return 0;
        }
    }

    /* Create new file */
    s_msg_file = fopen(MSG_FILE_PATH, "w+b");
    if (!s_msg_file) {
        ESP_LOGE(TAG, "Failed to create message file");
        return -1;
    }

    /* Write header */
    memset(&s_header, 0, sizeof(s_header));
    s_header.magic = MSG_FILE_MAGIC;
    s_header.version = MSG_FILE_VERSION;
    s_header.record_size = sizeof(stored_msg_t);
    s_header.record_count = 0;
    s_header.next_id = 1;

    if (fwrite(&s_header, sizeof(s_header), 1, s_msg_file) != 1) {
        ESP_LOGE(TAG, "Failed to write header");
        fclose(s_msg_file);
        s_msg_file = NULL;
        return -1;
    }

    fflush(s_msg_file);
    fsync(fileno(s_msg_file)); /* see msg_store_spiffs_save: durability needs fsync */
    s_initialized = true;
    ESP_LOGI(TAG, "Created new message file");
    return 0;
}

int msg_store_spiffs_save(const stored_msg_t* msg) {
    if (!s_initialized || !s_msg_file || !msg) {
        return -1;
    }

    /* Seek to end of file */
    if (fseek(s_msg_file, 0, SEEK_END) != 0) {
        ESP_LOGE(TAG, "fseek failed");
        return -1;
    }

    /* Append message */
    if (fwrite(msg, sizeof(stored_msg_t), 1, s_msg_file) != 1) {
        ESP_LOGE(TAG, "Failed to write message");
        return -1;
    }

    /* Update header */
    s_header.record_count++;
    s_header.next_id++;

    if (fseek(s_msg_file, 0, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "fseek to header failed");
        return -1;
    }

    if (fwrite(&s_header, sizeof(s_header), 1, s_msg_file) != 1) {
        ESP_LOGE(TAG, "Failed to update header");
        return -1;
    }

    /* fflush only drains the stdio buffer into VFS writes; the SPIFFS write
     * cache (CONFIG_SPIFFS_CACHE_WR) can still hold the pages in RAM, and a
     * reboot then loses them. Observed exactly here: the appended record
     * reached flash but this in-place header update did not, so every boot
     * restored "0 messages". fsync forces the cache to flash. */
    fflush(s_msg_file);
    if (fsync(fileno(s_msg_file)) != 0) {
        ESP_LOGE(TAG, "fsync failed");
        return -1;
    }
    return 0;
}

int msg_store_spiffs_update(int from_end, const stored_msg_t* msg) {
    if (!s_initialized || !s_msg_file || !msg) {
        return -1;
    }
    if (from_end < 0 || (uint32_t)from_end >= s_header.record_count) {
        return -1;
    }

    long index = (long)s_header.record_count - 1 - (long)from_end;
    long offset = (long)sizeof(msg_file_header_t) + index * (long)sizeof(stored_msg_t);

    /* Read the record back and confirm it is the message the caller means
     * before overwriting it: see msg_store_spiffs_update's contract. */
    if (fseek(s_msg_file, offset, SEEK_SET) != 0 ||
        fread(&s_record_scratch, sizeof(s_record_scratch), 1, s_msg_file) != 1) {
        ESP_LOGW(TAG, "Update: record %ld unreadable", index);
        return -1;
    }
    if (!msg_store_record_matches(&s_record_scratch, msg)) {
        ESP_LOGW(TAG, "Update: record %ld is a different message, skipping", index);
        return -1;
    }

    uint32_t persisted_ts = s_record_scratch.timestamp_s;
    s_record_scratch = *msg;
    s_record_scratch.timestamp_s = persisted_ts;

    if (fseek(s_msg_file, offset, SEEK_SET) != 0 ||
        fwrite(&s_record_scratch, sizeof(s_record_scratch), 1, s_msg_file) != 1) {
        ESP_LOGE(TAG, "Update: failed to rewrite record %ld", index);
        return -1;
    }

    /* The record count does not change, so the header needs no update; only
     * the record itself has to reach flash. fsync for the same reason
     * msg_store_spiffs_save does: fflush alone leaves it in the write cache. */
    fflush(s_msg_file);
    if (fsync(fileno(s_msg_file)) != 0) {
        ESP_LOGE(TAG, "Update: fsync failed");
        return -1;
    }
    return 0;
}

int msg_store_spiffs_get_count(void) { return s_initialized ? s_header.record_count : 0; }

int msg_store_spiffs_load_recent(stored_msg_t* msgs, int max_count) {
    if (!s_initialized || !s_msg_file || !msgs || max_count <= 0) {
        return 0;
    }

    /* Calculate how many messages to load (most recent ones) */
    int total = s_header.record_count;
    int to_load = (total < max_count) ? total : max_count;

    if (to_load == 0) {
        return 0;
    }

    /* Calculate offset to start reading from (skip older messages) */
    int skip_count = total - to_load;
    long offset = sizeof(msg_file_header_t) + skip_count * sizeof(stored_msg_t);

    /* Seek to start of recent messages */
    if (fseek(s_msg_file, offset, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "fseek to records failed");
        return 0;
    }

    /* Read messages */
    int loaded = fread(msgs, sizeof(stored_msg_t), to_load, s_msg_file);

    ESP_LOGI(TAG, "Loaded %d recent messages (requested %d, available %" PRIu32 ")", loaded,
             max_count, s_header.record_count);
    return loaded;
}

void msg_store_spiffs_rollover(int max_messages, int keep_pct) {
    if (!s_initialized || !s_msg_file || s_header.record_count <= max_messages) {
        return; /* No rollover needed */
    }

    ESP_LOGI(TAG, "Rolling over message file: %" PRIu32 " -> %d messages", s_header.record_count,
             max_messages);

    /* Validate keep percentage */
    if (keep_pct < 50 || keep_pct > 90) {
        ESP_LOGW(TAG, "Invalid keep_pct %d, using 75", keep_pct);
        keep_pct = 75;
    }

    int keep_count = (max_messages * keep_pct) / 100;
    int skip_count = s_header.record_count - keep_count;

    /* Allocate temp buffer for messages to keep */
    stored_msg_t* keep_msgs = malloc(keep_count * sizeof(stored_msg_t));
    if (!keep_msgs) {
        ESP_LOGE(TAG, "malloc failed for rollover");
        return;
    }

    /* Seek to first message to keep */
    long offset = sizeof(msg_file_header_t) + skip_count * sizeof(stored_msg_t);
    if (fseek(s_msg_file, offset, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "fseek failed in rollover");
        free(keep_msgs);
        return;
    }

    /* Read messages to keep */
    int loaded = fread(keep_msgs, sizeof(stored_msg_t), keep_count, s_msg_file);
    if (loaded != keep_count) {
        ESP_LOGW(TAG, "Only loaded %d/%d messages for rollover", loaded, keep_count);
        keep_count = loaded;
    }

    /* Close and reopen file for writing */
    fclose(s_msg_file);
    s_msg_file = fopen(MSG_FILE_PATH, "w+b");
    if (!s_msg_file) {
        ESP_LOGE(TAG, "Failed to reopen file for rollover");
        free(keep_msgs);
        s_initialized = false;
        return;
    }

    /* Write new header */
    s_header.record_count = keep_count;
    if (fwrite(&s_header, sizeof(s_header), 1, s_msg_file) != 1) {
        ESP_LOGE(TAG, "Failed to write header in rollover");
        free(keep_msgs);
        return;
    }

    /* Write kept messages */
    if (fwrite(keep_msgs, sizeof(stored_msg_t), keep_count, s_msg_file) != keep_count) {
        ESP_LOGE(TAG, "Failed to write messages in rollover");
    }

    fflush(s_msg_file);
    fsync(fileno(s_msg_file)); /* see msg_store_spiffs_save: durability needs fsync */
    free(keep_msgs);

    ESP_LOGI(TAG, "Rollover complete: %d messages retained", keep_count);
}

void msg_store_spiffs_clear(void) {
    if (s_msg_file) {
        fclose(s_msg_file);
        s_msg_file = NULL;
    }
    unlink(MSG_FILE_PATH);
    s_initialized = false;
    ESP_LOGI(TAG, "Cleared message file");
}

#else /* Host stubs for unit tests */

int msg_store_spiffs_init(void) { return -1; }
int msg_store_spiffs_save(const stored_msg_t* msg) {
    (void)msg;
    return -1;
}
int msg_store_spiffs_update(int from_end, const stored_msg_t* msg) {
    (void)from_end;
    (void)msg;
    return -1;
}
int msg_store_spiffs_get_count(void) { return 0; }
int msg_store_spiffs_load_recent(stored_msg_t* msgs, int max_count) {
    (void)msgs;
    (void)max_count;
    return 0;
}
void msg_store_spiffs_rollover(int max_messages, int keep_pct) {
    (void)max_messages;
    (void)keep_pct;
}
void msg_store_spiffs_clear(void) {}

#endif /* ESP_PLATFORM */
