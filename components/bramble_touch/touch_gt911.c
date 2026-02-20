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

static const char *TAG = "gt911";

/* GT911 registers */
#define GT911_COORD_ADDR    0x814E
#define GT911_PRODUCT_ID    0x8140
#define GT911_CONFIG_START  0x8047
#define GT911_CONFIG_CHKSUM 0x80FF
#define GT911_CONFIG_FRESH  0x8100

/* Config register offsets (from 0x8047) */
#define CFG_OFF_MODULE_SW   0        /* 0x8047 */
#define CFG_OFF_X_RES_L     1        /* 0x8048 */
#define CFG_OFF_X_RES_H     2        /* 0x8049 */
#define CFG_OFF_Y_RES_L     3        /* 0x804A */
#define CFG_OFF_Y_RES_H     4        /* 0x804B */

#define GT911_CONFIG_LEN    (0x80FF - 0x8047)  /* 184 bytes */

static i2c_master_dev_handle_t gt911_dev = NULL;
static bool initialized = false;
static uint16_t touch_x_max = 320;
static uint16_t touch_y_max = 240;

static esp_err_t gt911_read_reg(uint16_t reg, uint8_t *data, size_t len) {
    uint8_t reg_buf[2] = { reg >> 8, reg & 0xFF };
    return i2c_master_transmit_receive(gt911_dev, reg_buf, 2, data, len, 100);
}

static esp_err_t gt911_write_reg(uint16_t reg, uint8_t *data, size_t len) {
    uint8_t *buf = malloc(2 + len);
    if (!buf) return ESP_ERR_NO_MEM;
    buf[0] = reg >> 8;
    buf[1] = reg & 0xFF;
    memcpy(buf + 2, data, len);
    esp_err_t ret = i2c_master_transmit(gt911_dev, buf, 2 + len, 100);
    free(buf);
    return ret;
}

/* Write GT911 config: set resolution to 320x240 landscape */
static void gt911_configure_landscape(void) {
    uint8_t cfg[GT911_CONFIG_LEN];
    
    /* Read current config */
    if (gt911_read_reg(GT911_CONFIG_START, cfg, GT911_CONFIG_LEN) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read GT911 config");
        return;
    }
    
    uint16_t cur_x = cfg[CFG_OFF_X_RES_L] | (cfg[CFG_OFF_X_RES_H] << 8);
    uint16_t cur_y = cfg[CFG_OFF_Y_RES_L] | (cfg[CFG_OFF_Y_RES_H] << 8);
    uint8_t cur_mod = cfg[CFG_OFF_MODULE_SW];
    
    ESP_LOGI(TAG, "GT911 current: %dx%d, module=0x%02X", cur_x, cur_y, cur_mod);
    
    /* Check if already configured correctly */
    if (cur_x == 320 && cur_y == 240) {
        ESP_LOGI(TAG, "GT911 already configured for 320x240");
        touch_x_max = 320;
        touch_y_max = 240;
        return;
    }
    
    /* Set resolution to 320x240 */
    cfg[CFG_OFF_X_RES_L] = 320 & 0xFF;
    cfg[CFG_OFF_X_RES_H] = 320 >> 8;
    cfg[CFG_OFF_Y_RES_L] = 240 & 0xFF;
    cfg[CFG_OFF_Y_RES_H] = 240 >> 8;
    
    /* Module switch: keep XY swap if set, clear inversions for now.
     * We'll verify orientation after and adjust. */
    
    /* Compute checksum: complement of sum of all config bytes + 1 */
    uint8_t chksum = 0;
    for (int i = 0; i < GT911_CONFIG_LEN; i++) {
        chksum += cfg[i];
    }
    chksum = (~chksum) + 1;
    
    /* Write config */
    if (gt911_write_reg(GT911_CONFIG_START, cfg, GT911_CONFIG_LEN) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to write GT911 config");
        return;
    }
    
    /* Write checksum */
    if (gt911_write_reg(GT911_CONFIG_CHKSUM, &chksum, 1) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to write GT911 checksum");
        return;
    }
    
    /* Write config fresh flag (1 = update config) */
    uint8_t fresh = 1;
    if (gt911_write_reg(GT911_CONFIG_FRESH, &fresh, 1) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to write GT911 fresh flag");
        return;
    }
    
    vTaskDelay(pdMS_TO_TICKS(10));
    
    touch_x_max = 320;
    touch_y_max = 240;
    ESP_LOGI(TAG, "GT911 reconfigured to 320x240 (checksum=0x%02X)", chksum);
}

int touch_init(void) {
    const bramble_board_config_t *board = board_get_config();
    if (!(board->capabilities & BOARD_CAP_TOUCH)) {
        ESP_LOGI(TAG, "No touch capability - skipping");
        return 0;
    }

    /* Get shared I2C bus from keyboard driver */
    i2c_master_bus_handle_t bus = keyboard_get_i2c_bus();
    if (!bus) {
        ESP_LOGE(TAG, "I2C bus not available (keyboard not initialized?)");
        return -1;
    }

    /* Configure interrupt pin as input */
    if (board->touch.int_pin >= 0) {
        gpio_config_t io_in = {
            .pin_bit_mask = (1ULL << board->touch.int_pin),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_in);
    }

    /* Try both GT911 addresses (0x14 and 0x5D) */
    uint8_t addrs[] = { board->touch.i2c_addr,
                        board->touch.i2c_addr == 0x14 ? (uint8_t)0x5D : (uint8_t)0x14 };
    
    for (int i = 0; i < 2; i++) {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addrs[i],
            .scl_speed_hz = 400000,
        };
        
        if (i2c_master_bus_add_device(bus, &dev_cfg, &gt911_dev) != ESP_OK) {
            continue;
        }
        
        uint8_t product_id[4] = {0};
        if (gt911_read_reg(GT911_PRODUCT_ID, product_id, 4) == ESP_OK) {
            ESP_LOGI(TAG, "GT911 found at 0x%02X, product: %.4s",
                     addrs[i], product_id);
            initialized = true;
            gt911_configure_landscape();
            return 0;
        }
        
        i2c_master_bus_rm_device(gt911_dev);
        gt911_dev = NULL;
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
        /* Clear status register */
        uint8_t zero = 0;
        gt911_write_reg(GT911_COORD_ADDR, &zero, 1);
        return true;
    }

    /* Read first touch point (8 bytes: track_id, x_lo, x_hi, y_lo, y_hi, w_lo, w_hi, reserved) */
    uint8_t data[8] = {0};
    if (gt911_read_reg(GT911_COORD_ADDR + 2, data, 8) != ESP_OK) {
        return false;
    }

    uint16_t raw_x = (data[1] << 8) | data[0];
    uint16_t raw_y = (data[3] << 8) | data[2];

    /* Clamp to display bounds */
    point->x = (raw_x >= 320) ? 319 : raw_x;
    point->y = (raw_y >= 240) ? 239 : raw_y;
    point->pressed = true;

    static uint32_t touch_log_count = 0;
    if (++touch_log_count <= 20) {
        ESP_LOGI(TAG, "Touch: raw(%d,%d) → (%d,%d)", raw_x, raw_y, point->x, point->y);
    }

    /* Clear status register */
    uint8_t zero = 0;
    gt911_write_reg(GT911_COORD_ADDR, &zero, 1);

    return true;
}
