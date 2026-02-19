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

static i2c_master_dev_handle_t gt911_dev = NULL;
static bool initialized = false;
static void gt911_dump_config(void);

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

    /* GT911 reset sequence: drive INT pin to select I2C address.
     * INT LOW during reset → address 0x5D, INT HIGH → 0x14.
     * The T-Deck Plus GT911 defaults to 0x5D. */
    if (board->touch.int_pin >= 0) {
        /* Drive INT as output LOW to select 0x5D address */
        gpio_config_t io_out = {
            .pin_bit_mask = (1ULL << board->touch.int_pin),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_out);
        gpio_set_level(board->touch.int_pin, 0);
        vTaskDelay(pdMS_TO_TICKS(10));

        /* Release INT pin back to input after address selection */
        gpio_config_t io_in = {
            .pin_bit_mask = (1ULL << board->touch.int_pin),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_in);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    /* Try both GT911 addresses */
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
            gt911_dump_config();
            return 0;
        }
        
        /* Remove device if probe failed, try next address */
        i2c_master_bus_rm_device(gt911_dev);
        gt911_dev = NULL;
    }

    ESP_LOGW(TAG, "GT911 not found at 0x%02X or 0x%02X", addrs[0], addrs[1]);
    return -1;
}

static void gt911_dump_config(void) {
    /* Read resolution config from GT911 registers */
    uint8_t cfg[4] = {0};
    if (gt911_read_reg(0x8048, cfg, 4) == ESP_OK) {
        uint16_t x_res = cfg[0] | (cfg[1] << 8);
        uint16_t y_res = cfg[2] | (cfg[3] << 8);
        ESP_LOGI(TAG, "GT911 config resolution: %dx%d", x_res, y_res);
    }
    uint8_t mod = 0;
    if (gt911_read_reg(0x8047, &mod, 1) == ESP_OK) {
        ESP_LOGI(TAG, "GT911 module switch: 0x%02X (bit3=XY_swap, bit2=Y_inv, bit1=X_inv)", mod);
    }
}

bool touch_read(touch_point_t *point) {
    if (!initialized || !point) return false;

    uint8_t status = 0;
    esp_err_t err = gt911_read_reg(GT911_COORD_ADDR, &status, 1);
    if (err != ESP_OK) {
        static uint32_t err_count = 0;
        if (++err_count <= 5) {
            ESP_LOGW(TAG, "Status read failed: 0x%x", err);
        }
        return false;
    }

    bool ready = (status & 0x80) != 0;
    int num_points = status & 0x0F;

    static uint32_t poll_count = 0;
    poll_count++;
    if (poll_count <= 3 || (ready && poll_count <= 20)) {
        ESP_LOGI(TAG, "Poll #%"PRIu32" status=0x%02X ready=%d pts=%d",
                 poll_count, status, ready, num_points);
    }

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

    static uint32_t touch_log_count = 0;
    if (++touch_log_count <= 10) {
        ESP_LOGI(TAG, "Touch: x=%d y=%d", point->x, point->y);
    }

    uint8_t zero = 0;
    gt911_write_reg(GT911_COORD_ADDR, &zero, 1);

    return true;
}
