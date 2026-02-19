#include "touch.h"
#include "board_config.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "gt911";

#define GT911_COORD_ADDR    0x814E
#define GT911_PRODUCT_ID    0x8140

#define GT911_I2C_PORT      I2C_NUM_0
#define GT911_TIMEOUT_MS    100

static uint8_t gt911_addr = 0;
static bool initialized = false;

static esp_err_t gt911_read_reg(uint16_t reg, uint8_t *data, size_t len) {
    uint8_t reg_buf[2] = { reg >> 8, reg & 0xFF };
    esp_err_t ret = i2c_master_write_read_device(
        GT911_I2C_PORT, gt911_addr, reg_buf, 2, data, len,
        pdMS_TO_TICKS(GT911_TIMEOUT_MS));
    return ret;
}

static esp_err_t gt911_write_reg(uint16_t reg, uint8_t *data, size_t len) {
    uint8_t *buf = malloc(2 + len);
    if (!buf) return ESP_ERR_NO_MEM;
    buf[0] = reg >> 8;
    buf[1] = reg & 0xFF;
    memcpy(buf + 2, data, len);
    esp_err_t ret = i2c_master_write_to_device(
        GT911_I2C_PORT, gt911_addr, buf, 2 + len,
        pdMS_TO_TICKS(GT911_TIMEOUT_MS));
    free(buf);
    return ret;
}

int touch_init(void) {
    const bramble_board_config_t *board = board_get_config();
    if (!(board->capabilities & BOARD_CAP_TOUCH)) {
        ESP_LOGI(TAG, "No touch capability - skipping");
        return 0;
    }

    if (board->touch.int_pin >= 0) {
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << board->touch.int_pin),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);
    }

    uint8_t addrs[] = { board->touch.i2c_addr,
                        board->touch.i2c_addr == 0x14 ? (uint8_t)0x5D : (uint8_t)0x14 };
    uint8_t product_id[4] = {0};
    
    for (int i = 0; i < 2; i++) {
        gt911_addr = addrs[i];
        if (gt911_read_reg(GT911_PRODUCT_ID, product_id, 4) == ESP_OK) {
            ESP_LOGI(TAG, "GT911 found at 0x%02X, product: %.4s",
                     gt911_addr, product_id);
            initialized = true;
            return 0;
        }
    }

    ESP_LOGW(TAG, "GT911 not found at 0x%02X or 0x%02X", addrs[0], addrs[1]);
    return -1;
}

bool touch_read(touch_point_t *point) {
    if (!initialized || !point) return false;

    uint8_t status = 0;
    if (gt911_read_reg(GT911_COORD_ADDR, &status, 1) != ESP_OK) {
        return false;
    }

    bool ready = (status & 0x80) != 0;
    int num_points = status & 0x0F;

    if (!ready || num_points == 0) {
        point->pressed = false;
        uint8_t zero = 0;
        gt911_write_reg(GT911_COORD_ADDR, &zero, 1);
        return true;
    }

    uint8_t data[8] = {0};
    if (gt911_read_reg(GT911_COORD_ADDR + 2, data, 8) != ESP_OK) {
        return false;
    }

    point->x = (data[1] << 8) | data[0];
    point->y = (data[3] << 8) | data[2];
    point->pressed = true;

    uint8_t zero = 0;
    gt911_write_reg(GT911_COORD_ADDR, &zero, 1);

    return true;
}
