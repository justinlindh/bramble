/*
 * Flash-backed NVS for the nRF52840 target: the same nvs.h contract the RAM
 * shim implemented (nvs_ram.c, still the host-test reference), now durable.
 *
 * Layout: one littlefs file per entry at "/<namespace>/<key>", whose first
 * byte is the nvs_type_t and whose remainder is the value. That makes
 * namespaces real directories (so the iterator is a directory scan) and
 * makes a torn write cost exactly one key rather than the whole store.
 *
 * Durability: nvs_commit() is the flush point (it was a no-op in RAM), and
 * every set path already fsyncs its file before returning, so a commit is a
 * cheap confirmation rather than the only barrier. Callers all commit
 * already, so making it meaningful is a drop-in.
 */
#include "nvs.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "lfs.h"
#include "lfs_nvmc.h"
#include "nvs_flash.h"
#include "nvs_lfs_mount.h"

static const char* TAG = "nvs_lfs";

static lfs_t s_lfs;
static struct lfs_config s_cfg;
static bool s_mounted;
static char s_namespaces[NVS_RAM_MAX_NS][NVS_RAM_NAME_MAX];
static uint8_t s_ns_count;

/* LFS_NO_MALLOC: one file buffer, safe because the lock serializes every
 * filesystem call in this shim. */
static uint8_t s_file_buffer[LFS_NVMC_CACHE_SIZE];
static const struct lfs_file_config s_file_cfg = {.buffer = s_file_buffer};

#if defined(BRAMBLE_PLATFORM_NRF)
#include <FreeRTOS.h>
#include <semphr.h>
#include <task.h>
/* LOCK-ORDER INVARIANT: this mutex sits strictly BELOW every other lock in
 * the firmware. It may be taken while holding nothing else, and no other lock
 * may be taken while holding it. That is what makes it deadlock-free despite
 * the iterator holding it across a caller's whole loop body.
 *
 * The invariant is load-bearing, not decorative. Two reverse orderings exist
 * and both are reachable: mesh_add_channel() holds s_state_mutex across
 * channel_storage_save() (mesh_channels.c), and sendMessage holds s_dm_mutex
 * then s_nonce_mutex across nonce_counter_next()'s reserve-ceiling flush,
 * which reaches NVS two frames down through mesh_nonce_write()
 * (nonce_counter.c, mesh_persist.c). Any iterator caller that transmits or
 * mutates shared mesh state from inside its loop would close one of those
 * cycles. mesh_send_location_updates() is written in two phases for exactly
 * this reason: collect targets under the iterator, release, then transmit.
 *
 * RECURSIVE, and it has to be. The iterator API below holds this lock for the
 * whole life of an iteration (see nvs_entry_find), and every caller that
 * iterates also calls locked nvs_* getters from inside its loop body:
 * main/mesh_location.c (nvs_get_str) and main/rpc_methods.c (nvs_get_blob,
 * nvs_get_str). With a plain mutex the first such nested call would deadlock
 * the caller against itself. Priority inheritance still applies to the
 * recursive variant, so the bond-writer inversion protection is unchanged. */
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;
static void lock_init(void) {
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateRecursiveMutexStatic(&s_lock_buf);
    }
}
static void lock_take(void) {
    if (s_lock != NULL && xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    }
}
static void lock_give(void) {
    if (s_lock != NULL && xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        xSemaphoreGiveRecursive(s_lock);
    }
}
/* Which task owns the live iteration, for the holder validation in
 * nvs_release_iterator. Same intent as DM_MUTEX_GIVE in
 * main/mesh_internal.h: a release from the wrong task is misuse, so name it
 * in the log rather than let it corrupt the owner's recursion depth. The
 * lifetime hold means a non-owner cannot normally reach the check at all;
 * this catches the case where that stops being true. */
static TaskHandle_t s_iter_owner;
static void iter_set_owner(void) {
    s_iter_owner = (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
                       ? xTaskGetCurrentTaskHandle()
                       : NULL;
}
static void iter_clear_owner(void) { s_iter_owner = NULL; }
static bool iter_owned_here(void) {
    if (s_iter_owner == NULL || xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) {
        return true;
    }
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    if (s_iter_owner != me) {
        ESP_LOGE(TAG, "NVS iterator release by '%s' but owner is '%s'", pcTaskGetName(NULL),
                 pcTaskGetName(s_iter_owner));
        return false;
    }
    return true;
}
#else
/* Host suite: single-threaded, no RTOS. */
static void lock_init(void) {}
static void lock_take(void) {}
static void lock_give(void) {}
static void iter_set_owner(void) {}
static void iter_clear_owner(void) {}
static bool iter_owned_here(void) { return true; }
#endif

/* The lock, exported for the mount header's other consumers. littlefs is not
 * thread-safe, there is exactly one lfs_t on this device, and the nvs_* API
 * takes this lock internally; anything that touches the handle from
 * nvs_lfs_handle() directly (the message store) must hold it across every
 * lfs_* call, or a mesh-task message append can interleave with an RPC-task
 * settings write inside littlefs's shared metadata and cache state. */
void nvs_lfs_lock(void) { lock_take(); }
void nvs_lfs_unlock(void) { lock_give(); }

static int ns_index(const char* ns) {
    for (int i = 0; i < s_ns_count; i++) {
        if (strcmp(s_namespaces[i], ns) == 0) {
            return i;
        }
    }
    return -1;
}

/* Namespace registry is rebuilt from the filesystem at mount, so handles stay
 * stable within a boot and nothing extra is persisted. */
static void scan_namespaces(void) {
    s_ns_count = 0;
    lfs_dir_t dir;
    if (lfs_dir_open(&s_lfs, &dir, "/") < 0) {
        return;
    }
    struct lfs_info info;
    while (lfs_dir_read(&s_lfs, &dir, &info) > 0) {
        if (info.type != LFS_TYPE_DIR || info.name[0] == '.' ||
            strlen(info.name) >= NVS_RAM_NAME_MAX) {
            continue;
        }
        if (s_ns_count >= NVS_RAM_MAX_NS) {
            break;
        }
        strncpy(s_namespaces[s_ns_count], info.name, NVS_RAM_NAME_MAX - 1);
        s_namespaces[s_ns_count][NVS_RAM_NAME_MAX - 1] = '\0';
        s_ns_count++;
    }
    lfs_dir_close(&s_lfs, &dir);
}

lfs_t* nvs_lfs_handle(void) { return s_mounted ? &s_lfs : NULL; }

esp_err_t nvs_flash_init(void) {
    lock_init();
    lock_take();
    if (s_mounted) {
        lock_give();
        return ESP_OK;
    }
    lfs_nvmc_config_init(&s_cfg, BRAMBLE_LFS_BASE, BRAMBLE_LFS_SIZE);
    int err = lfs_mount(&s_lfs, &s_cfg);
    if (err != LFS_ERR_OK) {
        /* First boot or corrupt store: format once and mount. A format is
         * the honest recovery, and it is loud so a repeating one is
         * visible rather than silently eating settings every boot. */
        ESP_LOGW(TAG, "mount failed (%d), formatting the settings partition", err);
        err = lfs_format(&s_lfs, &s_cfg);
        if (err == LFS_ERR_OK) {
            err = lfs_mount(&s_lfs, &s_cfg);
        }
    }
    if (err != LFS_ERR_OK) {
        ESP_LOGE(TAG, "littlefs unusable (%d), settings will not persist", err);
        lock_give();
        return ESP_FAIL;
    }
    s_mounted = true;
    scan_namespaces();
    ESP_LOGI(TAG, "settings mounted at 0x%08lx (%lu KB), %u namespaces",
             (unsigned long)BRAMBLE_LFS_BASE, (unsigned long)(BRAMBLE_LFS_SIZE / 1024),
             (unsigned)s_ns_count);
    lock_give();
    return ESP_OK;
}

esp_err_t nvs_flash_erase(void) {
    lock_init();
    lock_take();
    if (s_mounted) {
        lfs_unmount(&s_lfs);
        s_mounted = false;
    }
    lfs_nvmc_config_init(&s_cfg, BRAMBLE_LFS_BASE, BRAMBLE_LFS_SIZE);
    int err = lfs_format(&s_lfs, &s_cfg);
    if (err == LFS_ERR_OK) {
        err = lfs_mount(&s_lfs, &s_cfg);
    }
    s_ns_count = 0;
    s_mounted = (err == LFS_ERR_OK);
    lock_give();
    return s_mounted ? ESP_OK : ESP_FAIL;
}

esp_err_t nvs_open(const char* ns, nvs_open_mode_t mode, nvs_handle_t* out_handle) {
    if (ns == NULL || out_handle == NULL || strlen(ns) >= NVS_RAM_NAME_MAX) {
        return ESP_ERR_NVS_INVALID_NAME;
    }
    if (!s_mounted) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    lock_take();
    int idx = ns_index(ns);
    if (idx < 0) {
        if (mode == NVS_READONLY) {
            lock_give();
            return ESP_ERR_NVS_NOT_FOUND;
        }
        if (s_ns_count >= NVS_RAM_MAX_NS) {
            lock_give();
            return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
        }
        char path[NVS_RAM_NAME_MAX + 1];
        snprintf(path, sizeof(path), "/%s", ns);
        int err = lfs_mkdir(&s_lfs, path);
        if (err != LFS_ERR_OK && err != LFS_ERR_EXIST) {
            lock_give();
            return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
        }
        idx = s_ns_count++;
        strncpy(s_namespaces[idx], ns, NVS_RAM_NAME_MAX - 1);
        s_namespaces[idx][NVS_RAM_NAME_MAX - 1] = '\0';
    }
    lock_give();
    *out_handle = (nvs_handle_t)(idx + 1);
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle) { (void)handle; }

esp_err_t nvs_commit(nvs_handle_t handle) {
    (void)handle;
    /* Every set path syncs its own file before returning; nothing is left
     * buffered for this to flush. */
    return ESP_OK;
}

static esp_err_t handle_path(nvs_handle_t handle, const char* key, char* out, size_t out_len) {
    if (handle == 0 || handle > s_ns_count) {
        return ESP_ERR_NVS_INVALID_HANDLE;
    }
    if (key == NULL || strlen(key) >= NVS_RAM_NAME_MAX) {
        return ESP_ERR_NVS_INVALID_NAME;
    }
    snprintf(out, out_len, "/%s/%s", s_namespaces[handle - 1], key);
    return ESP_OK;
}

static esp_err_t set_value(nvs_handle_t handle, const char* key, nvs_type_t type, const void* value,
                           size_t len) {
    char path[2 * NVS_RAM_NAME_MAX + 2];
    esp_err_t err = handle_path(handle, key, path, sizeof(path));
    if (err != ESP_OK) {
        return err;
    }
    lock_take();
    lfs_file_t f;
    if (lfs_file_opencfg(&s_lfs, &f, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC, &s_file_cfg) <
        0) {
        lock_give();
        return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
    }
    uint8_t type_byte = (uint8_t)type;
    bool ok = lfs_file_write(&s_lfs, &f, &type_byte, 1) == 1;
    if (ok && len > 0) {
        ok = lfs_file_write(&s_lfs, &f, value, len) == (lfs_ssize_t)len;
    }
    if (ok) {
        ok = lfs_file_sync(&s_lfs, &f) == LFS_ERR_OK;
    }
    lfs_file_close(&s_lfs, &f);
    lock_give();
    return ok ? ESP_OK : ESP_ERR_NVS_NOT_ENOUGH_SPACE;
}

static esp_err_t get_value(nvs_handle_t handle, const char* key, nvs_type_t type, void* out,
                           size_t* inout_len, bool length_query_allowed) {
    char path[2 * NVS_RAM_NAME_MAX + 2];
    esp_err_t err = handle_path(handle, key, path, sizeof(path));
    if (err != ESP_OK) {
        return err;
    }
    if (inout_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    lock_take();
    lfs_file_t f;
    if (lfs_file_opencfg(&s_lfs, &f, path, LFS_O_RDONLY, &s_file_cfg) < 0) {
        lock_give();
        return ESP_ERR_NVS_NOT_FOUND;
    }
    lfs_soff_t total = lfs_file_size(&s_lfs, &f);
    uint8_t stored_type = 0;
    if (total < 1 || lfs_file_read(&s_lfs, &f, &stored_type, 1) != 1) {
        lfs_file_close(&s_lfs, &f);
        lock_give();
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (stored_type != (uint8_t)type) {
        lfs_file_close(&s_lfs, &f);
        lock_give();
        return ESP_ERR_NVS_TYPE_MISMATCH;
    }
    size_t value_len = (size_t)total - 1;
    if (out == NULL && length_query_allowed) {
        *inout_len = value_len;
        lfs_file_close(&s_lfs, &f);
        lock_give();
        return ESP_OK;
    }
    if (*inout_len < value_len) {
        lfs_file_close(&s_lfs, &f);
        lock_give();
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    bool ok = value_len == 0 || lfs_file_read(&s_lfs, &f, out, value_len) == (lfs_ssize_t)value_len;
    lfs_file_close(&s_lfs, &f);
    lock_give();
    if (!ok) {
        return ESP_FAIL;
    }
    *inout_len = value_len;
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
    char path[2 * NVS_RAM_NAME_MAX + 2];
    esp_err_t err = handle_path(handle, key, path, sizeof(path));
    if (err != ESP_OK) {
        return err;
    }
    lock_take();
    int rc = lfs_remove(&s_lfs, path);
    lock_give();
    return rc == LFS_ERR_OK ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_erase_all(nvs_handle_t handle) {
    if (handle == 0 || handle > s_ns_count) {
        return ESP_ERR_NVS_INVALID_HANDLE;
    }
    char dir_path[NVS_RAM_NAME_MAX + 1];
    snprintf(dir_path, sizeof(dir_path), "/%s", s_namespaces[handle - 1]);
    lock_take();
    lfs_dir_t dir;
    if (lfs_dir_open(&s_lfs, &dir, dir_path) < 0) {
        lock_give();
        return ESP_OK; /* nothing stored is not an error */
    }
    /* Collect then delete: removing during a directory read invalidates it. */
    char names[NVS_RAM_MAX_ENTRIES][NVS_RAM_NAME_MAX];
    int count = 0;
    struct lfs_info info;
    while (count < NVS_RAM_MAX_ENTRIES && lfs_dir_read(&s_lfs, &dir, &info) > 0) {
        if (info.type != LFS_TYPE_REG || strlen(info.name) >= NVS_RAM_NAME_MAX) {
            continue;
        }
        strncpy(names[count], info.name, NVS_RAM_NAME_MAX - 1);
        names[count][NVS_RAM_NAME_MAX - 1] = '\0';
        count++;
    }
    lfs_dir_close(&s_lfs, &dir);
    for (int i = 0; i < count; i++) {
        char path[2 * NVS_RAM_NAME_MAX + 2];
        snprintf(path, sizeof(path), "%s/%.*s", dir_path, NVS_RAM_NAME_MAX - 1, names[i]);
        lfs_remove(&s_lfs, path);
    }
    lock_give();
    return ESP_OK;
}

/* Single live iterator, matching the RAM shim's documented limit. */
struct nvs_opaque_iterator_t {
    uint8_t ns_idx;
    nvs_type_t type;
    lfs_dir_t dir;
    char key[NVS_RAM_NAME_MAX];
    uint8_t entry_type;
    bool open;
};
static struct nvs_opaque_iterator_t s_iter;
/* True from a successful nvs_entry_find() until the iteration ends, either by
 * nvs_entry_next() running out or by nvs_release_iterator(). While it is true
 * this module's lock is held by the iterating task. */
static bool s_iter_active;

/* Ends the current iteration and drops the lifetime lock. Caller must hold
 * the lock and have s_iter_active set. */
static void iter_finish(void) {
    if (s_iter.open) {
        lfs_dir_close(&s_lfs, &s_iter.dir);
        s_iter.open = false;
    }
    s_iter_active = false;
    iter_clear_owner();
    lock_give();
}

static esp_err_t iter_advance(struct nvs_opaque_iterator_t* it) {
    struct lfs_info info;
    while (lfs_dir_read(&s_lfs, &it->dir, &info) > 0) {
        if (info.type != LFS_TYPE_REG) {
            continue;
        }
        if (strlen(info.name) >= NVS_RAM_NAME_MAX) {
            continue; /* longer than any key this shim can write */
        }
        char path[2 * NVS_RAM_NAME_MAX + 2];
        snprintf(path, sizeof(path), "/%s/%.*s", s_namespaces[it->ns_idx], NVS_RAM_NAME_MAX - 1,
                 info.name);
        lfs_file_t f;
        if (lfs_file_opencfg(&s_lfs, &f, path, LFS_O_RDONLY, &s_file_cfg) < 0) {
            continue;
        }
        uint8_t stored_type = 0;
        bool got = lfs_file_read(&s_lfs, &f, &stored_type, 1) == 1;
        lfs_file_close(&s_lfs, &f);
        if (!got) {
            continue;
        }
        if (it->type != NVS_TYPE_ANY && (uint8_t)it->type != stored_type) {
            continue;
        }
        strncpy(it->key, info.name, NVS_RAM_NAME_MAX - 1);
        it->key[NVS_RAM_NAME_MAX - 1] = '\0';
        it->entry_type = stored_type;
        return ESP_OK;
    }
    return ESP_ERR_NVS_NOT_FOUND;
}

/* Takes the module lock and KEEPS it until the iteration ends. Nothing here
 * took any lock before, which is what wedged the device: this family walks a
 * single global s_iter (and the shared s_file_cfg buffer) with no mutual
 * exclusion at all, so the mesh task's 1Hz policy scan and the gnss task's
 * 1Hz duty-cycle scan could both be inside lfs_dir_open() on the SAME
 * lfs_dir_t. littlefs threads open dirs onto lfs->mlist, and a concurrent
 * second open self-cycles that list (dir->next == dir, lfs.c lfs_mlist_append),
 * after which the next write walks it forever while holding this lock, and
 * every NVS consumer blocks on portMAX_DELAY behind it in turn.
 *
 * The hold spans the caller's whole loop body, not just these functions,
 * because littlefs directory reads are not valid across a concurrent write to
 * the same directory. That is also why the lock is recursive: every caller
 * makes locked nvs_get_* calls from inside its loop. */
esp_err_t nvs_entry_find(const char* part, const char* ns, nvs_type_t type, nvs_iterator_t* it) {
    (void)part;
    if (ns == NULL || it == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    lock_take();
    /* Single live iterator, matching the RAM shim's documented limit. With
     * the lifetime hold, a second task's find() simply waits here, so this
     * can only trip on same-task misuse: a find() whose iteration was never
     * released. Fail it loudly instead of silently closing someone else's
     * open dir out from under them, which is what the old code did. */
    if (s_iter_active) {
        lock_give();
        *it = NULL;
        return ESP_ERR_INVALID_STATE;
    }
    int ns_idx = ns_index(ns);
    if (ns_idx < 0) {
        lock_give();
        *it = NULL;
        return ESP_ERR_NVS_NOT_FOUND;
    }
    char dir_path[NVS_RAM_NAME_MAX + 1];
    snprintf(dir_path, sizeof(dir_path), "/%s", ns);
    if (lfs_dir_open(&s_lfs, &s_iter.dir, dir_path) < 0) {
        lock_give();
        *it = NULL;
        return ESP_ERR_NVS_NOT_FOUND;
    }
    s_iter.open = true;
    s_iter.ns_idx = (uint8_t)ns_idx;
    s_iter.type = type;
    s_iter_active = true;
    iter_set_owner();
    if (iter_advance(&s_iter) != ESP_OK) {
        iter_finish();
        *it = NULL;
        return ESP_ERR_NVS_NOT_FOUND;
    }
    *it = &s_iter;
    return ESP_OK;
}

/* Called with the lifetime lock held (nvs_entry_find took it). Releases it
 * when the iteration runs out, so the common
 * "while (...) { if (nvs_entry_next(&it) != ESP_OK) break; }" shape cannot
 * leak the lock: every caller either breaks out here or calls
 * nvs_release_iterator, and both paths end the iteration exactly once. */
esp_err_t nvs_entry_next(nvs_iterator_t* it) {
    if (it == NULL || *it == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_iter_active) {
        *it = NULL;
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = iter_advance(*it);
    if (err != ESP_OK) {
        iter_finish();
        *it = NULL;
    }
    return err;
}

esp_err_t nvs_entry_info(nvs_iterator_t it, nvs_entry_info_t* out_info) {
    if (it == NULL || out_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    strncpy(out_info->namespace_name, s_namespaces[it->ns_idx], NVS_RAM_NAME_MAX - 1);
    out_info->namespace_name[NVS_RAM_NAME_MAX - 1] = '\0';
    strncpy(out_info->key, it->key, NVS_RAM_NAME_MAX - 1);
    out_info->key[NVS_RAM_NAME_MAX - 1] = '\0';
    out_info->type = (nvs_type_t)it->entry_type;
    return ESP_OK;
}

/* Deliberately keyed on s_iter_active rather than on `it`. Callers routinely
 * reach here with it == NULL, because nvs_entry_next() NULLs the handle when
 * the iteration runs out and the loop then falls through to this call; keying
 * on the handle would have made those calls no-ops and stranded the lifetime
 * lock forever. Releasing an already-finished iteration is a no-op.
 *
 * The flag is read UNDER the lock, never before it. Reading it first would
 * let a task that owns no iteration observe another task's live one and call
 * iter_finish(), closing that task's directory and giving a recursive mutex
 * it does not hold (which FreeRTOS rejects, and which corrupts the owner's
 * recursion depth). Taking the lock first costs the real owner one cheap
 * recursive re-take, and makes a non-owner wait until the iteration is over
 * and then correctly find nothing to do. */
void nvs_release_iterator(nvs_iterator_t it) {
    (void)it;
    lock_take();
    if (s_iter_active && iter_owned_here()) {
        iter_finish(); /* drops the lifetime hold */
    }
    lock_give(); /* drops the hold taken just above */
}
