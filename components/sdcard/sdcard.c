#include "sdcard.h"
#include "board_config.h"

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"
#include <string.h>

static const char* TAG = "sdcard";
static const char* MOUNT_POINT = "/sdcard";

static bool s_mounted = false;
static sdmmc_card_t* s_card = NULL;

int sdcard_init(void) {
    const bramble_board_config_t* board = board_get_config();

    /* Check if board has SD card support */
    if (!(board->capabilities & BOARD_CAP_SDCARD)) {
        ESP_LOGW(TAG, "Board does not support SD card");
        return -1;
    }

    if (board->sdcard_cs < 0) {
        ESP_LOGE(TAG, "SD card CS pin not configured");
        return -1;
    }

    if (!(board->capabilities & BOARD_CAP_SHARED_SPI)) {
        ESP_LOGE(TAG, "SD card requires shared SPI bus (board_init must be called first)");
        return -1;
    }

    ESP_LOGI(TAG, "Initializing SD card (CS=%d)", board->sdcard_cs);

    /* Mount options */
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false, /* Don't auto-format */
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    /* SPI bus config (shared bus already initialized by board_init) */
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = board->spi_host;

    /* SD card SPI device config */
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = board->sdcard_cs;
    slot_config.host_id = board->spi_host;

    /* Mount filesystem */
    esp_err_t ret =
        esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGW(TAG, "Failed to mount filesystem (no card or format error)");
        } else {
            ESP_LOGW(TAG, "Failed to initialize SD card: %s", esp_err_to_name(ret));
        }
        return -1;
    }

    /* Card info */
    if (s_card) {
        ESP_LOGI(TAG, "SD card mounted successfully");
        ESP_LOGI(TAG, "Name: %s, Speed: %d MHz, Size: %llu MB", s_card->cid.name,
                 s_card->csd.tr_speed / 1000000,
                 ((uint64_t)s_card->csd.capacity) * s_card->csd.sector_size / (1024 * 1024));
    }

    s_mounted = true;
    return 0;
}

bool sdcard_is_present(void) { return s_mounted; }

const char* sdcard_get_mount_point(void) { return s_mounted ? MOUNT_POINT : NULL; }

#else
/* Host build stubs */
int sdcard_init(void) { return -1; }
bool sdcard_is_present(void) { return false; }
const char* sdcard_get_mount_point(void) { return NULL; }
#endif /* ESP_PLATFORM */
