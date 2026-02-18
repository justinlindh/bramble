#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "display.h"
#include "button.h"
#include "ui.h"
#include "crypto.h"

static const char *TAG = "bramble";

/* ── Splash screen ──────────────────────────────────────────────────── */

static void show_splash(void) {
    display_clear();

    /* "BRAMBLE" in large text, centered */
    display_draw_text_large(16, 8, "BRAMBLE");

    /* Divider line */
    display_hline(10, 30, 108);

    /* Tagline */
    display_draw_text(16, 36, "LoRa Mesh Network");

    /* Version */
    display_draw_text(34, 50, "v0.1.0-dev");

    display_flush();
}

/* ── Screen renderers ───────────────────────────────────────────────── */

static uint32_t my_addr = 0;
static uint32_t boot_time_ms = 0;

static void render_main_screen(void) {
    display_clear();

    /* Header */
    display_draw_text(0, 0, "Bramble");
    display_hline(0, 10, 128);

    /* Node address */
    char line[48];
    snprintf(line, sizeof(line), "Node: %08" PRIX32, my_addr);
    display_draw_text(0, 14, line);

    /* Neighbors (stub — no radio yet) */
    display_draw_text(0, 24, "Peers: 0  (no radio)");

    /* Uptime */
    uint32_t up_sec = (uint32_t)((esp_timer_get_time() / 1000000ULL) -
                                  (boot_time_ms / 1000));
    char uptime[32];
    ui_format_uptime(up_sec, uptime, sizeof(uptime));
    snprintf(line, sizeof(line), "Up: %s", uptime);
    display_draw_text(0, 34, line);

    /* Status */
    display_draw_text(0, 48, "Radio: not init");
    display_draw_text(0, 56, "[press] cycle screens");

    display_flush();
}

static void render_screen(ui_state_t *ui) {
    switch (ui_get_screen(ui)) {
    case SCREEN_MAIN:
        render_main_screen();
        break;
    case SCREEN_MESSAGES:
        display_clear();
        display_draw_text(0, 0, "Messages");
        display_hline(0, 10, 128);
        display_draw_text(0, 24, "(no messages yet)");
        display_draw_text(0, 56, "[press] next screen");
        display_flush();
        break;
    case SCREEN_NODES:
        display_clear();
        display_draw_text(0, 0, "Nodes");
        display_hline(0, 10, 128);
        display_draw_text(0, 24, "(no neighbors yet)");
        display_draw_text(0, 56, "[press] next screen");
        display_flush();
        break;
    case SCREEN_SETTINGS:
        display_clear();
        display_draw_text(0, 0, "Settings");
        display_hline(0, 10, 128);
        char line[32];
        snprintf(line, sizeof(line), "Addr: %08" PRIX32, my_addr);
        display_draw_text(0, 14, line);
        display_draw_text(0, 24, "Radio: SF9 BW125");
        display_draw_text(0, 34, "Freq: 915.0 MHz");
        display_draw_text(0, 44, "TX: 17 dBm");
        display_draw_text(0, 56, "[press] next screen");
        display_flush();
        break;
    default:
        display_clear();
        display_draw_text(0, 28, "Unknown screen");
        display_flush();
        break;
    }
}

/* ── Main ───────────────────────────────────────────────────────────── */

void app_main(void)
{
    ESP_LOGI(TAG, "Bramble LoRa Mesh starting...");

    /* NVS init */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialized");

    /* Generate node address from random bytes */
    uint8_t addr_bytes[4];
    crypto_random(addr_bytes, 4);
    my_addr = (uint32_t)(addr_bytes[0] | (addr_bytes[1] << 8) |
                         (addr_bytes[2] << 16) | (addr_bytes[3] << 24));
    /* TODO: persist in NVS and derive from keypair */
    ESP_LOGI(TAG, "Node address: %08" PRIX32, my_addr);

    boot_time_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

    /* Init display */
    if (display_init() != 0) {
        ESP_LOGE(TAG, "Display init failed!");
    } else {
        show_splash();
        ESP_LOGI(TAG, "Splash screen displayed");
        vTaskDelay(pdMS_TO_TICKS(2000)); /* Show splash for 2 seconds */
    }

    /* Init button */
    button_init();

    /* Init UI state machine */
    ui_state_t ui;
    ui_init(&ui);

    /* Render initial screen */
    render_screen(&ui);

    ESP_LOGI(TAG, "Entering main loop");

    /* Main loop — 50ms tick (20 Hz) */
    while (1) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

        /* Poll button */
        ui_button_t btn = button_poll(now_ms);
        if (btn != BTN_NONE) {
            ESP_LOGI(TAG, "Button event: %d", btn);
            ui_handle_button(&ui, btn, now_ms);
        }

        /* Check inactivity timeout */
        ui_check_timeout(&ui, now_ms);

        /* Redraw if needed */
        if (ui_needs_redraw(&ui)) {
            render_screen(&ui);
            ui_mark_drawn(&ui);
        }

        /* Periodic refresh of main screen (uptime counter) */
        if (ui_get_screen(&ui) == SCREEN_MAIN && (now_ms % 1000) < 50) {
            render_main_screen();
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
