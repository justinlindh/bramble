/*
 * Per-node NVS persistence for the IDF linux target.
 *
 * IDF's esp_partition linux backend emulates SPI flash in a file. By default
 * (README.md caveat 6) it mkstemp()s a fresh file per process, so NVS -- and
 * therefore the node identity -- is regenerated on every restart. The gosim
 * process supervisor restarts a node process as its "reset button"; without a
 * stable flash file each reset would mint a new node id, which breaks the
 * reserved-slot reattach model (extnode.go) and any test that resets a node.
 *
 * The documented hook is esp_partition_get_file_mmap_ctrl_input(): point its
 * flash_file_name at a persistent file before nvs_flash_init and the backend
 * mmaps that file (MAP_SHARED) instead of a throwaway temp. This binds the
 * flash image to "$NODE_DIR/flash.bin" when NODE_DIR is set (the supervisor
 * sets it per instance). NODE_DIR unset keeps today's ephemeral-temp behavior,
 * so a standalone `./bramble-node` run (spike_check.sh) is unchanged.
 *
 * The backend's existing-file path opens O_RDWR with no O_CREAT and rejects a
 * flash_file_name combined with a size/partition-table, so it cannot create a
 * new named image itself. First boot therefore creates the file here the same
 * way the backend creates its temp image: a 4 MB (ESP_PARTITION_DEFAULT_
 * EMULATED_FLASH_SIZE) buffer of 0xFF with the build's partition-table.bin
 * copied in at ESP_PARTITION_TABLE_OFFSET. The partition table lives next to
 * the executable (build/partition_table/partition-table.bin), located via
 * /proc/self/exe so it resolves regardless of the launcher's cwd.
 *
 * Host-only: built only by emulator/node (null_drivers component) on the linux
 * target; never part of a device build.
 */
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_private/partition_linux.h"
#include "sdkconfig.h"

static const char *TAG = "emu_persist";

/* Offset of the partition table within the emulated flash image. Matches
 * ESP_PARTITION_TABLE_OFFSET (== CONFIG_PARTITION_TABLE_OFFSET), taken from
 * sdkconfig directly so this file need not pull in bootloader_support (which is
 * device-only on the linux target). */
#define EMU_PARTITION_TABLE_OFFSET CONFIG_PARTITION_TABLE_OFFSET

/* Resolves the build's partition-table.bin path from the running executable:
 * <dir of /proc/self/exe>/partition_table/partition-table.bin. Returns 0 and
 * fills out on success, -1 otherwise. */
static int resolve_partition_table_path(char *out, size_t out_len) {
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0)
        return -1;
    exe[n] = '\0';
    char *slash = strrchr(exe, '/');
    if (!slash)
        return -1;
    *slash = '\0'; /* exe now holds the build dir */
    int w = snprintf(out, out_len, "%s/partition_table/partition-table.bin", exe);
    if (w < 0 || (size_t)w >= out_len)
        return -1;
    return 0;
}

/* Creates a blank but valid emulated-flash image at path: 4 MB of 0xFF with the
 * partition table copied in at the table offset, mirroring what the esp_partition
 * linux backend writes into a fresh temp image. Returns 0 on success. */
static int create_blank_flash(const char *path) {
    char pt_path[PATH_MAX];
    if (resolve_partition_table_path(pt_path, sizeof(pt_path)) != 0) {
        ESP_LOGE(TAG, "cannot locate partition-table.bin next to executable");
        return -1;
    }

    FILE *pt = fopen(pt_path, "rb");
    if (!pt) {
        ESP_LOGE(TAG, "open partition table %s: %s", pt_path, strerror(errno));
        return -1;
    }
    if (fseek(pt, 0L, SEEK_END) != 0) {
        fclose(pt);
        return -1;
    }
    long pt_len = ftell(pt);
    rewind(pt);
    if (pt_len <= 0) {
        ESP_LOGE(TAG, "partition table %s is empty", pt_path);
        fclose(pt);
        return -1;
    }

    const size_t flash_size = ESP_PARTITION_DEFAULT_EMULATED_FLASH_SIZE;
    if ((size_t)EMU_PARTITION_TABLE_OFFSET + (size_t)pt_len > flash_size) {
        ESP_LOGE(TAG, "partition table does not fit in emulated flash");
        fclose(pt);
        return -1;
    }

    uint8_t *img = malloc(flash_size);
    if (!img) {
        ESP_LOGE(TAG, "out of memory allocating %zu-byte flash image", flash_size);
        fclose(pt);
        return -1;
    }
    memset(img, 0xFF, flash_size); /* NOR erased state */
    size_t pt_read = fread(img + EMU_PARTITION_TABLE_OFFSET, 1, (size_t)pt_len, pt);
    fclose(pt);
    if (pt_read != (size_t)pt_len) {
        ESP_LOGE(TAG, "short read of partition table %s", pt_path);
        free(img);
        return -1;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "create flash image %s: %s", path, strerror(errno));
        free(img);
        return -1;
    }
    size_t wrote = fwrite(img, 1, flash_size, f);
    int rc = (fclose(f) == 0 && wrote == flash_size) ? 0 : -1;
    free(img);
    if (rc != 0) {
        ESP_LOGE(TAG, "short write creating flash image %s", path);
        return rc;
    }
    ESP_LOGI(TAG, "created blank flash image %s (%zu bytes)", path, flash_size);
    return 0;
}

/* Points the esp_partition linux backend at $NODE_DIR/flash.bin so NVS (and the
 * node identity) survives a process restart. No-op when NODE_DIR is unset.
 * Must be called before nvs_flash_init. */
void emu_node_flash_persist_init(void) {
    const char *node_dir = getenv("NODE_DIR");
    if (!node_dir || !*node_dir) {
        ESP_LOGI(TAG, "NODE_DIR unset: using ephemeral temp flash (no persistence)");
        return;
    }

    char path[PATH_MAX];
    int w = snprintf(path, sizeof(path), "%s/flash.bin", node_dir);
    if (w < 0 || (size_t)w >= sizeof(path)) {
        ESP_LOGE(TAG, "NODE_DIR path too long, falling back to ephemeral flash");
        return;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        if (create_blank_flash(path) != 0) {
            ESP_LOGE(TAG, "flash image creation failed, falling back to ephemeral flash");
            return;
        }
    } else if ((size_t)st.st_size != (size_t)ESP_PARTITION_DEFAULT_EMULATED_FLASH_SIZE) {
        /* A wrong-sized image means a prior write was interrupted (truncated) or
         * the emulated flash size changed. The backend mmaps the file blindly, so
         * a short image SIGBUSes on first access past its end. Recreate a blank
         * valid image rather than mapping a corrupt one; a truncated NVS is not
         * worth salvaging (identity would be unreadable anyway). */
        ESP_LOGW(TAG, "persistent flash image %s is %lld bytes, expected %zu; recreating blank",
                 path, (long long)st.st_size, (size_t)ESP_PARTITION_DEFAULT_EMULATED_FLASH_SIZE);
        if (create_blank_flash(path) != 0) {
            ESP_LOGE(TAG, "flash image recreation failed, falling back to ephemeral flash");
            return;
        }
    } else {
        ESP_LOGI(TAG, "reusing persistent flash image %s (%lld bytes)", path,
                 (long long)st.st_size);
    }

    esp_partition_file_mmap_ctrl_t *ctrl = esp_partition_get_file_mmap_ctrl_input();
    if (!ctrl) {
        ESP_LOGE(TAG, "esp_partition mmap ctrl unavailable, falling back to ephemeral flash");
        return;
    }
    /* Existing-file mode: name only, size/partition-table must stay zero. */
    strlcpy(ctrl->flash_file_name, path, sizeof(ctrl->flash_file_name));
    ctrl->flash_file_size = 0;
    ctrl->partition_file_name[0] = '\0';
    ESP_LOGI(TAG, "NVS flash persisted at %s", path);
}
