#include "battery.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"

static const char *TAG = "battery";

/* Heltec V3: battery voltage on GPIO1 = ADC1_CHANNEL_0
 * Voltage divider: 390K / 100K → factor = (390+100)/100 = 4.9
 * But Heltec schematic shows 2x 100K divider → factor = 2.0
 * Measured: ~2100 raw at 4.2V → factor ≈ 2.0 confirmed */
#define BATTERY_ADC_UNIT    ADC_UNIT_1
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_0  /* GPIO1 */
#define BATTERY_ADC_ATTEN   ADC_ATTEN_DB_12
#define VOLTAGE_DIVIDER_FACTOR 2

static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t s_cali_handle = NULL;
static bool s_initialized = false;

void battery_init(void)
{
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
        .atten = BATTERY_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_oneshot_config_channel(s_adc_handle, BATTERY_ADC_CHANNEL, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC channel config failed: %d", err);
        return;
    }

    /* Calibration — try curve fitting first, fall back to line fitting */
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = BATTERY_ADC_UNIT,
        .chan = BATTERY_ADC_CHANNEL,
        .atten = BATTERY_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Curve fitting cali failed, trying line fitting");
        adc_cali_line_fitting_config_t line_cfg = {
            .unit_id = BATTERY_ADC_UNIT,
            .atten = BATTERY_ADC_ATTEN,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        err = adc_cali_create_scheme_line_fitting(&line_cfg, &s_cali_handle);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "No ADC calibration available — raw readings only");
            s_cali_handle = NULL;
        }
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Battery ADC initialized (GPIO1, channel %d)", BATTERY_ADC_CHANNEL);
}

uint32_t battery_read_mv(void)
{
    if (!s_initialized || !s_adc_handle) return 0;

    int raw = 0;
    esp_err_t err = adc_oneshot_read(s_adc_handle, BATTERY_ADC_CHANNEL, &raw);
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

    /* Apply voltage divider factor */
    return (uint32_t)(voltage_mv * VOLTAGE_DIVIDER_FACTOR);
}

uint8_t battery_mv_to_pct(uint32_t mv)
{
    /* LiPo discharge curve approximation:
     * 4200 mV = 100%, 4060 mV = 90%, 3900 mV = 70%,
     * 3800 mV = 50%, 3700 mV = 30%, 3600 mV = 15%,
     * 3300 mV = 0% (cutoff) */
    if (mv >= 4200) return 100;
    if (mv <= 3300) return 0;

    /* Piecewise linear approximation */
    static const struct { uint32_t mv; uint8_t pct; } curve[] = {
        { 4200, 100 }, { 4060, 90 }, { 3900, 70 }, { 3800, 50 },
        { 3700, 30 },  { 3600, 15 }, { 3300, 0 },
    };
    for (int i = 0; i < 6; i++) {
        if (mv >= curve[i + 1].mv) {
            uint32_t range_mv = curve[i].mv - curve[i + 1].mv;
            uint8_t range_pct = curve[i].pct - curve[i + 1].pct;
            return curve[i + 1].pct +
                   (uint8_t)((mv - curve[i + 1].mv) * range_pct / range_mv);
        }
    }
    return 0;
}

uint8_t battery_read_pct(void)
{
    uint32_t mv = battery_read_mv();
    return battery_mv_to_pct(mv);
}
