/* See nvs_fake.h for why this exists alongside esp_stubs.c's no-op NVS. */
#include "nvs_fake.h"

#include "nvs.h"

#include <stdio.h>
#include <string.h>

#define NVS_FAKE_MAX_ENTRIES 64
#define NVS_FAKE_MAX_VALUE 64
#define NVS_FAKE_MAX_NS 8

typedef struct {
    char ns[16];
    char key[16];
    uint8_t value[NVS_FAKE_MAX_VALUE];
    size_t len;
    bool used;
} nvs_fake_entry_t;

static nvs_fake_entry_t s_entries[NVS_FAKE_MAX_ENTRIES];
static char s_open_ns[NVS_FAKE_MAX_NS][16];
static int s_open_count;
static bool s_open_fails;

/* An iterator is just a cursor into the entry table plus the namespace it is
 * walking. nvs.h types it as a pointer to an incomplete struct, so define it
 * here and hand back pointers into a small pool. */
struct nvs_iter_rec {
    char ns[16];
    int index;
    bool used;
};
static struct nvs_iter_rec s_iters[4];

void nvs_fake_reset(void) {
    memset(s_entries, 0, sizeof(s_entries));
    memset(s_open_ns, 0, sizeof(s_open_ns));
    memset(s_iters, 0, sizeof(s_iters));
    s_open_count = 0;
    s_open_fails = false;
}

void nvs_fake_set_open_fails(bool fails) { s_open_fails = fails; }

static nvs_fake_entry_t* find_entry(const char* ns, const char* key) {
    for (int i = 0; i < NVS_FAKE_MAX_ENTRIES; i++) {
        if (s_entries[i].used && strcmp(s_entries[i].ns, ns) == 0 &&
            strcmp(s_entries[i].key, key) == 0) {
            return &s_entries[i];
        }
    }
    return NULL;
}

static nvs_fake_entry_t* alloc_entry(const char* ns, const char* key) {
    nvs_fake_entry_t* e = find_entry(ns, key);
    if (e)
        return e;
    for (int i = 0; i < NVS_FAKE_MAX_ENTRIES; i++) {
        if (!s_entries[i].used) {
            s_entries[i].used = true;
            snprintf(s_entries[i].ns, sizeof(s_entries[i].ns), "%s", ns);
            snprintf(s_entries[i].key, sizeof(s_entries[i].key), "%s", key);
            s_entries[i].len = 0;
            return &s_entries[i];
        }
    }
    return NULL;
}

void nvs_fake_put_blob(const char* ns, const char* key, const void* value, size_t len) {
    if (len > NVS_FAKE_MAX_VALUE)
        return;
    nvs_fake_entry_t* e = alloc_entry(ns, key);
    if (!e)
        return;
    memcpy(e->value, value, len);
    e->len = len;
}

bool nvs_fake_read_blob(const char* ns, const char* key, void* out, size_t* len) {
    nvs_fake_entry_t* e = find_entry(ns, key);
    if (!e)
        return false;
    if (out && len && *len >= e->len)
        memcpy(out, e->value, e->len);
    if (len)
        *len = e->len;
    return true;
}

void nvs_fake_put_u32(const char* ns, const char* key, uint32_t value) {
    nvs_fake_put_blob(ns, key, &value, sizeof(value));
}

bool nvs_fake_read_u32(const char* ns, const char* key, uint32_t* out) {
    size_t len = sizeof(*out);
    nvs_fake_entry_t* e = find_entry(ns, key);
    if (!e || e->len != sizeof(uint32_t))
        return false;
    memcpy(out, e->value, sizeof(uint32_t));
    (void)len;
    return true;
}

int nvs_fake_count(const char* ns) {
    int n = 0;
    for (int i = 0; i < NVS_FAKE_MAX_ENTRIES; i++) {
        if (s_entries[i].used && strcmp(s_entries[i].ns, ns) == 0)
            n++;
    }
    return n;
}

/* ── The NVS API itself ─────────────────────────────────────────────────── */

/* A handle is an index into s_open_ns, offset by one so 0 is never valid. */
static const char* handle_ns(nvs_handle_t h) {
    int idx = (int)h - 1;
    if (idx < 0 || idx >= s_open_count)
        return NULL;
    return s_open_ns[idx];
}

esp_err_t nvs_open(const char* ns, int mode, nvs_handle_t* out_handle) {
    (void)mode;
    if (s_open_fails || !ns || !out_handle)
        return ESP_FAIL;
    for (int i = 0; i < s_open_count; i++) {
        if (strcmp(s_open_ns[i], ns) == 0) {
            *out_handle = (nvs_handle_t)(i + 1);
            return ESP_OK;
        }
    }
    if (s_open_count >= NVS_FAKE_MAX_NS)
        return ESP_FAIL;
    snprintf(s_open_ns[s_open_count], sizeof(s_open_ns[0]), "%s", ns);
    s_open_count++;
    *out_handle = (nvs_handle_t)s_open_count;
    return ESP_OK;
}

void nvs_close(nvs_handle_t h) { (void)h; }

esp_err_t nvs_commit(nvs_handle_t h) { return handle_ns(h) ? ESP_OK : ESP_FAIL; }

esp_err_t nvs_set_blob(nvs_handle_t h, const char* key, const void* value, size_t length) {
    const char* ns = handle_ns(h);
    if (!ns || length > NVS_FAKE_MAX_VALUE)
        return ESP_FAIL;
    nvs_fake_entry_t* e = alloc_entry(ns, key);
    if (!e)
        return ESP_FAIL;
    memcpy(e->value, value, length);
    e->len = length;
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t h, const char* key, void* out, size_t* len) {
    const char* ns = handle_ns(h);
    if (!ns || !len)
        return ESP_FAIL;
    nvs_fake_entry_t* e = find_entry(ns, key);
    if (!e)
        return ESP_FAIL;
    if (!out) {
        *len = e->len;
        return ESP_OK;
    }
    if (*len < e->len)
        return ESP_FAIL;
    memcpy(out, e->value, e->len);
    *len = e->len;
    return ESP_OK;
}

static esp_err_t fake_set_fixed(nvs_handle_t h, const char* key, const void* v, size_t n) {
    const char* ns = handle_ns(h);
    if (!ns)
        return ESP_FAIL;
    nvs_fake_entry_t* e = alloc_entry(ns, key);
    if (!e)
        return ESP_FAIL;
    memcpy(e->value, v, n);
    e->len = n;
    return ESP_OK;
}

static esp_err_t fake_get_fixed(nvs_handle_t h, const char* key, void* out, size_t n) {
    const char* ns = handle_ns(h);
    if (!ns || !out)
        return ESP_FAIL;
    nvs_fake_entry_t* e = find_entry(ns, key);
    if (!e || e->len != n)
        return ESP_FAIL;
    memcpy(out, e->value, n);
    return ESP_OK;
}

esp_err_t nvs_set_u8(nvs_handle_t h, const char* key, uint8_t v) {
    return fake_set_fixed(h, key, &v, sizeof(v));
}
esp_err_t nvs_set_u16(nvs_handle_t h, const char* key, uint16_t v) {
    return fake_set_fixed(h, key, &v, sizeof(v));
}
esp_err_t nvs_set_u32(nvs_handle_t h, const char* key, uint32_t v) {
    return fake_set_fixed(h, key, &v, sizeof(v));
}
esp_err_t nvs_set_i8(nvs_handle_t h, const char* key, int8_t v) {
    return fake_set_fixed(h, key, &v, sizeof(v));
}
esp_err_t nvs_set_i32(nvs_handle_t h, const char* key, int32_t v) {
    return fake_set_fixed(h, key, &v, sizeof(v));
}
esp_err_t nvs_get_u8(nvs_handle_t h, const char* key, uint8_t* out) {
    return fake_get_fixed(h, key, out, sizeof(*out));
}
esp_err_t nvs_get_u16(nvs_handle_t h, const char* key, uint16_t* out) {
    return fake_get_fixed(h, key, out, sizeof(*out));
}
esp_err_t nvs_get_u32(nvs_handle_t h, const char* key, uint32_t* out) {
    return fake_get_fixed(h, key, out, sizeof(*out));
}
esp_err_t nvs_get_i32(nvs_handle_t h, const char* key, int32_t* out) {
    return fake_get_fixed(h, key, out, sizeof(*out));
}

esp_err_t nvs_set_str(nvs_handle_t h, const char* key, const char* value) {
    return fake_set_fixed(h, key, value, strlen(value) + 1);
}

esp_err_t nvs_get_str(nvs_handle_t h, const char* key, char* out, size_t* length) {
    const char* ns = handle_ns(h);
    if (!ns || !length)
        return ESP_FAIL;
    nvs_fake_entry_t* e = find_entry(ns, key);
    if (!e)
        return ESP_FAIL;
    if (!out) {
        *length = e->len;
        return ESP_OK;
    }
    if (*length < e->len)
        return ESP_FAIL;
    memcpy(out, e->value, e->len);
    *length = e->len;
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t h, const char* key) {
    const char* ns = handle_ns(h);
    if (!ns)
        return ESP_FAIL;
    nvs_fake_entry_t* e = find_entry(ns, key);
    if (!e)
        return ESP_FAIL;
    memset(e, 0, sizeof(*e));
    return ESP_OK;
}

/* ── Directory iteration ────────────────────────────────────────────────── */

static int next_index_in_ns(const char* ns, int from) {
    for (int i = from; i < NVS_FAKE_MAX_ENTRIES; i++) {
        if (s_entries[i].used && strcmp(s_entries[i].ns, ns) == 0)
            return i;
    }
    return -1;
}

esp_err_t nvs_entry_find(const char* part_name, const char* namespace_name, nvs_type_t type,
                         nvs_iterator_t* out_iterator) {
    (void)part_name;
    (void)type;
    if (!namespace_name || !out_iterator)
        return ESP_FAIL;
    *out_iterator = NULL;
    if (s_open_fails)
        return ESP_FAIL;

    int first = next_index_in_ns(namespace_name, 0);
    if (first < 0)
        return ESP_FAIL;

    for (size_t i = 0; i < sizeof(s_iters) / sizeof(s_iters[0]); i++) {
        if (!s_iters[i].used) {
            s_iters[i].used = true;
            s_iters[i].index = first;
            snprintf(s_iters[i].ns, sizeof(s_iters[i].ns), "%s", namespace_name);
            *out_iterator = &s_iters[i];
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

esp_err_t nvs_entry_next(nvs_iterator_t* iterator) {
    if (!iterator || !*iterator)
        return ESP_FAIL;
    struct nvs_iter_rec* it = *iterator;
    int next = next_index_in_ns(it->ns, it->index + 1);
    if (next < 0) {
        it->used = false;
        *iterator = NULL;
        return ESP_FAIL;
    }
    it->index = next;
    return ESP_OK;
}

void nvs_entry_info(nvs_iterator_t iterator, nvs_entry_info_t* out_info) {
    if (!out_info)
        return;
    out_info->key[0] = '\0';
    if (!iterator)
        return;
    snprintf(out_info->key, sizeof(out_info->key), "%s", s_entries[iterator->index].key);
}

void nvs_release_iterator(nvs_iterator_t iterator) {
    if (iterator)
        iterator->used = false;
}
