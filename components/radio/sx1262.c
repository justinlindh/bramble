/*
 * SX1262 low-level SPI driver for Heltec WiFi LoRa 32 V3.
 *
 * Uses ESP-IDF SPI master on SPI2_HOST at 8 MHz.
 * NSS is toggled manually (GPIO) so it stays asserted across multi-byte
 * transfers as the SX1262 datasheet requires.
 */

#ifdef ESP_PLATFORM

#include "sx1262.h"

#include <string.h>
#include <inttypes.h>
#include <math.h>

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sx1262";

static spi_device_handle_t s_spi;

/* ------------------------------------------------------------------ */
/*  Helper: NSS control                                                */
/* ------------------------------------------------------------------ */

static inline void nss_low(void)  { gpio_set_level(SX1262_PIN_NSS, 0); }
static inline void nss_high(void) { gpio_set_level(SX1262_PIN_NSS, 1); }

/* ------------------------------------------------------------------ */
/*  BUSY                                                               */
/* ------------------------------------------------------------------ */

int sx1262_wait_busy(uint32_t timeout_ms)
{
    uint32_t start = xTaskGetTickCount();
    while (gpio_get_level(SX1262_PIN_BUSY)) {
        if ((xTaskGetTickCount() - start) * portTICK_PERIOD_MS >= timeout_ms) {
            ESP_LOGE(TAG, "BUSY timeout (%" PRIu32 " ms)", timeout_ms);
            return -1;
        }
        vTaskDelay(1);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Low-level SPI helpers                                              */
/* ------------------------------------------------------------------ */

static int spi_transfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
    spi_transaction_t t = {
        .length    = len * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    esp_err_t err = spi_device_transmit(s_spi, &t);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI transmit error: %s", esp_err_to_name(err));
        return -1;
    }
    return 0;
}

int sx1262_write_command(uint8_t cmd, const uint8_t *data, size_t len)
{
    if (sx1262_wait_busy(100) != 0) return -1;

    uint8_t tx[1 + len];
    tx[0] = cmd;
    if (len && data) memcpy(tx + 1, data, len);

    nss_low();
    int rc = spi_transfer(tx, NULL, 1 + len);
    nss_high();
    return rc;
}

int sx1262_read_command(uint8_t cmd, uint8_t *data, size_t len)
{
    if (sx1262_wait_busy(100) != 0) return -1;

    /* cmd + 1 NOP (status) + len data bytes */
    size_t total = 2 + len;
    uint8_t tx[total];
    uint8_t rx[total];
    memset(tx, 0x00, total);
    tx[0] = cmd;

    nss_low();
    int rc = spi_transfer(tx, rx, total);
    nss_high();

    if (rc == 0 && data) memcpy(data, rx + 2, len);
    return rc;
}

int sx1262_write_register(uint16_t addr, const uint8_t *data, size_t len)
{
    if (sx1262_wait_busy(100) != 0) return -1;

    size_t total = 3 + len; /* cmd + addr_hi + addr_lo + data */
    uint8_t tx[total];
    tx[0] = SX1262_CMD_WRITE_REGISTER;
    tx[1] = (uint8_t)(addr >> 8);
    tx[2] = (uint8_t)(addr & 0xFF);
    if (len && data) memcpy(tx + 3, data, len);

    nss_low();
    int rc = spi_transfer(tx, NULL, total);
    nss_high();
    return rc;
}

int sx1262_read_register(uint16_t addr, uint8_t *data, size_t len)
{
    if (sx1262_wait_busy(100) != 0) return -1;

    /* cmd + addr_hi + addr_lo + 1 NOP + len data */
    size_t total = 4 + len;
    uint8_t tx[total];
    uint8_t rx[total];
    memset(tx, 0x00, total);
    tx[0] = SX1262_CMD_READ_REGISTER;
    tx[1] = (uint8_t)(addr >> 8);
    tx[2] = (uint8_t)(addr & 0xFF);

    nss_low();
    int rc = spi_transfer(tx, rx, total);
    nss_high();

    if (rc == 0 && data) memcpy(data, rx + 4, len);
    return rc;
}

int sx1262_write_buffer(uint8_t offset, const uint8_t *data, size_t len)
{
    if (sx1262_wait_busy(100) != 0) return -1;

    size_t total = 2 + len; /* cmd + offset + data */
    uint8_t tx[total];
    tx[0] = SX1262_CMD_WRITE_BUFFER;
    tx[1] = offset;
    if (len && data) memcpy(tx + 2, data, len);

    nss_low();
    int rc = spi_transfer(tx, NULL, total);
    nss_high();
    return rc;
}

int sx1262_read_buffer(uint8_t offset, uint8_t *data, size_t len)
{
    if (sx1262_wait_busy(100) != 0) return -1;

    /* cmd + offset + 1 NOP + len data */
    size_t total = 3 + len;
    uint8_t tx[total];
    uint8_t rx[total];
    memset(tx, 0x00, total);
    tx[0] = SX1262_CMD_READ_BUFFER;
    tx[1] = offset;

    nss_low();
    int rc = spi_transfer(tx, rx, total);
    nss_high();

    if (rc == 0 && data) memcpy(data, rx + 3, len);
    return rc;
}

/* ------------------------------------------------------------------ */
/*  Status                                                             */
/* ------------------------------------------------------------------ */

int sx1262_get_status(uint8_t *status)
{
    uint8_t st;
    int rc = sx1262_read_command(SX1262_CMD_GET_STATUS, &st, 0);
    /* Status is actually in the first response byte (index 1) — re-read */
    if (sx1262_wait_busy(100) != 0) return -1;

    uint8_t tx[2] = { SX1262_CMD_GET_STATUS, 0x00 };
    uint8_t rx[2] = { 0 };
    nss_low();
    rc = spi_transfer(tx, rx, 2);
    nss_high();
    if (rc == 0 && status) *status = rx[1];
    return rc;
}

/* ------------------------------------------------------------------ */
/*  Reset                                                              */
/* ------------------------------------------------------------------ */

int sx1262_reset(void)
{
    gpio_set_level(SX1262_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(1));
    gpio_set_level(SX1262_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    return sx1262_wait_busy(100);
}

/* ------------------------------------------------------------------ */
/*  Configuration commands                                             */
/* ------------------------------------------------------------------ */

int sx1262_set_standby(uint8_t mode)
{
    uint8_t data = mode; /* 0 = STDBY_RC, 1 = STDBY_XOSC */
    return sx1262_write_command(SX1262_CMD_SET_STANDBY, &data, 1);
}

int sx1262_set_packet_type(uint8_t type)
{
    return sx1262_write_command(SX1262_CMD_SET_PKT_TYPE, &type, 1);
}

int sx1262_set_rf_frequency(float freq_mhz)
{
    uint32_t freq_raw = (uint32_t)((double)freq_mhz * 1e6 / (32e6 / (1 << 25)));
    uint8_t data[4] = {
        (uint8_t)(freq_raw >> 24),
        (uint8_t)(freq_raw >> 16),
        (uint8_t)(freq_raw >> 8),
        (uint8_t)(freq_raw),
    };
    ESP_LOGD(TAG, "SetRfFrequency: %.1f MHz -> raw 0x%08" PRIX32, freq_mhz, freq_raw);
    return sx1262_write_command(SX1262_CMD_SET_RF_FREQ, data, 4);
}

int sx1262_set_pa_config(int8_t power_dbm)
{
    /* SX1262: paDutyCycle=0x04, hpMax=0x07, deviceSel=0x00, paLut=0x01 */
    uint8_t data[4] = { 0x04, 0x07, 0x00, 0x01 };
    int rc = sx1262_write_command(SX1262_CMD_SET_PA_CONFIG, data, 4);
    if (rc != 0) return rc;

    /* Set OCP to 140 mA (register 0x08E7 = 0x38) */
    uint8_t ocp = 0x38;
    return sx1262_write_register(0x08E7, &ocp, 1);
}

int sx1262_set_tx_params(int8_t power_dbm, uint8_t ramp_time)
{
    uint8_t data[2] = { (uint8_t)power_dbm, ramp_time };
    return sx1262_write_command(SX1262_CMD_SET_TX_PARAMS, data, 2);
}

int sx1262_set_modulation_params(uint8_t sf, uint8_t bw, uint8_t cr, uint8_t ldro)
{
    /* BW encoding: 125kHz=0x04, 250kHz=0x05, 500kHz=0x06 */
    uint8_t bw_param;
    switch (bw) {
        case 125: bw_param = 0x04; break;
        case 250: bw_param = 0x05; break;
        case 500: bw_param = 0x06; break;
        default:  bw_param = 0x04; break; /* default 125kHz */
    }

    /* Auto-calculate LDRO if caller passed 0xFF */
    if (ldro == 0xFF) {
        /* Symbol time = 2^SF / BW.  Enable LDRO if > 16 ms */
        double sym_time_ms = (double)(1u << sf) / ((double)bw * 1000.0) * 1000.0;
        ldro = (sym_time_ms > 16.0) ? 1 : 0;
    }

    uint8_t data[4] = { sf, bw_param, cr, ldro };
    return sx1262_write_command(SX1262_CMD_SET_MOD_PARAMS, data, 4);
}

int sx1262_set_packet_params(uint16_t preamble, uint8_t header_type,
                             uint8_t payload_len, uint8_t crc_on, uint8_t invert_iq)
{
    uint8_t data[6] = {
        (uint8_t)(preamble >> 8),
        (uint8_t)(preamble & 0xFF),
        header_type,      /* 0 = explicit, 1 = implicit */
        payload_len,
        crc_on,           /* 1 = on, 0 = off */
        invert_iq,        /* 1 = inverted */
    };
    return sx1262_write_command(SX1262_CMD_SET_PKT_PARAMS, data, 6);
}

int sx1262_set_buffer_base_address(uint8_t tx_base, uint8_t rx_base)
{
    uint8_t data[2] = { tx_base, rx_base };
    return sx1262_write_command(SX1262_CMD_SET_BUFF_BASE_ADDR, data, 2);
}

int sx1262_set_dio_irq_params(uint16_t irq_mask, uint16_t dio1_mask,
                              uint16_t dio2_mask, uint16_t dio3_mask)
{
    uint8_t data[8] = {
        (uint8_t)(irq_mask >> 8),  (uint8_t)(irq_mask & 0xFF),
        (uint8_t)(dio1_mask >> 8), (uint8_t)(dio1_mask & 0xFF),
        (uint8_t)(dio2_mask >> 8), (uint8_t)(dio2_mask & 0xFF),
        (uint8_t)(dio3_mask >> 8), (uint8_t)(dio3_mask & 0xFF),
    };
    return sx1262_write_command(SX1262_CMD_SET_DIO_IRQ_PARAMS, data, 8);
}

int sx1262_clear_irq_status(uint16_t mask)
{
    uint8_t data[2] = { (uint8_t)(mask >> 8), (uint8_t)(mask & 0xFF) };
    return sx1262_write_command(SX1262_CMD_CLR_IRQ_STATUS, data, 2);
}

uint16_t sx1262_get_irq_status(void)
{
    uint8_t data[2] = { 0 };
    if (sx1262_read_command(SX1262_CMD_GET_IRQ_STATUS, data, 2) != 0) {
        return 0;
    }
    return ((uint16_t)data[0] << 8) | data[1];
}

/* ------------------------------------------------------------------ */
/*  TX / RX                                                            */
/* ------------------------------------------------------------------ */

int sx1262_set_tx(uint32_t timeout_ms)
{
    /* Timeout in units of 15.625 µs (1/64 ms).  0 = no timeout. */
    uint32_t raw = (timeout_ms == 0) ? 0 : (uint32_t)((double)timeout_ms / 0.015625);
    uint8_t data[3] = {
        (uint8_t)(raw >> 16),
        (uint8_t)(raw >> 8),
        (uint8_t)(raw),
    };
    return sx1262_write_command(SX1262_CMD_SET_TX, data, 3);
}

int sx1262_set_rx(uint32_t timeout_ms)
{
    /* 0 = continuous, 0xFFFFFF = single */
    uint32_t raw;
    if (timeout_ms == 0) {
        raw = 0xFFFFFF; /* continuous RX */
    } else {
        raw = (uint32_t)((double)timeout_ms / 0.015625);
        if (raw > 0xFFFFFE) raw = 0xFFFFFE;
    }
    uint8_t data[3] = {
        (uint8_t)(raw >> 16),
        (uint8_t)(raw >> 8),
        (uint8_t)(raw),
    };
    return sx1262_write_command(SX1262_CMD_SET_RX, data, 3);
}

int sx1262_set_cad(void)
{
    return sx1262_write_command(SX1262_CMD_SET_CAD, NULL, 0);
}

int sx1262_get_rx_buffer_status(uint8_t *payload_len, uint8_t *rx_start_offset)
{
    uint8_t data[2] = { 0 };
    int rc = sx1262_read_command(SX1262_CMD_GET_RX_BUFF_STATUS, data, 2);
    if (rc == 0) {
        if (payload_len)      *payload_len = data[0];
        if (rx_start_offset)  *rx_start_offset = data[1];
    }
    return rc;
}

int sx1262_get_packet_status(int16_t *rssi, int8_t *snr)
{
    uint8_t data[3] = { 0 };
    int rc = sx1262_read_command(SX1262_CMD_GET_PKT_STATUS, data, 3);
    if (rc == 0) {
        if (rssi) *rssi = -(int16_t)data[0] / 2;
        if (snr)  *snr  = (int8_t)data[1] / 4;
    }
    return rc;
}

int sx1262_set_sleep(uint8_t config)
{
    /* No BUSY wait before sleep command */
    uint8_t data = config;
    nss_low();
    uint8_t tx[2] = { SX1262_CMD_SET_SLEEP, data };
    int rc = spi_transfer(tx, NULL, 2);
    nss_high();
    return rc;
}

/* ------------------------------------------------------------------ */
/*  Heltec V3 specific                                                 */
/* ------------------------------------------------------------------ */

int sx1262_set_dio3_as_tcxo(float voltage, uint32_t timeout_ms)
{
    /* Voltage encoding: 1.6V=0x00, 1.7V=0x01, 1.8V=0x02, 2.2V=0x03,
       2.4V=0x04, 2.7V=0x05, 3.0V=0x06, 3.3V=0x07 */
    uint8_t volt_code;
    if (voltage <= 1.6f)      volt_code = 0x00;
    else if (voltage <= 1.7f) volt_code = 0x01;
    else if (voltage <= 1.8f) volt_code = 0x02;
    else if (voltage <= 2.2f) volt_code = 0x03;
    else if (voltage <= 2.4f) volt_code = 0x04;
    else if (voltage <= 2.7f) volt_code = 0x05;
    else if (voltage <= 3.0f) volt_code = 0x06;
    else                      volt_code = 0x07;

    /* Timeout in units of 15.625 µs */
    uint32_t raw = (uint32_t)((double)timeout_ms * 1000.0 / 15.625);
    uint8_t data[4] = {
        volt_code,
        (uint8_t)(raw >> 16),
        (uint8_t)(raw >> 8),
        (uint8_t)(raw),
    };
    ESP_LOGD(TAG, "SetDIO3 TCXO: %.1fV (code 0x%02x), timeout %" PRIu32 " ms",
             voltage, volt_code, timeout_ms);
    return sx1262_write_command(SX1262_CMD_SET_DIO3_AS_TCXO, data, 4);
}

int sx1262_calibrate(uint8_t cal_mask)
{
    return sx1262_write_command(SX1262_CMD_CALIBRATE, &cal_mask, 1);
}

int sx1262_calibrate_image(float freq_mhz)
{
    uint8_t data[2];
    if (freq_mhz >= 902.0f && freq_mhz <= 928.0f) {
        data[0] = 0xE1; data[1] = 0xE9; /* 902-928 MHz */
    } else if (freq_mhz >= 863.0f && freq_mhz <= 870.0f) {
        data[0] = 0xD7; data[1] = 0xDB; /* 863-870 MHz */
    } else if (freq_mhz >= 779.0f && freq_mhz <= 787.0f) {
        data[0] = 0xC1; data[1] = 0xC5;
    } else if (freq_mhz >= 470.0f && freq_mhz <= 510.0f) {
        data[0] = 0x75; data[1] = 0x81;
    } else if (freq_mhz >= 430.0f && freq_mhz <= 440.0f) {
        data[0] = 0x6B; data[1] = 0x6F;
    } else {
        data[0] = 0xE1; data[1] = 0xE9; /* default 915 */
    }
    return sx1262_write_command(SX1262_CMD_CALIBRATE_IMAGE, data, 2);
}

int sx1262_set_regulator_mode(uint8_t mode)
{
    return sx1262_write_command(SX1262_CMD_SET_REGULATOR_MODE, &mode, 1);
}

/* ------------------------------------------------------------------ */
/*  Init                                                               */
/* ------------------------------------------------------------------ */

int sx1262_init(void)
{
    ESP_LOGD(TAG, "Initializing SX1262");

    /* --- GPIO --- */
    gpio_config_t out_conf = {
        .pin_bit_mask = (1ULL << SX1262_PIN_NSS) | (1ULL << SX1262_PIN_RST),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&out_conf);
    gpio_set_level(SX1262_PIN_NSS, 1);
    gpio_set_level(SX1262_PIN_RST, 1);

    gpio_config_t in_conf = {
        .pin_bit_mask = (1ULL << SX1262_PIN_BUSY) | (1ULL << SX1262_PIN_DIO1),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&in_conf);

    /* --- SPI bus --- */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num   = SX1262_PIN_MOSI,
        .miso_io_num   = SX1262_PIN_MISO,
        .sclk_io_num   = SX1262_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 256,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
        return -1;
    }

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = 8 * 1000 * 1000, /* 8 MHz */
        .mode           = 0,                /* CPOL=0, CPHA=0 */
        .spics_io_num   = -1,              /* manual CS */
        .queue_size     = 1,
        .pre_cb         = NULL,
        .post_cb        = NULL,
    };
    err = spi_bus_add_device(SPI2_HOST, &dev_cfg, &s_spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI add device failed: %s", esp_err_to_name(err));
        return -1;
    }

    /* --- Reset chip --- */
    if (sx1262_reset() != 0) return -1;

    /* --- Standby STDBY_RC --- */
    if (sx1262_set_standby(0) != 0) return -1;

    /* --- Heltec V3: DIO3 as TCXO (1.7V, 5ms timeout) --- */
    if (sx1262_set_dio3_as_tcxo(1.7f, 5) != 0) return -1;

    /* --- Calibrate all blocks --- */
    if (sx1262_calibrate(0x7F) != 0) return -1;
    vTaskDelay(pdMS_TO_TICKS(5)); /* wait for calibration */

    /* --- Calibrate image for 915 MHz --- */
    if (sx1262_calibrate_image(915.0f) != 0) return -1;

    /* --- DC-DC regulator (Heltec V3 uses DC-DC) --- */
    if (sx1262_set_regulator_mode(1) != 0) return -1;

    /* --- LoRa packet type --- */
    if (sx1262_set_packet_type(1) != 0) return -1;

    /* --- Buffer base addresses: TX=0, RX=128 --- */
    if (sx1262_set_buffer_base_address(0, 128) != 0) return -1;

    ESP_LOGD(TAG, "SX1262 initialized OK");
    return 0;
}

#endif /* ESP_PLATFORM */
