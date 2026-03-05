#include "touch.h"
#include "board_config.h"
#include "keyboard.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <inttypes.h>

static const char* TAG = "gt911";

/* GT911 registers */
#define GT911_COORD_ADDR 0x814E
#define GT911_PRODUCT_ID 0x8140

static i2c_master_dev_handle_t gt911_dev = NULL;
static bool initialized = false;

/* GT911 resolution — read during init, used for coordinate mapping */
static uint16_t gt911_x_res = 320;
static uint16_t gt911_y_res = 240;

static esp_err_t gt911_read_reg(uint16_t reg, uint8_t* data, size_t len) {
    uint8_t reg_buf[2] = {reg >> 8, reg & 0xFF};
    return i2c_master_transmit_receive(gt911_dev, reg_buf, 2, data, len, 100);
}

static esp_err_t gt911_write_reg(uint16_t reg, uint8_t* data, size_t len) {
    uint8_t* buf = malloc(2 + len);
    if (!buf)
        return ESP_ERR_NO_MEM;
    buf[0] = reg >> 8;
    buf[1] = reg & 0xFF;
    memcpy(buf + 2, data, len);
    esp_err_t ret = i2c_master_transmit(gt911_dev, buf, 2 + len, 100);
    free(buf);
    return ret;
}

static void gt911_read_config(void) {
    uint8_t cfg[4] = {0};
    if (gt911_read_reg(0x8048, cfg, 4) == ESP_OK) {
        gt911_x_res = cfg[0] | (cfg[1] << 8);
        gt911_y_res = cfg[2] | (cfg[3] << 8);
    }
    uint8_t mod = 0;
    gt911_read_reg(0x8047, &mod, 1);
    ESP_LOGI(TAG, "GT911 config: %dx%d, module=0x%02X", gt911_x_res, gt911_y_res, mod);
}

int touch_init(void) {
    const bramble_board_config_t* board = board_get_config();
    if (!(board->capabilities & BOARD_CAP_TOUCH)) {
        ESP_LOGI(TAG, "No touch capability - skipping");
        return 0;
    }

    i2c_master_bus_handle_t bus = keyboard_get_i2c_bus();
    if (!bus) {
        ESP_LOGE(TAG, "I2C bus not available (keyboard not initialized?)");
        return -1;
    }

    /* NOTE: Do NOT configure the INT pin — leave it floating.
     * The GT911 on T-Deck Plus works in polling mode without INT.
     * Configuring it as input can interfere with touch detection. */

    /* Try both GT911 addresses (0x14 and 0x5D) */
    uint8_t addrs[] = {board->touch.i2c_addr,
                       board->touch.i2c_addr == 0x14 ? (uint8_t)0x5D : (uint8_t)0x14};

    for (int i = 0; i < 2; i++) {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addrs[i],
            .scl_speed_hz = 400000,
        };

        if (i2c_master_bus_add_device(bus, &dev_cfg, &gt911_dev) != ESP_OK)
            continue;

        uint8_t product_id[4] = {0};
        if (gt911_read_reg(GT911_PRODUCT_ID, product_id, 4) == ESP_OK) {
            ESP_LOGI(TAG, "GT911 found at 0x%02X, product: %.4s", addrs[i], product_id);
            initialized = true;
            gt911_read_config();
            return 0;
        }

        i2c_master_bus_rm_device(gt911_dev);
        gt911_dev = NULL;
    }

    ESP_LOGW(TAG, "GT911 not found");
    return -1;
}

bool touch_read(touch_point_t* point) {
    if (!initialized || !point)
        return false;

    uint8_t status = 0;
    if (gt911_read_reg(GT911_COORD_ADDR, &status, 1) != ESP_OK)
        return false;

    bool ready = (status & 0x80) != 0;
    int num_points = status & 0x0F;

    if (!ready || num_points == 0) {
        point->pressed = false;
        uint8_t zero = 0;
        gt911_write_reg(GT911_COORD_ADDR, &zero, 1);
        return true;
    }

    uint8_t data[8] = {0};
    if (gt911_read_reg(GT911_COORD_ADDR + 2, data, 8) != ESP_OK)
        return false;

    uint16_t raw_x = (data[1] << 8) | data[0];
    uint16_t raw_y = (data[3] << 8) | data[2];

    /* Map GT911 coordinates to display 320x240.
     * The display uses MADCTL 0x68 (MV=1, MX=1) for landscape rotation.
     * GT911 reports in the panel's native portrait orientation, so we must
     * swap axes and invert to match the display's logical coordinates:
     *   display_x = raw_y (mapped to 0..319)
     *   display_y = (gt911_x_res - 1 - raw_x) (mapped to 0..239)
     */
    if (gt911_x_res > 0 && gt911_y_res > 0) {
        point->x = (int)(raw_y * 320 / gt911_y_res);
        point->y = (int)((gt911_x_res - 1 - raw_x) * 240 / gt911_x_res);
    } else {
        point->x = raw_y;
        point->y = raw_x;
    }

    /* Clamp */
    if (point->x >= 320)
        point->x = 319;
    if (point->y >= 240)
        point->y = 239;
    if (point->x < 0)
        point->x = 0;
    if (point->y < 0)
        point->y = 0;
    point->pressed = true;

    static uint32_t touch_log_count = 0;
    if (++touch_log_count <= 5) {
        ESP_LOGI(TAG, "Touch: raw(%u,%u) → (%d,%d)", raw_x, raw_y, point->x, point->y);
    }

    uint8_t zero = 0;
    gt911_write_reg(GT911_COORD_ADDR, &zero, 1);
    return true;
}
