/*
 * LR1110 radio backend for the nRF52840 target (Wio-WM1110 / T1000-E),
 * implementing the radio.h interface over Semtech's SWDR001 driver.
 *
 * This file deliberately mirrors components/radio/radio_esp.c function for
 * function: the same state atomics, the same ISR-to-task-notification
 * dispatch, the same atomic TX-waiter handoff, the same RX-sequence lock
 * (issue #225), the same CAD fail-open/closed policy (#118), and the same
 * driver-owned reinit contract. Where behavior differs it is because the
 * LR1110 differs (RF switch table instead of DIO2, TCXO command instead of
 * DIO3 register, CAD_DETECTED as a separate IRQ bit).
 *
 * Board facts (Seeed vendor SDK + Meshtastic, researched 2026-07-27):
 * regulator DC-DC; RF switch enable=DIO5|DIO6, rx=DIO5, tx=DIO5|DIO6,
 * tx_hp=DIO6; TCXO 1.6V with a 5ms (164 tick) startup, the Meshtastic
 * field-proven value (the vendor SDK says 3.0V; if calibration errors show
 * up on the bench, that is the knob).
 */

#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>

#include <FreeRTOS.h>
#include <semphr.h>
#include <task.h>

#include <hal/nrf_gpio.h>
#include <nrfx_gpiote.h>

#include <lr11xx_radio.h>
#include <lr11xx_regmem.h>
#include <lr11xx_system.h>

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "lr11xx_hal_nrf.h"
#include "radio.h"
#include "radio_internal.h"
#include "tx_gate.h"
#include "wio_wm1110_devkit.h"

static const char* TAG = "radio_lr1110";

/* ------------------------------------------------------------------ */
/*  State (mirrors radio_esp.c)                                        */
/* ------------------------------------------------------------------ */

static const void* s_lr; /* SWDR001 context from lr11xx_hal_nrf_init() */

static atomic_int s_state = RADIO_STATE_IDLE;
static radio_config_t s_config;
static radio_rx_callback_t s_rx_cb;
static radio_tx_done_callback_t s_tx_done_cb;
static radio_cad_done_callback_t s_cad_done_cb;

static TaskHandle_t s_radio_task;

/* See radio_esp.c: both sides claim the waiter with an exchange so exactly
 * one wins and stale notifications can always be drained. */
static _Atomic(TaskHandle_t) s_tx_waiter;

static volatile bool s_cad_result;
static SemaphoreHandle_t s_cad_sem;

/* RX-done read sequence vs reconfigure; deliberately NOT the tx_gate lock
 * (deadlock rationale in radio_esp.c, issue #225). */
static SemaphoreHandle_t s_rx_seq_mutex;

static void rx_seq_lock(void) {
    if (s_rx_seq_mutex)
        xSemaphoreTake(s_rx_seq_mutex, portMAX_DELAY);
}

static void rx_seq_unlock(void) {
    if (s_rx_seq_mutex)
        xSemaphoreGive(s_rx_seq_mutex);
}

static cad_timeout_policy_t s_cad_timeout_policy;

/* ------------------------------------------------------------------ */
/*  LR1110 parameter mapping                                           */
/* ------------------------------------------------------------------ */

static lr11xx_radio_lora_bw_t bw_from_hz(uint32_t bw_hz) {
    switch (bw_hz) {
    case 62500:
        return LR11XX_RADIO_LORA_BW_62;
    case 250000:
        return LR11XX_RADIO_LORA_BW_250;
    case 500000:
        return LR11XX_RADIO_LORA_BW_500;
    case 125000:
    default:
        return LR11XX_RADIO_LORA_BW_125;
    }
}

/* LDRO on when the symbol time reaches 16.38ms (SF11+ at 125kHz), the same
 * threshold the SX1262's auto mode applies. */
static uint8_t ldro_for(uint8_t sf, uint32_t bw_hz) {
    return bramble_symbol_time_us(sf, bw_hz) >= 16380 ? 1 : 0;
}

/* CAD detection peak per SF, from Semtech's SWSD003 measured table
 * (2-symbol CAD; SF5..SF12). det_min stays at the recommended 10. */
static uint8_t cad_det_peak_for(uint8_t sf) {
    static const uint8_t peaks[] = {48, 48, 50, 55, 55, 59, 61, 65};
    if (sf < 5 || sf > 12) {
        return 0x32; /* driver default */
    }
    return peaks[sf - 5];
}

/* PA selection per the Seeed shield table: low-power PA/VREG up to +14dBm,
 * high-power PA/VBAT above, one authoritative copy for boot config and
 * runtime power changes alike (the SX1262 backend has the same shape in
 * sx1262_set_pa_config). */
static int set_pa(int8_t power) {
    lr11xx_radio_pa_cfg_t pa;
    int8_t chip_power;
    if (power <= 14) {
        pa = (lr11xx_radio_pa_cfg_t){
            .pa_sel = LR11XX_RADIO_PA_SEL_LP,
            .pa_reg_supply = LR11XX_RADIO_PA_REG_SUPPLY_VREG,
            .pa_duty_cycle = 0x04,
            .pa_hp_sel = 0x00,
        };
        chip_power = power;
    } else {
        pa = (lr11xx_radio_pa_cfg_t){
            .pa_sel = LR11XX_RADIO_PA_SEL_HP,
            .pa_reg_supply = LR11XX_RADIO_PA_REG_SUPPLY_VBAT,
            .pa_duty_cycle = 0x04,
            .pa_hp_sel = 0x07,
        };
        chip_power = power > 22 ? 22 : power;
    }
    if (lr11xx_radio_set_pa_cfg(s_lr, &pa) != LR11XX_STATUS_OK)
        return -1;
    return lr11xx_radio_set_tx_params(s_lr, chip_power, LR11XX_RADIO_RAMP_48_US) == LR11XX_STATUS_OK
               ? 0
               : -1;
}

static int configure_radio(const radio_config_t* cfg) {
    if (lr11xx_radio_set_pkt_type(s_lr, LR11XX_RADIO_PKT_TYPE_LORA) != LR11XX_STATUS_OK)
        return -1;

    if (lr11xx_radio_set_rf_freq(s_lr, (uint32_t)(cfg->frequency_mhz * 1e6f)) != LR11XX_STATUS_OK)
        return -1;

    if (set_pa(cfg->tx_power) != 0)
        return -1;

    lr11xx_radio_mod_params_lora_t mod = {
        .sf = (lr11xx_radio_lora_sf_t)cfg->sf,
        .bw = bw_from_hz(cfg->bw_hz),
        .cr = (lr11xx_radio_lora_cr_t)cfg->coding_rate,
        .ldro = ldro_for(cfg->sf, cfg->bw_hz),
    };
    if (lr11xx_radio_set_lora_mod_params(s_lr, &mod) != LR11XX_STATUS_OK)
        return -1;

    lr11xx_radio_pkt_params_lora_t pkt = {
        .preamble_len_in_symb = cfg->preamble,
        .header_type =
            cfg->explicit_header ? LR11XX_RADIO_LORA_PKT_EXPLICIT : LR11XX_RADIO_LORA_PKT_IMPLICIT,
        .pld_len_in_bytes = 255, /* max payload for RX */
        .crc = cfg->crc ? LR11XX_RADIO_LORA_CRC_ON : LR11XX_RADIO_LORA_CRC_OFF,
        .iq = LR11XX_RADIO_LORA_IQ_STANDARD,
    };
    if (lr11xx_radio_set_lora_pkt_params(s_lr, &pkt) != LR11XX_STATUS_OK)
        return -1;

    /* The LR1110 takes the sync word byte directly (0x12 private / 0x34
     * public), no register poke needed. */
    if (lr11xx_radio_set_lora_sync_word(s_lr, cfg->sync_word) != LR11XX_STATUS_OK) {
        ESP_LOGE(TAG, "set_lora_sync_word failed");
        return -1;
    }

    uint32_t irq_mask = LR11XX_SYSTEM_IRQ_TX_DONE | LR11XX_SYSTEM_IRQ_RX_DONE |
                        LR11XX_SYSTEM_IRQ_CRC_ERROR | LR11XX_SYSTEM_IRQ_TIMEOUT |
                        LR11XX_SYSTEM_IRQ_CAD_DONE | LR11XX_SYSTEM_IRQ_CAD_DETECTED;
    if (lr11xx_system_set_dio_irq_params(s_lr, irq_mask, 0) != LR11XX_STATUS_OK)
        return -1;

    lr11xx_radio_cad_params_t cad = {
        .cad_symb_nb = BRAMBLE_CAD_SYMBOL_COUNT,
        .cad_detect_peak = cad_det_peak_for(cfg->sf),
        .cad_detect_min = 10,
        .cad_exit_mode = LR11XX_RADIO_CAD_EXIT_MODE_STANDBYRC,
        .cad_timeout = 0,
    };
    return lr11xx_radio_set_cad_params(s_lr, &cad) == LR11XX_STATUS_OK ? 0 : -1;
}

static inline int radio_standby(void) {
    return lr11xx_system_set_standby(s_lr, LR11XX_SYSTEM_STANDBY_CFG_RC) == LR11XX_STATUS_OK ? 0
                                                                                             : -1;
}

static inline int clear_all_irq(void) {
    return lr11xx_system_clear_irq_status(s_lr, LR11XX_SYSTEM_IRQ_ALL_MASK) == LR11XX_STATUS_OK
               ? 0
               : -1;
}

/* ------------------------------------------------------------------ */
/*  TX waiter handoff (verbatim protocol from radio_esp.c)             */
/* ------------------------------------------------------------------ */

static void wake_tx_waiter(void) {
    TaskHandle_t waiter = atomic_exchange(&s_tx_waiter, (TaskHandle_t)NULL);
    if (waiter) {
        xTaskNotifyGive(waiter);
    }
}

#define TX_DISARM_DRAIN_TICKS 2

static void tx_disarm(void) {
    TaskHandle_t prev = atomic_exchange(&s_tx_waiter, (TaskHandle_t)NULL);
    ulTaskNotifyTake(pdTRUE, prev == NULL ? TX_DISARM_DRAIN_TICKS : 0);
}

static void cad_check_cb(bool detected) {
    s_cad_result = detected;
    if (s_cad_sem) {
        xSemaphoreGive(s_cad_sem);
    }
}

/* ------------------------------------------------------------------ */
/*  IRQ line ISR -> radio task                                         */
/* ------------------------------------------------------------------ */

static void dio_isr_handler(nrfx_gpiote_pin_t pin, nrfx_gpiote_trigger_t trigger, void* p_context) {
    (void)pin;
    (void)trigger;
    (void)p_context;
    BaseType_t woken = pdFALSE;
    if (s_radio_task) {
        vTaskNotifyGiveFromISR(s_radio_task, &woken);
    }
    portYIELD_FROM_ISR(woken);
}

static void radio_handle_rx_done(uint8_t* buf, size_t buf_size) {
    rx_seq_lock();

    lr11xx_radio_rx_buffer_status_t st = {0};
    lr11xx_radio_get_rx_buffer_status(s_lr, &st);

    radio_rx_info_t info = {0};
    bool have_frame = false;
    if (st.pld_len_in_bytes > 0 && st.pld_len_in_bytes <= buf_size) {
        lr11xx_regmem_read_buffer8(s_lr, buf, st.buffer_start_pointer, st.pld_len_in_bytes);
        info.len = st.pld_len_in_bytes;
        lr11xx_radio_pkt_status_lora_t ps = {0};
        lr11xx_radio_get_lora_pkt_status(s_lr, &ps);
        info.rssi = ps.rssi_pkt_in_dbm;
        info.snr = ps.snr_pkt_in_db;
        have_frame = true;
    }

    rx_seq_unlock();

    if (have_frame && s_rx_cb) {
        s_rx_cb(buf, info.len, &info);
        static bool logged_hwm;
        if (!logged_hwm) {
            logged_hwm = true;
            ESP_LOGD(TAG, "radio task stack high-water: %u words free",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
    }
}

static void radio_task(void* arg) {
    (void)arg;
    uint8_t buf[256];

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        lr11xx_system_irq_mask_t irq = 0;
        if (lr11xx_system_get_and_clear_irq_status(s_lr, &irq) != LR11XX_STATUS_OK) {
            continue;
        }

        if (irq & LR11XX_SYSTEM_IRQ_TX_DONE) {
            atomic_store(&s_state, RADIO_STATE_IDLE);
            wake_tx_waiter();
        }

        if (irq & LR11XX_SYSTEM_IRQ_RX_DONE) {
            if (irq & LR11XX_SYSTEM_IRQ_CRC_ERROR) {
                ESP_LOGD(TAG, "RX CRC error, discarding");
            } else {
                radio_handle_rx_done(buf, sizeof(buf));
            }
        }

        if (irq & LR11XX_SYSTEM_IRQ_CAD_DONE) {
            bool detected = (irq & LR11XX_SYSTEM_IRQ_CAD_DETECTED) != 0;
            atomic_store(&s_state, RADIO_STATE_IDLE);
            if (s_cad_done_cb) {
                s_cad_done_cb(detected);
            }
        }

        if (irq & LR11XX_SYSTEM_IRQ_TIMEOUT) {
            atomic_store(&s_state, RADIO_STATE_IDLE);
            wake_tx_waiter();
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

int radio_reconfigure(const radio_config_t* config) {
    ESP_LOGI(TAG, "Reconfiguring radio: %u.%02u MHz SF%u BW%" PRIu32 " TX %ddBm",
             (unsigned)config->frequency_mhz,
             (unsigned)((uint32_t)(config->frequency_mhz * 100) % 100), config->sf, config->bw_hz,
             config->tx_power);

    tx_gate_radio_lock();
    rx_seq_lock();

    int standby_rc = radio_standby();
    if (standby_rc != 0) {
        ESP_LOGW(TAG, "standby before reconfigure failed, continuing");
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    memcpy(&s_config, config, sizeof(s_config));

    int rc = configure_radio(config);
    if (rc != 0) {
        ESP_LOGE(TAG, "configure_radio failed during reconfigure");
        rx_seq_unlock();
        tx_gate_radio_unlock();
        return rc;
    }

    radio_start_rx();
    rx_seq_unlock();
    tx_gate_radio_unlock();
    ESP_LOGI(TAG, "Radio reconfigured successfully");
    return 0;
}

void radio_get_config(radio_config_t* config) { memcpy(config, &s_config, sizeof(*config)); }

/* Full LR1110 system bring-up: the board-level sequence that has no SX1262
 * equivalent lives here rather than in configure_radio so reconfigure stays
 * cheap (the system settings survive a reconfigure; only a hard reset loses
 * them, and the reinit path goes through radio_init-level recovery via
 * radio_reconfigure after the HAL's hard reset, which re-runs this). */
static int lr1110_system_init(void) {
    lr11xx_hal_nrf_hard_reset();

    lr11xx_system_version_t ver = {0};
    if (lr11xx_system_get_version(s_lr, &ver) == LR11XX_STATUS_OK) {
        ESP_LOGI(TAG, "LR1110 hw 0x%02x type 0x%02x fw 0x%04x", ver.hw, ver.type, ver.fw);
    }

    if (lr11xx_system_set_reg_mode(s_lr, LR11XX_SYSTEM_REG_MODE_DCDC) != LR11XX_STATUS_OK)
        return -1;

    /* Wio-WM1110 RF switch table (Seeed vendor SDK, Meshtastic-confirmed). */
    lr11xx_system_rfswitch_cfg_t rfsw = {0};
    rfsw.enable = LR11XX_SYSTEM_RFSW0_HIGH | LR11XX_SYSTEM_RFSW1_HIGH;
    rfsw.standby = 0;
    rfsw.rx = LR11XX_SYSTEM_RFSW0_HIGH;
    rfsw.tx = LR11XX_SYSTEM_RFSW0_HIGH | LR11XX_SYSTEM_RFSW1_HIGH;
    rfsw.tx_hp = LR11XX_SYSTEM_RFSW1_HIGH;
    if (lr11xx_system_set_dio_as_rf_switch(s_lr, &rfsw) != LR11XX_STATUS_OK)
        return -1;

    /* TCXO 1.6V, 164 x 30.52us = 5ms startup. */
    if (lr11xx_system_set_tcxo_mode(s_lr, LR11XX_SYSTEM_TCXO_CTRL_1_6V, 164) != LR11XX_STATUS_OK)
        return -1;

    if (lr11xx_system_cfg_lfclk(s_lr, LR11XX_SYSTEM_LFCLK_XTAL, true) != LR11XX_STATUS_OK)
        return -1;

    lr11xx_system_clear_errors(s_lr);
    if (lr11xx_system_calibrate(s_lr, 0x3F) != LR11XX_STATUS_OK)
        return -1;

    lr11xx_system_errors_t errors = 0;
    lr11xx_system_get_errors(s_lr, &errors);
    if (errors != 0) {
        /* Calibration errors here are the TCXO-voltage tripwire named in the
         * file header. Surface loudly and fail init. */
        ESP_LOGE(TAG, "LR1110 system errors after calibrate: 0x%04x", (unsigned)errors);
        return -1;
    }
    lr11xx_system_clear_errors(s_lr);
    clear_all_irq();

    if (lr11xx_system_calibrate_image_in_mhz(s_lr, 902, 928) != LR11XX_STATUS_OK)
        return -1;

    return 0;
}

int radio_init(const radio_config_t* config) {
    memcpy(&s_config, config, sizeof(s_config));

    s_lr = lr11xx_hal_nrf_init();
    if (s_lr == NULL) {
        ESP_LOGE(TAG, "HAL init failed");
        return -1;
    }

    if (lr1110_system_init() != 0) {
        ESP_LOGE(TAG, "LR1110 system init failed");
        return -1;
    }

    int rc = configure_radio(config);
    if (rc != 0) {
        ESP_LOGE(TAG, "configure_radio failed");
        return -1;
    }

    s_rx_seq_mutex = xSemaphoreCreateMutex();
    if (!s_rx_seq_mutex) {
        return -1;
    }

    if (s_radio_task == NULL) {
        BaseType_t ret = xTaskCreate(radio_task, "radio", 1024, NULL, 5, &s_radio_task);
        if (ret != pdPASS) {
            ESP_LOGE(TAG, "Failed to create radio task");
            return -1;
        }

        /* IRQ line (LR1110 DIO9, board net "DIO1", P1.08), rising edge. IRQ
         * priority 6 stays below (numerically above) the FreeRTOS
         * max-syscall priority so the ISR may use the FromISR API. */
        static nrfx_gpiote_t gpiote = NRFX_GPIOTE_INSTANCE(0);
        if (!nrfx_gpiote_init_check(&gpiote)) {
            if (nrfx_gpiote_init(&gpiote, 6) != NRFX_SUCCESS) {
                ESP_LOGE(TAG, "GPIOTE init failed");
                return -1;
            }
        }
        static uint8_t ch;
        if (nrfx_gpiote_channel_alloc(&gpiote, &ch) != NRFX_SUCCESS) {
            ESP_LOGE(TAG, "GPIOTE channel alloc failed");
            return -1;
        }
        static const nrf_gpio_pin_pull_t pull = NRF_GPIO_PIN_NOPULL;
        nrfx_gpiote_trigger_config_t trig = {
            .trigger = NRFX_GPIOTE_TRIGGER_LOTOHI,
            .p_in_channel = &ch,
        };
        nrfx_gpiote_handler_config_t handler = {
            .handler = dio_isr_handler,
            .p_context = NULL,
        };
        nrfx_gpiote_input_pin_config_t in_cfg = {
            .p_pull_config = &pull,
            .p_trigger_config = &trig,
            .p_handler_config = &handler,
        };
        if (nrfx_gpiote_input_configure(&gpiote, BOARD_PIN_LORA_IRQ, &in_cfg) != NRFX_SUCCESS) {
            ESP_LOGE(TAG, "GPIOTE input configure failed");
            return -1;
        }
        nrfx_gpiote_trigger_enable(&gpiote, BOARD_PIN_LORA_IRQ, true);
    }

    radio_start_rx();
    if (radio_get_state() != RADIO_STATE_RX) {
        ESP_LOGE(TAG, "radio_init: could not enter RX, node is not receiving yet");
    }

    ESP_LOGI(TAG, "LR1110 radio up: %u.%02u MHz SF%u", (unsigned)config->frequency_mhz,
             (unsigned)((uint32_t)(config->frequency_mhz * 100) % 100), config->sf);
    return 0;
}

static int tx_abort(const char* what) {
    ESP_LOGE(TAG, "TX aborted: %s failed", what);
    tx_disarm();
    atomic_store(&s_state, RADIO_STATE_IDLE);
    if (lr11xx_hal_nrf_needs_reinit()) {
        ESP_LOGW(TAG, "LR1110 was hard reset, deferring recovery to radio reinit");
    } else {
        radio_start_rx();
    }
    return -1;
}

int radio_transmit_raw(const uint8_t* data, uint8_t len) {
    ulTaskNotifyTake(pdTRUE, 0);

    atomic_store(&s_state, RADIO_STATE_TX);
    atomic_store(&s_tx_waiter, xTaskGetCurrentTaskHandle());

    if (radio_standby() != 0) {
        return tx_abort("set_standby");
    }

    if (lr11xx_regmem_write_buffer8(s_lr, data, len) != LR11XX_STATUS_OK) {
        return tx_abort("write_buffer");
    }

    lr11xx_radio_pkt_params_lora_t pkt = {
        .preamble_len_in_symb = s_config.preamble,
        .header_type = s_config.explicit_header ? LR11XX_RADIO_LORA_PKT_EXPLICIT
                                                : LR11XX_RADIO_LORA_PKT_IMPLICIT,
        .pld_len_in_bytes = len,
        .crc = s_config.crc ? LR11XX_RADIO_LORA_CRC_ON : LR11XX_RADIO_LORA_CRC_OFF,
        .iq = LR11XX_RADIO_LORA_IQ_STANDARD,
    };
    if (lr11xx_radio_set_lora_pkt_params(s_lr, &pkt) != LR11XX_STATUS_OK) {
        return tx_abort("set_packet_params");
    }

    if (clear_all_irq() != 0) {
        return tx_abort("clear_irq_status");
    }
    if (lr11xx_radio_set_tx(s_lr, 3000) != LR11XX_STATUS_OK) {
        return tx_abort("set_tx");
    }
    ESP_LOGD(TAG, "TX started: %u bytes", len);

    esp_task_wdt_reset();

    uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(4000));

    if (notified == 0) {
        tx_disarm();
        ESP_LOGE(TAG, "TX timeout: IRQ pin=%d BUSY pin=%d",
                 (int)nrf_gpio_pin_read(BOARD_PIN_LORA_IRQ),
                 (int)nrf_gpio_pin_read(BOARD_PIN_LORA_BUSY));
        atomic_store(&s_state, RADIO_STATE_IDLE);
        radio_start_rx();
        return -1;
    }

    atomic_store(&s_tx_waiter, (TaskHandle_t)NULL);

    if (s_tx_done_cb) {
        s_tx_done_cb();
    }

    radio_start_rx();
    return 0;
}

void radio_start_rx(void) {
    int rc = radio_standby();
    if (rc == 0) {
        rc = clear_all_irq();
    }
    if (rc == 0) {
        /* 0xFFFFFF RTC steps = continuous RX per the LR1110 command set. */
        rc = lr11xx_radio_set_rx_with_timeout_in_rtc_step(s_lr, 0x00FFFFFF) == LR11XX_STATUS_OK
                 ? 0
                 : -1;
    }

    if (rc != 0) {
        if (lr11xx_hal_nrf_needs_reinit()) {
            ESP_LOGE(TAG, "start_rx: LR1110 was hard reset, awaiting radio reinit");
        } else {
            ESP_LOGE(TAG, "start_rx failed, radio is NOT receiving");
        }
        atomic_store(&s_state, RADIO_STATE_IDLE);
        return;
    }

    atomic_store(&s_state, RADIO_STATE_RX);
}

void radio_cad(void) {
    int rc = radio_standby();
    if (rc == 0) {
        rc = clear_all_irq();
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "radio_cad: standby/clear_irq failed");
        atomic_store(&s_state, RADIO_STATE_IDLE);
        return;
    }

    atomic_store(&s_state, RADIO_STATE_CAD);
    if (lr11xx_radio_set_cad(s_lr) != LR11XX_STATUS_OK) {
        ESP_LOGE(TAG, "radio_cad: set_cad failed");
        atomic_store(&s_state, RADIO_STATE_IDLE);
    }
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

    radio_cad();

    uint32_t timeout_ms =
        bramble_cad_timeout_ms(s_config.sf, s_config.bw_hz, BRAMBLE_CAD_SYMBOL_NUM_REG);
    bool got_result = xSemaphoreTake(s_cad_sem, pdMS_TO_TICKS(timeout_ms));

    s_cad_done_cb = prev_cb;
    radio_start_rx();

    if (!got_result) {
        cad_timeout_action_t action = cad_timeout_policy_on_timeout(&s_cad_timeout_policy);
        if (action == CAD_TIMEOUT_FAIL_CLOSED) {
            ESP_LOGE(TAG, "CAD timed out %u times running, failing closed + reinit",
                     (unsigned)BRAMBLE_CAD_TIMEOUT_REINIT_THRESHOLD);
            lr11xx_hal_nrf_request_reinit();
            return true;
        }
        ESP_LOGW(TAG, "CAD check timed out after %u ms, failing open", (unsigned)timeout_ms);
        return false;
    }

    cad_timeout_policy_on_success(&s_cad_timeout_policy);
    return s_cad_result;
}

void radio_set_tx_power(int8_t power) {
    s_config.tx_power = power;
    if (set_pa(power) != 0) {
        ESP_LOGE(TAG, "radio_set_tx_power(%d) failed, power unchanged on chip", power);
    }
}

radio_state_t radio_get_state(void) { return (radio_state_t)atomic_load(&s_state); }

void radio_set_rx_callback(radio_rx_callback_t cb) { s_rx_cb = cb; }

void radio_set_tx_done_callback(radio_tx_done_callback_t cb) { s_tx_done_cb = cb; }

void radio_sleep(void) {
    radio_standby();
    lr11xx_system_sleep_cfg_t sleep_cfg = {
        .is_warm_start = true,
        .is_rtc_timeout = false,
    };
    if (lr11xx_system_set_sleep(s_lr, sleep_cfg, 0) != LR11XX_STATUS_OK) {
        ESP_LOGE(TAG, "radio_sleep failed, radio still awake");
        return;
    }
    atomic_store(&s_state, RADIO_STATE_SLEEP);
}

bool radio_check_and_clear_reinit(void) {
    if (!lr11xx_hal_nrf_needs_reinit())
        return false;
    lr11xx_hal_nrf_clear_reinit();
    ESP_LOGW(TAG, "Radio reinit after hard reset, reconfiguring");
    /* The hard reset lost the system settings too; redo the full bring-up
     * before the parameter reconfigure. */
    if (lr1110_system_init() != 0) {
        ESP_LOGE(TAG, "LR1110 system re-init failed after hard reset");
        return true;
    }
    int rc = radio_reconfigure(&s_config);
    if (rc != 0) {
        ESP_LOGE(TAG, "Radio reconfigure failed after hard reset: %d", rc);
    }
    return true;
}
