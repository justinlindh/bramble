/*
 * ESP-IDF radio implementation using the SX1262 driver.
 * Implements the radio.h interface for Heltec WiFi LoRa 32 V3.
 */

#ifdef ESP_PLATFORM

#include "radio.h"
#include "radio_internal.h"
#include "sx1262.h"
#include "board_config.h"

#include <string.h>
#include <inttypes.h>
#include <stdatomic.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char* TAG = "radio_esp";

/* ------------------------------------------------------------------ */
/*  State                                                              */
/* ------------------------------------------------------------------ */

static atomic_int s_state = RADIO_STATE_IDLE;
static radio_config_t s_config;
static radio_rx_callback_t s_rx_cb;
static radio_tx_done_callback_t s_tx_done_cb;
static radio_cad_done_callback_t s_cad_done_cb;

static TaskHandle_t s_radio_task;
static TaskHandle_t s_tx_waiter; /* task waiting for TX done */

static volatile bool s_cad_result;
static SemaphoreHandle_t s_cad_sem;

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/* STDBY_RC (mode 0): the SX1262 auto-enables TCXO via DIO3 when entering
 * TX or RX, so we don't need to stay in STDBY_XOSC between commands. */
static inline int radio_standby(void) { return sx1262_set_standby(0); }

/* Convert bw_hz (e.g. 125000) to bw code for sx1262_set_modulation_params */
static uint8_t bw_hz_to_code(uint32_t bw_hz) {
    if (bw_hz <= 125000)
        return 125;
    else if (bw_hz <= 250000)
        return 250;
    else
        return 500;
}

static void set_sync_word(uint8_t sw) {
    /* LoRa sync word register 0x0740-0x0741.
       Public (0x34):  0x3444.  Private (0x12): 0x1424 */
    uint8_t regs[2];
    if (sw == 0x34) {
        regs[0] = 0x34;
        regs[1] = 0x44;
    } else {
        /* 0x12 or anything else → private */
        regs[0] = 0x14;
        regs[1] = 0x24;
    }
    sx1262_write_register(0x0740, regs, 2);
}

static int configure_radio(const radio_config_t* cfg) {
    int rc;

    rc = sx1262_set_rf_frequency(cfg->frequency_mhz);
    if (rc != 0)
        return rc;

    rc = sx1262_set_pa_config(cfg->tx_power);
    if (rc != 0)
        return rc;

    /* Ramp time 0x04 = 200 µs */
    rc = sx1262_set_tx_params(cfg->tx_power, 0x04);
    if (rc != 0)
        return rc;

    rc = sx1262_set_modulation_params(cfg->sf, bw_hz_to_code(cfg->bw_hz), cfg->coding_rate,
                                      0xFF /* auto LDRO */);
    if (rc != 0)
        return rc;

    rc = sx1262_set_packet_params(cfg->preamble, cfg->explicit_header ? 0 : 1,
                                  255, /* max payload for RX */
                                  cfg->crc ? 1 : 0, 0 /* normal IQ */);
    if (rc != 0)
        return rc;

    set_sync_word(cfg->sync_word);

    /* Route TxDone, RxDone, CRC error, Timeout, CAD done/detected to DIO1 */
    uint16_t irq_mask = SX1262_IRQ_TX_DONE | SX1262_IRQ_RX_DONE | SX1262_IRQ_CRC_ERR |
                        SX1262_IRQ_TIMEOUT | SX1262_IRQ_CAD_DONE | SX1262_IRQ_CAD_DETECTED;
    rc = sx1262_set_dio_irq_params(irq_mask, irq_mask, 0x0000, 0x0000);
    if (rc != 0)
        return rc;

    rc = sx1262_set_cad_params(BRAMBLE_CAD_SYMBOL_NUM_REG, 22, 10, 0x00, 0);
    return rc;
}

static void cad_check_cb(bool detected) {
    s_cad_result = detected;
    if (s_cad_sem) {
        xSemaphoreGive(s_cad_sem);
    }
}

/* ------------------------------------------------------------------ */
/*  DIO1 ISR → radio task                                              */
/* ------------------------------------------------------------------ */

static void IRAM_ATTR dio1_isr_handler(void* arg) {
    (void)arg;
    BaseType_t woken = pdFALSE;
    if (s_radio_task) {
        vTaskNotifyGiveFromISR(s_radio_task, &woken);
    }
    if (woken)
        portYIELD_FROM_ISR();
}

static void radio_task(void* arg) {
    (void)arg;
    uint8_t buf[256];

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        uint16_t irq = sx1262_get_irq_status();
        sx1262_clear_irq_status(0x03FF); /* clear all */

        ESP_LOGD(TAG, "IRQ: 0x%04x", irq);

        if (irq & SX1262_IRQ_TX_DONE) {
            ESP_LOGD(TAG, "TX done");
            atomic_store(&s_state, RADIO_STATE_IDLE);
            /* Wake the TX waiter */
            if (s_tx_waiter) {
                xTaskNotifyGive(s_tx_waiter);
            }
        }

        if (irq & SX1262_IRQ_RX_DONE) {
            if (irq & SX1262_IRQ_CRC_ERR) {
                ESP_LOGD(TAG, "RX CRC error, discarding");
            } else {
                uint8_t len = 0, offset = 0;
                sx1262_get_rx_buffer_status(&len, &offset);
                if (len > 0 && len <= sizeof(buf)) {
                    sx1262_read_buffer(offset, buf, len);

                    radio_rx_info_t info = {0};
                    info.len = len;
                    sx1262_get_packet_status(&info.rssi, &info.snr);

                    ESP_LOGD(TAG, "RX: %u bytes, RSSI %d, SNR %d", len, info.rssi, info.snr);

                    if (s_rx_cb) {
                        s_rx_cb(buf, len, &info);
                    }
                }
            }
        }

        if (irq & SX1262_IRQ_CAD_DONE) {
            bool detected = (irq & SX1262_IRQ_CAD_DETECTED) != 0;
            ESP_LOGD(TAG, "CAD done, detected=%d", detected);
            atomic_store(&s_state, RADIO_STATE_IDLE);
            if (s_cad_done_cb) {
                s_cad_done_cb(detected);
            }
        }

        if (irq & SX1262_IRQ_TIMEOUT) {
            ESP_LOGD(TAG, "Timeout");
            atomic_store(&s_state, RADIO_STATE_IDLE);
            /* If TX timed out, wake waiter */
            if (s_tx_waiter) {
                xTaskNotifyGive(s_tx_waiter);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Profile defaults                                                   */
/* ------------------------------------------------------------------ */

int radio_reconfigure(const radio_config_t* config) {
    ESP_LOGI(TAG, "Reconfiguring radio: %.1f MHz SF%u BW%" PRIu32 " TX %ddBm",
             config->frequency_mhz, config->sf, config->bw_hz, config->tx_power);

    /* Put radio in standby before reconfiguring (0 = RC oscillator) */
    radio_standby();
    vTaskDelay(pdMS_TO_TICKS(10));

    memcpy(&s_config, config, sizeof(s_config));

    int rc = configure_radio(config);
    if (rc != 0) {
        ESP_LOGE(TAG, "configure_radio failed during reconfigure");
        return rc;
    }

    /* Resume RX */
    radio_start_rx();
    ESP_LOGI(TAG, "Radio reconfigured successfully");
    return 0;
}

void radio_get_config(radio_config_t* config) { memcpy(config, &s_config, sizeof(*config)); }

/* radio_get_profile_config lives in radio_profiles.c (shared by the SX1262
 * driver, the virtual emulator driver, and host tests). */

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

int radio_init(const radio_config_t* config) {
    ESP_LOGD(TAG, "radio_init: %.1f MHz, SF%u, BW %" PRIu32, config->frequency_mhz, config->sf,
             config->bw_hz);

    memcpy(&s_config, config, sizeof(s_config));

    /* Init SX1262 hardware */
    int rc = sx1262_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "sx1262_init failed");
        return -1;
    }

    /* Some radio modules (e.g. NiceRF LoRa1262) route their RF switch off DIO2
     * rather than exposing a separate control pin; the SX1262 must be told to
     * drive DIO2 automatically during TX/RX or transmission is dead. */
    if (board_get_config()->radio_dio2_rf_switch) {
        rc = sx1262_set_dio2_as_rf_switch(true);
        if (rc != 0) {
            ESP_LOGE(TAG, "sx1262_set_dio2_as_rf_switch failed");
            return -1;
        }
        ESP_LOGI(TAG, "DIO2 configured as RF switch control");
    }

    /* Calibrate image for the configured frequency band */
    rc = sx1262_calibrate_image(config->frequency_mhz);
    if (rc != 0) {
        ESP_LOGE(TAG, "calibrate_image failed");
        return -1;
    }

    /* Configure radio parameters */
    rc = configure_radio(config);
    if (rc != 0) {
        ESP_LOGE(TAG, "configure_radio failed");
        return -1;
    }

    /* Create radio task */
    BaseType_t ret = xTaskCreate(radio_task, "radio", 4096, NULL, 5, &s_radio_task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create radio task");
        return -1;
    }

    /* Install DIO1 ISR */
    const bramble_board_config_t* board = board_get_config();
    gpio_set_intr_type(board->radio.dio1, GPIO_INTR_POSEDGE);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(board->radio.dio1, dio1_isr_handler, NULL);

    /* Start continuous RX */
    radio_start_rx();

    ESP_LOGD(TAG, "radio_init complete");
    return 0;
}

int radio_transmit_raw(const uint8_t* data, uint8_t len) {
    ESP_LOGD(TAG, "radio_transmit_raw: %u bytes", len);

    atomic_store(&s_state, RADIO_STATE_TX);
    s_tx_waiter = xTaskGetCurrentTaskHandle();

    /* Switch to standby */
    radio_standby();

    /* Write payload to buffer */
    sx1262_write_buffer(0, data, len);

    /* Update packet params with actual payload length */
    sx1262_set_packet_params(s_config.preamble, s_config.explicit_header ? 0 : 1, len,
                             s_config.crc ? 1 : 0, 0);

    /* Clear IRQ and start TX with 3s hardware timeout */
    sx1262_clear_irq_status(0x03FF);
    int tx_err = sx1262_set_tx(3000);
    if (tx_err != 0) {
        ESP_LOGE(TAG, "sx1262_set_tx failed (BUSY timeout?), aborting TX");
        s_tx_waiter = NULL;
        atomic_store(&s_state, RADIO_STATE_IDLE);
        radio_start_rx();
        return -1;
    }
    ESP_LOGI(TAG, "TX started: %u bytes, DIO1_GPIO=%d, BUSY_GPIO=%d", len,
             gpio_get_level(board_get_config()->radio.dio1),
             gpio_get_level(board_get_config()->radio.busy));

    /* Reset WDT before the blocking wait — TX can take up to 4s and the
     * caller may have consumed most of the 5s WDT window already. */
    esp_task_wdt_reset();

    /* Wait for TX done (or TX timeout) notification — max 4s FreeRTOS timeout */
    uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(4000));
    s_tx_waiter = NULL;

    if (notified == 0) {
        /* Diagnostic: read back what the radio chip thinks happened */
        uint16_t irq_status = sx1262_get_irq_status();
        int dio1_level = gpio_get_level(board_get_config()->radio.dio1);
        ESP_LOGE(TAG, "TX timeout: DIO1=%d IRQ_reg=0x%04x (TxDone=%d Timeout=%d)", dio1_level,
                 irq_status, (irq_status & SX1262_IRQ_TX_DONE) ? 1 : 0,
                 (irq_status & SX1262_IRQ_TIMEOUT) ? 1 : 0);
        atomic_store(&s_state, RADIO_STATE_IDLE);
        radio_standby();
        radio_start_rx();
        return -1;
    }

    /* Call TX done callback */
    if (s_tx_done_cb) {
        s_tx_done_cb();
    }

    /* Return to RX */
    radio_start_rx();
    return 0;
}

void radio_start_rx(void) {
    radio_standby();
    sx1262_clear_irq_status(0x03FF);
    sx1262_set_rx(0); /* continuous */
    atomic_store(&s_state, RADIO_STATE_RX);
}

void radio_cad(void) {
    radio_standby();
    sx1262_clear_irq_status(0x03FF);
    atomic_store(&s_state, RADIO_STATE_CAD);
    sx1262_set_cad();
}

bool radio_cad_check(void) {
    if (!s_cad_sem) {
        s_cad_sem = xSemaphoreCreateBinary();
        if (!s_cad_sem) {
            return false;
        }
    }

    radio_cad_done_callback_t prev_cb = s_cad_done_cb;
    s_cad_done_cb = cad_check_cb;
    s_cad_result = false;

    radio_standby();
    sx1262_clear_irq_status(0x03FF);
    radio_cad();

    /* Scale the wait with the live radio config. A fixed 50 ms could not
     * cover a 4-symbol CAD above SF10 at 125 kHz (SF12 alone is 131 ms), so
     * the take always expired and listen-before-talk silently degraded to
     * nothing on long-range profiles. */
    uint32_t timeout_ms =
        bramble_cad_timeout_ms(s_config.sf, s_config.bw_hz, BRAMBLE_CAD_SYMBOL_NUM_REG);
    bool got_result = xSemaphoreTake(s_cad_sem, pdMS_TO_TICKS(timeout_ms));

    s_cad_done_cb = prev_cb;
    radio_start_rx();

    if (!got_result) {
        ESP_LOGW(TAG, "CAD check timed out after %u ms (sf=%u bw=%u)", (unsigned)timeout_ms,
                 (unsigned)s_config.sf, (unsigned)s_config.bw_hz);
        return false;
    }

    return s_cad_result;
}

void radio_set_tx_power(int8_t power) {
    s_config.tx_power = power;
    sx1262_set_pa_config(power);
    sx1262_set_tx_params(power, 0x04);
}

radio_state_t radio_get_state(void) { return (radio_state_t)atomic_load(&s_state); }

void radio_set_rx_callback(radio_rx_callback_t cb) { s_rx_cb = cb; }

void radio_set_tx_done_callback(radio_tx_done_callback_t cb) { s_tx_done_cb = cb; }

void radio_set_cad_done_callback(radio_cad_done_callback_t cb) { s_cad_done_cb = cb; }

void radio_sleep(void) {
    radio_standby();
    sx1262_set_sleep(0x04); /* warm start (retain config) */
    atomic_store(&s_state, RADIO_STATE_SLEEP);
}

bool radio_check_and_clear_reinit(void) {
    if (!sx1262_needs_reinit())
        return false;
    sx1262_clear_reinit();
    ESP_LOGW(TAG, "Radio reinit after hard reset — reconfiguring");
    int rc = radio_reconfigure(&s_config);
    if (rc != 0) {
        ESP_LOGE(TAG, "Radio reconfigure failed after hard reset: %d", rc);
    }
    return true;
}

#endif /* ESP_PLATFORM */
