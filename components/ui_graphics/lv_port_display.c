#include "lv_port_display.h"
#include "display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <inttypes.h>

static const char *TAG = "lv_port_disp";

#define DISP_HOR_RES 320
#define DISP_VER_RES 240
#define BUF_LINES 40
#define BUF_SIZE (DISP_HOR_RES * BUF_LINES * sizeof(lv_color16_t))

static uint32_t flush_count = 0;

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    flush_count++;
    if (flush_count <= 10 || (flush_count % 100) == 0) {
        ESP_LOGI(TAG, "flush_cb #%"PRIu32": (%"PRId32",%"PRId32")-(%"PRId32",%"PRId32")",
                 flush_count, area->x1, area->y1, area->x2, area->y2);
    }
    display_flush_area(area->x1, area->y1, area->x2, area->y2,
                       (const uint16_t *)px_map);
    lv_display_flush_ready(disp);
}

lv_display_t *lv_port_display_init(void) {
    /* Allocate from PSRAM — display_flush_area() handles the PSRAM→internal
     * RAM copy needed for DMA-safe SPI transfers on ESP32-S3. */
    void *buf1 = heap_caps_malloc(BUF_SIZE, MALLOC_CAP_SPIRAM);
    void *buf2 = heap_caps_malloc(BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!buf1 || !buf2) {
        ESP_LOGE(TAG, "Failed to allocate LVGL draw buffers");
        return NULL;
    }

    lv_display_t *disp = lv_display_create(DISP_HOR_RES, DISP_VER_RES);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_buffers(disp, buf1, buf2, BUF_SIZE,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);

    ESP_LOGI(TAG, "LVGL display port initialized (%dx%d, %d-line buffers)",
             DISP_HOR_RES, DISP_VER_RES, BUF_LINES);
    return disp;
}
