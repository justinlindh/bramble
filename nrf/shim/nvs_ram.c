// RAM-backed NVS shim for the nRF52840 target (P0: flash persistence arrives
// with LittleFS in P2; until then settings and stores live for one boot).
//
// Design: a fixed entry table plus one shared byte pool with a bump
// allocator. Overwrites that fit rewrite in place; growing overwrites leak
// the old pool bytes (tracked in s_pool_dead). That is acceptable for the
// firmware's write pattern (few, small, mostly-stable keys) and keeps the
// code trivially auditable. Capacity bounds are sized from real payloads;
// see nvs.h and test/test_nvs_ram_shim.c.
#include "nvs.h"

#include <stdbool.h>
#include <string.h>

#include "nvs_flash.h"

#if defined(BRAMBLE_PLATFORM_NRF)
#include <FreeRTOS.h>
#include <semphr.h>
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;
static void lock_init(void) {
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }
}
static void lock_take(void) {
    if (s_lock != NULL && xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}
static void lock_give(void) {
    if (s_lock != NULL && xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        xSemaphoreGive(s_lock);
    }
}
#else
static void lock_init(void) {}
static void lock_take(void) {}
static void lock_give(void) {}
#endif

typedef struct {
    bool used;
    uint8_t ns_idx;
    char key[NVS_RAM_NAME_MAX];
    uint8_t type;
    uint16_t len;
    uint16_t off; // offset into s_pool
} nvs_ram_entry_t;

static char s_namespaces[NVS_RAM_MAX_NS][NVS_RAM_NAME_MAX];
static uint8_t s_ns_count;
static nvs_ram_entry_t s_entries[NVS_RAM_MAX_ENTRIES];
static uint8_t s_pool[NVS_RAM_POOL_SIZE];
static uint16_t s_pool_used;
static uint16_t s_pool_dead;

// Iterator handles are indices+1 into s_entries, filtered by namespace/type.
struct nvs_opaque_iterator_t {
    uint8_t ns_idx;
    nvs_type_t type;
    int idx; // current position in s_entries
};
static struct nvs_opaque_iterator_t s_iter; // single live iterator is enough

static int ns_find(const char* ns) {
    for (int i = 0; i < s_ns_count; i++) {
        if (strcmp(s_namespaces[i], ns) == 0) {
            return i;
        }
    }
    return -1;
}

static nvs_ram_entry_t* entry_find(uint8_t ns_idx, const char* key) {
    for (int i = 0; i < NVS_RAM_MAX_ENTRIES; i++) {
        if (s_entries[i].used && s_entries[i].ns_idx == ns_idx &&
            strcmp(s_entries[i].key, key) == 0) {
            return &s_entries[i];
        }
    }
    return NULL;
}

esp_err_t nvs_flash_init(void) {
    lock_init();
    return ESP_OK;
}

esp_err_t nvs_flash_erase(void) {
    lock_init();
    lock_take();
    memset(s_entries, 0, sizeof(s_entries));
    memset(s_namespaces, 0, sizeof(s_namespaces));
    s_ns_count = 0;
    s_pool_used = 0;
    s_pool_dead = 0;
    lock_give();
    return ESP_OK;
}

esp_err_t nvs_open(const char* ns, nvs_open_mode_t mode, nvs_handle_t* out_handle) {
    if (ns == NULL || out_handle == NULL || strlen(ns) >= NVS_RAM_NAME_MAX) {
        return ESP_ERR_NVS_INVALID_NAME;
    }
    lock_init();
    lock_take();
    int idx = ns_find(ns);
    if (idx < 0) {
        if (mode == NVS_READONLY) {
            lock_give();
            return ESP_ERR_NVS_NOT_FOUND;
        }
        if (s_ns_count >= NVS_RAM_MAX_NS) {
            lock_give();
            return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
        }
        idx = s_ns_count++;
        strncpy(s_namespaces[idx], ns, NVS_RAM_NAME_MAX - 1);
    }
    lock_give();
    *out_handle = (nvs_handle_t)(idx + 1);
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle) { (void)handle; }

esp_err_t nvs_commit(nvs_handle_t handle) {
    (void)handle;
    return ESP_OK;
}

static esp_err_t handle_ns(nvs_handle_t handle, uint8_t* out_ns) {
    if (handle == 0 || handle > s_ns_count) {
        return ESP_ERR_NVS_INVALID_HANDLE;
    }
    *out_ns = (uint8_t)(handle - 1);
    return ESP_OK;
}

static esp_err_t set_value(nvs_handle_t handle, const char* key, nvs_type_t type, const void* value,
                           size_t len) {
    uint8_t ns_idx;
    esp_err_t err = handle_ns(handle, &ns_idx);
    if (err != ESP_OK) {
        return err;
    }
    if (key == NULL || strlen(key) >= NVS_RAM_NAME_MAX || len > UINT16_MAX) {
        return ESP_ERR_NVS_INVALID_NAME;
    }
    lock_take();
    nvs_ram_entry_t* e = entry_find(ns_idx, key);
    if (e != NULL && e->len >= len) {
        // Rewrite in place; shrinking leaks the tail into dead bytes.
        s_pool_dead += (uint16_t)(e->len - len);
        e->type = (uint8_t)type;
        e->len = (uint16_t)len;
        memcpy(&s_pool[e->off], value, len);
        lock_give();
        return ESP_OK;
    }
    if ((size_t)s_pool_used + len > NVS_RAM_POOL_SIZE) {
        lock_give();
        return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
    }
    if (e == NULL) {
        for (int i = 0; i < NVS_RAM_MAX_ENTRIES; i++) {
            if (!s_entries[i].used) {
                e = &s_entries[i];
                break;
            }
        }
        if (e == NULL) {
            lock_give();
            return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
        }
        e->used = true;
        e->ns_idx = ns_idx;
        strncpy(e->key, key, NVS_RAM_NAME_MAX - 1);
        e->key[NVS_RAM_NAME_MAX - 1] = '\0';
    } else {
        // Growing overwrite: the old allocation becomes dead pool bytes.
        s_pool_dead += e->len;
    }
    e->type = (uint8_t)type;
    e->len = (uint16_t)len;
    e->off = s_pool_used;
    memcpy(&s_pool[e->off], value, len);
    s_pool_used = (uint16_t)(s_pool_used + len);
    lock_give();
    return ESP_OK;
}

static esp_err_t get_value(nvs_handle_t handle, const char* key, nvs_type_t type, void* out,
                           size_t* inout_len, bool length_query_allowed) {
    uint8_t ns_idx;
    esp_err_t err = handle_ns(handle, &ns_idx);
    if (err != ESP_OK) {
        return err;
    }
    if (key == NULL || inout_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    lock_take();
    nvs_ram_entry_t* e = entry_find(ns_idx, key);
    if (e == NULL) {
        lock_give();
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (e->type != (uint8_t)type) {
        lock_give();
        return ESP_ERR_NVS_TYPE_MISMATCH;
    }
    if (out == NULL && length_query_allowed) {
        *inout_len = e->len;
        lock_give();
        return ESP_OK;
    }
    if (*inout_len < e->len) {
        lock_give();
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(out, &s_pool[e->off], e->len);
    *inout_len = e->len;
    lock_give();
    return ESP_OK;
}

#define SCALAR_OPS(suffix, ctype, nvstype)                                                         \
    esp_err_t nvs_set_##suffix(nvs_handle_t h, const char* key, ctype value) {                     \
        return set_value(h, key, nvstype, &value, sizeof(value));                                  \
    }                                                                                              \
    esp_err_t nvs_get_##suffix(nvs_handle_t h, const char* key, ctype* out_value) {                \
        size_t len = sizeof(*out_value);                                                           \
        return get_value(h, key, nvstype, out_value, &len, false);                                 \
    }

SCALAR_OPS(u8, uint8_t, NVS_TYPE_U8)
SCALAR_OPS(i8, int8_t, NVS_TYPE_I8)
SCALAR_OPS(u16, uint16_t, NVS_TYPE_U16)
SCALAR_OPS(u32, uint32_t, NVS_TYPE_U32)
SCALAR_OPS(i32, int32_t, NVS_TYPE_I32)

esp_err_t nvs_set_str(nvs_handle_t handle, const char* key, const char* value) {
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return set_value(handle, key, NVS_TYPE_STR, value, strlen(value) + 1);
}

esp_err_t nvs_get_str(nvs_handle_t handle, const char* key, char* out_value, size_t* inout_len) {
    return get_value(handle, key, NVS_TYPE_STR, out_value, inout_len, true);
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char* key, const void* value, size_t len) {
    if (value == NULL && len > 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return set_value(handle, key, NVS_TYPE_BLOB, value, len);
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char* key, void* out_value, size_t* inout_len) {
    return get_value(handle, key, NVS_TYPE_BLOB, out_value, inout_len, true);
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char* key) {
    uint8_t ns_idx;
    esp_err_t err = handle_ns(handle, &ns_idx);
    if (err != ESP_OK) {
        return err;
    }
    lock_take();
    nvs_ram_entry_t* e = entry_find(ns_idx, key);
    if (e == NULL) {
        lock_give();
        return ESP_ERR_NVS_NOT_FOUND;
    }
    s_pool_dead += e->len;
    memset(e, 0, sizeof(*e));
    lock_give();
    return ESP_OK;
}

esp_err_t nvs_erase_all(nvs_handle_t handle) {
    uint8_t ns_idx;
    esp_err_t err = handle_ns(handle, &ns_idx);
    if (err != ESP_OK) {
        return err;
    }
    lock_take();
    for (int i = 0; i < NVS_RAM_MAX_ENTRIES; i++) {
        if (s_entries[i].used && s_entries[i].ns_idx == ns_idx) {
            s_pool_dead += s_entries[i].len;
            memset(&s_entries[i], 0, sizeof(s_entries[i]));
        }
    }
    lock_give();
    return ESP_OK;
}

static bool iter_matches(const struct nvs_opaque_iterator_t* it, int i) {
    if (!s_entries[i].used || s_entries[i].ns_idx != it->ns_idx) {
        return false;
    }
    return it->type == NVS_TYPE_ANY || (uint8_t)it->type == s_entries[i].type;
}

static esp_err_t iter_advance(struct nvs_opaque_iterator_t* it) {
    for (int i = it->idx + 1; i < NVS_RAM_MAX_ENTRIES; i++) {
        if (iter_matches(it, i)) {
            it->idx = i;
            return ESP_OK;
        }
    }
    return ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_entry_find(const char* part, const char* ns, nvs_type_t type, nvs_iterator_t* it) {
    (void)part;
    if (ns == NULL || it == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    int ns_idx = ns_find(ns);
    if (ns_idx < 0) {
        *it = NULL;
        return ESP_ERR_NVS_NOT_FOUND;
    }
    s_iter.ns_idx = (uint8_t)ns_idx;
    s_iter.type = type;
    s_iter.idx = -1;
    if (iter_advance(&s_iter) != ESP_OK) {
        *it = NULL;
        return ESP_ERR_NVS_NOT_FOUND;
    }
    *it = &s_iter;
    return ESP_OK;
}

esp_err_t nvs_entry_next(nvs_iterator_t* it) {
    if (it == NULL || *it == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = iter_advance(*it);
    if (err != ESP_OK) {
        *it = NULL;
    }
    return err;
}

esp_err_t nvs_entry_info(nvs_iterator_t it, nvs_entry_info_t* out_info) {
    if (it == NULL || out_info == NULL || it->idx < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const nvs_ram_entry_t* e = &s_entries[it->idx];
    strncpy(out_info->namespace_name, s_namespaces[e->ns_idx], NVS_RAM_NAME_MAX - 1);
    out_info->namespace_name[NVS_RAM_NAME_MAX - 1] = '\0';
    strncpy(out_info->key, e->key, NVS_RAM_NAME_MAX - 1);
    out_info->key[NVS_RAM_NAME_MAX - 1] = '\0';
    out_info->type = (nvs_type_t)e->type;
    return ESP_OK;
}

void nvs_release_iterator(nvs_iterator_t it) { (void)it; }
