#include "board_config.h"

/* The POSIX/Linux simulator has no SPI/GPIO drivers: rails/bus init below
 * compiles out there just like on the plain host build. */
#if defined(ESP_PLATFORM) && !defined(CONFIG_IDF_TARGET_LINUX)
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#endif

/* Include the correct board config */
#if defined(CONFIG_BRAMBLE_BOARD_TDECK_PLUS)
#include "boards/tdeck_plus.h"
#elif defined(CONFIG_BRAMBLE_BOARD_HELTEC_V4)
#include "boards/heltec_v4.h"
#elif defined(CONFIG_BRAMBLE_BOARD_PAGER)
#include "boards/bramble_pager.h"
#elif defined(CONFIG_BRAMBLE_BOARD_VIRTUAL_PAGER)
#include "boards/virtual_pager.h"
#elif defined(CONFIG_BRAMBLE_BOARD_VIRTUAL_HELTEC)
#include "boards/virtual_heltec.h"
#elif defined(CONFIG_BRAMBLE_BOARD_HELTEC_V3) || !defined(CONFIG_BRAMBLE_BOARD_CUSTOM)
#include "boards/heltec_v3.h"
#endif

#ifdef ESP_PLATFORM
#include "freertos/semphr.h"

#ifndef CONFIG_IDF_TARGET_LINUX
static const char* TAG = "board";
static bool s_initialized = false;
#endif

/* Shared SPI mutex: created for boards with BOARD_CAP_SHARED_SPI */
SemaphoreHandle_t g_spi_mutex = NULL;
#endif

const bramble_board_config_t* board_get_config(void) {
#if defined(CONFIG_BRAMBLE_BOARD_TDECK_PLUS)
    return &board_tdeck_plus;
#elif defined(CONFIG_BRAMBLE_BOARD_HELTEC_V4)
    return &board_heltec_v4;
#elif defined(CONFIG_BRAMBLE_BOARD_PAGER)
    return &board_bramble_pager;
#elif defined(CONFIG_BRAMBLE_BOARD_VIRTUAL_PAGER)
    return &board_virtual_pager;
#elif defined(CONFIG_BRAMBLE_BOARD_VIRTUAL_HELTEC)
    return &board_virtual_heltec;
#else
    return &board_heltec_v3;
#endif
}

int board_init(void) {
#if defined(ESP_PLATFORM) && !defined(CONFIG_IDF_TARGET_LINUX)
    const bramble_board_config_t* cfg = board_get_config();
    ESP_LOGI(TAG, "Board: %s", cfg->name);

    /* Step 1: Enable peripheral power rail if needed */
    if (cfg->capabilities & BOARD_CAP_PERIPHERAL_POWER) {
        gpio_config_t pwr_conf = {
            .pin_bit_mask = (1ULL << cfg->peripheral_power_pin),
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&pwr_conf);
        gpio_set_level(cfg->peripheral_power_pin, 1);
        ESP_LOGI(TAG, "Peripheral power enabled (GPIO%d)", cfg->peripheral_power_pin);
        vTaskDelay(pdMS_TO_TICKS(100)); /* Let rails stabilize */
    }

    /* Step 2: Initialize shared SPI bus if board has shared SPI */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = cfg->spi.mosi,
        .miso_io_num = cfg->spi.miso,
        .sclk_io_num = cfg->spi.sck,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = cfg->spi_max_transfer_sz,
    };

    if (cfg->capabilities & BOARD_CAP_SHARED_SPI) {
        /* CRITICAL: Drive ALL SPI CS pins HIGH before bus init.
         * On shared SPI buses, floating CS pins cause peripherals to
         * receive each other's traffic, corrupting display GRAM, etc.
         * (Learned from Bramble's earlyInitVariant pattern.) */
        /* .cs sits at offset 0 in every display-pin union variant, so
         * spi_display.cs also reads the e-paper CS on epd_display boards. */
        const int cs_pins[] = {
            cfg->radio.cs,
            cfg->spi_display.cs,
            cfg->sdcard_cs,
        };
        for (int i = 0; i < 3; i++) {
            if (cs_pins[i] >= 0) {
                gpio_config_t cs_conf = {
                    .pin_bit_mask = (1ULL << cs_pins[i]),
                    .mode = GPIO_MODE_OUTPUT,
                };
                gpio_config(&cs_conf);
                gpio_set_level(cs_pins[i], 1); /* Deselect */
            }
        }
        ESP_LOGI(TAG, "All SPI CS pins driven HIGH");

        /* Now safe to initialize the shared SPI bus */
        esp_err_t err = spi_bus_initialize(cfg->spi_host, &bus_cfg, SPI_DMA_CH_AUTO);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
            return -1;
        }
        ESP_LOGI(TAG, "Shared SPI bus initialized");
    }
    /* Non-shared SPI boards: radio driver inits its own bus (existing behavior) */

    /* Serialize multi-command SPI sequences on every board, not only shared-SPI
     * ones. spi_device_acquire_bus keeps a single transfer atomic, but a radio
     * command sequence (BUSY wait, CS low, transfer, CS high) or a full
     * radio_reconfigure spans several commands, and on a non-shared board this
     * mutex was left NULL, making spi_mutex_take() a no-op so two radio callers
     * could still interleave. On shared-SPI boards it additionally coordinates
     * radio traffic with the display. Either way it must exist before either
     * subsystem starts, and both start after board_init returns (issue #82). */
    g_spi_mutex = xSemaphoreCreateMutex();
    if (!g_spi_mutex) {
        ESP_LOGE(TAG, "Failed to create SPI mutex");
        return -1;
    }
    ESP_LOGI(TAG, "SPI serialization mutex created");

    s_initialized = true;
#endif
    return 0;
}
