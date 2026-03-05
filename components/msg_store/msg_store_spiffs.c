#include "include/msg_store_spiffs.h"
#include "include/msg_store.h"
#include <string.h>
#include <inttypes.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_spiffs.h"
#include <stdio.h>
#include <sys/stat.h>
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

int msg_store_spiffs_init(void) {
    /* Check if SPIFFS is mounted */
    struct stat st;
    if (stat("/spiffs", &st) != 0) {
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

    fflush(s_msg_file);
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
