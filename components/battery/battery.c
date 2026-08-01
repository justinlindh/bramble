#include "battery.h"
#include "board_config.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
/* ESP32-S3 supports curve fitting calibration only (no line fitting) */
#include "esp_log.h"
#include "driver/gpio.h"
#include <string.h>

static const char* TAG = "battery";

/* ADC unit */
#define BATTERY_ADC_UNIT ADC_UNIT_1

static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t s_cali_handle = NULL;
static bool s_initialized = false;
static const bramble_board_config_t* s_board = NULL;
static adc_atten_t s_batt_atten = ADC_ATTEN_DB_12;

void battery_init(void) {
    /* Get board configuration */
    s_board = board_get_config();

    /* Check if board has battery ADC capability */
    if (!(s_board->capabilities & BOARD_CAP_BATTERY_ADC)) {
        ESP_LOGI(TAG, "Board has no battery ADC support");
        return;
    }

    /* Heltec V4 requires ADC rail enable before battery reads are valid. */
    if (s_board->short_name && strcmp(s_board->short_name, "heltec_v4") == 0) {
        gpio_config_t adc_ctrl = {
            .pin_bit_mask = (1ULL << 37),
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&adc_ctrl);
        gpio_set_level(37, 1);
        s_batt_atten = ADC_ATTEN_DB_2_5;
        ESP_LOGI(TAG, "Heltec V4 battery ADC rail enabled (GPIO37 HIGH), atten=2.5dB");
    }

    /* Configure ADC unit */
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = BATTERY_ADC_UNIT,
    };
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC unit init failed: %d", err);
        return;
    }

    /* Configure channel */
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = s_batt_atten,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_oneshot_config_channel(s_adc_handle, s_board->battery.adc_channel, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC channel config failed: %d", err);
        return;
    }

    /* Calibration: try curve fitting first, fall back to line fitting */
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = BATTERY_ADC_UNIT,
        .chan = s_board->battery.adc_channel,
        .atten = s_batt_atten,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ADC calibration failed (%d), raw readings only", err);
        s_cali_handle = NULL;
    }

    /* Optional hardware charge-detect pin (wave 2). No ESP board has this
     * wired today (see board_config.h's charge struct, {-1,0,-1} on every
     * ESP profile); when one does, configure it as an input here so
     * battery_get_status can read it. */
    if (s_board->charge.chrg_gpio >= 0) {
        gpio_config_t chrg_cfg = {
            .pin_bit_mask = (1ULL << s_board->charge.chrg_gpio),
            .mode = GPIO_MODE_INPUT,
        };
        gpio_config(&chrg_cfg);
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Battery ADC initialized (GPIO%d, channel %d, divider=%dx)",
             s_board->battery.gpio, s_board->battery.adc_channel, s_board->battery.divider_factor);
}

/* One raw-ADC-to-millivolts conversion, pre-divider-factor. Shared by every
 * sample battery_get_status averages. Returns 0 on ADC read failure, same
 * degradation the pre-averaging single-sample code had. */
static uint32_t read_one_sample_mv(void) {
    int raw = 0;
    esp_err_t err = adc_oneshot_read(s_adc_handle, s_board->battery.adc_channel, &raw);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ADC read failed: %d", err);
        return 0;
    }

    int voltage_mv = 0;
    if (s_cali_handle) {
        adc_cali_raw_to_voltage(s_cali_handle, raw, &voltage_mv);
    } else {
        /* Rough estimate without calibration: 12-bit ADC, 0-3.3V range at 12dB atten */
        voltage_mv = (raw * 3300) / 4095;
    }
    return (uint32_t)voltage_mv;
}

/* battery_mv_to_pct lives in battery_pct.c, shared with the virtual driver. */

void battery_get_status(battery_status_t* out) {
    memset(out, 0, sizeof(*out));

    if (!s_initialized || !s_adc_handle || !s_board) {
        out->charging = BATTERY_CHG_UNKNOWN;
        return;
    }

    /* Average BATTERY_AVG_SAMPLE_COUNT raw conversions before applying the
     * board's divider factor. Averaging is linear, so doing it pre-divider
     * (rather than post-multiply) is numerically equivalent and keeps
     * read_one_sample_mv focused on one conversion. */
    uint32_t samples[BATTERY_AVG_SAMPLE_COUNT];
    for (int i = 0; i < BATTERY_AVG_SAMPLE_COUNT; i++)
        samples[i] = read_one_sample_mv();
    uint32_t voltage_mv = battery_average_mv(samples, BATTERY_AVG_SAMPLE_COUNT);

    out->mv = voltage_mv * (uint32_t)s_board->battery.divider_factor;
    out->pct = battery_mv_to_pct(out->mv);
    out->present = true;

    if (s_board->charge.chrg_gpio >= 0) {
        int level = gpio_get_level(s_board->charge.chrg_gpio);
        out->charging = battery_charging_from_gpio(s_board->charge.chrg_gpio,
                                                   s_board->charge.chrg_active_level, level);
    } else {
        out->charging = BATTERY_CHG_UNKNOWN;
    }
}
