#ifndef DRIVER_I2S_STD_H_STUB
#define DRIVER_I2S_STD_H_STUB

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_stubs.h"

typedef void* i2s_chan_handle_t;

typedef struct {
    int id;
    int role;
    bool auto_clear;
} i2s_chan_config_t;

typedef struct {
    struct {
        int sample_rate;
    } clk_cfg;
    struct {
        int data_bit_width;
        int slot_mode;
    } slot_cfg;
    struct {
        int mclk;
        int bclk;
        int ws;
        int dout;
        int din;
        struct {
            bool mclk_inv;
            bool bclk_inv;
            bool ws_inv;
        } invert_flags;
    } gpio_cfg;
} i2s_std_config_t;

#define I2S_NUM_0 0
#define I2S_ROLE_MASTER 1
#define I2S_GPIO_UNUSED (-1)
#define I2S_DATA_BIT_WIDTH_16BIT 16
#define I2S_SLOT_MODE_MONO 1
#define portMAX_DELAY 0xffffffffu

#define I2S_CHANNEL_DEFAULT_CONFIG(_id, _role)                                                     \
    ((i2s_chan_config_t){.id = (_id), .role = (_role), .auto_clear = false})
#define I2S_STD_CLK_DEFAULT_CONFIG(_rate)                                                          \
    ((typeof(((i2s_std_config_t*)0)->clk_cfg)){.sample_rate = (_rate)})
#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(_width, _mode)                                         \
    ((typeof(((i2s_std_config_t*)0)->slot_cfg)){.data_bit_width = (_width), .slot_mode = (_mode)})

esp_err_t i2s_new_channel(const i2s_chan_config_t* chan_cfg, i2s_chan_handle_t* tx_handle,
                          i2s_chan_handle_t* rx_handle);
esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t tx_handle, const i2s_std_config_t* std_cfg);
esp_err_t i2s_channel_enable(i2s_chan_handle_t tx_handle);
esp_err_t i2s_channel_write(i2s_chan_handle_t handle, const void* src, size_t size,
                            size_t* bytes_written, uint32_t timeout_ms);
esp_err_t i2s_channel_disable(i2s_chan_handle_t tx_handle);
esp_err_t i2s_del_channel(i2s_chan_handle_t tx_handle);
const char* esp_err_to_name(esp_err_t err);

#endif
