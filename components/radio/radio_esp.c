/*
 * ESP-IDF radio implementation using the SX1262 driver.
 * Implements the radio.h interface for Heltec WiFi LoRa 32 V3.
 */

#ifdef ESP_PLATFORM

#include "radio.h"
#include "radio_internal.h"
#include "sx1262.h"
#include "tx_gate.h"
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

/* Task waiting for TX done. Written by the sender, read by radio_task, so it
 * is atomic rather than plain: both sides claim it with an exchange, which
 * means exactly one of them wins. radio_task can therefore never notify a
 * handle the sender has already abandoned, and the sender knows from the
 * exchange result whether a notification is in flight and must be drained. */
static _Atomic(TaskHandle_t) s_tx_waiter;

static volatile bool s_cad_result;
static SemaphoreHandle_t s_cad_sem;

/* Serializes radio_task's RX-done read sequence (GetRxBufferStatus ->
 * ReadBuffer -> GetPacketStatus) against radio_reconfigure. It is deliberately
 * NOT the transmit gate lock (tx_gate_radio_lock): radio_transmit_raw and
 * radio_cad_check hold that gate while blocked waiting for radio_task to
 * service the TX-done / CAD-done IRQ, so making radio_task take the gate would
 * deadlock (radio_task could not drain the IRQ that wakes the waiter that holds
 * the gate). This dedicated lock is never held by any path that waits on
 * radio_task, so radio_task can always make progress (issue #225, following up
 * on the TX-side serialization from #82). */
static SemaphoreHandle_t s_rx_seq_mutex;

static void rx_seq_lock(void) {
    if (s_rx_seq_mutex)
        xSemaphoreTake(s_rx_seq_mutex, portMAX_DELAY);
}

static void rx_seq_unlock(void) {
    if (s_rx_seq_mutex)
        xSemaphoreGive(s_rx_seq_mutex);
}

/* Consecutive CAD-timeout run state for the fail-open/closed policy (#118). */
static cad_timeout_policy_t s_cad_timeout_policy;

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/* STDBY_RC (mode 0): the SX1262 auto-enables TCXO via DIO3 when entering
 * TX or RX, so we don't need to stay in STDBY_XOSC between commands. */
static inline int radio_standby(void) { return sx1262_set_standby(0); }

static int set_sync_word(uint8_t sw) {
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
    /* A silently dropped write leaves the node on the wrong sync word and
     * therefore invisible to the mesh, so the failure has to surface. */
    return sx1262_write_register(0x0740, regs, 2);
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

    rc = sx1262_set_modulation_params(cfg->sf, cfg->bw_hz, cfg->coding_rate, 0xFF /* auto LDRO */);
    if (rc != 0)
        return rc;

    rc = sx1262_set_packet_params(cfg->preamble, cfg->explicit_header ? 0 : 1,
                                  255, /* max payload for RX */
                                  cfg->crc ? 1 : 0, 0 /* normal IQ */);
    if (rc != 0)
        return rc;

    rc = set_sync_word(cfg->sync_word);
    if (rc != 0) {
        ESP_LOGE(TAG, "set_sync_word failed (rc=%d)", rc);
        return rc;
    }

    /* Route TxDone, RxDone, CRC error, Timeout, CAD done/detected to DIO1 */
    uint16_t irq_mask = SX1262_IRQ_TX_DONE | SX1262_IRQ_RX_DONE | SX1262_IRQ_CRC_ERR |
                        SX1262_IRQ_TIMEOUT | SX1262_IRQ_CAD_DONE | SX1262_IRQ_CAD_DETECTED;
    rc = sx1262_set_dio_irq_params(irq_mask, irq_mask, 0x0000, 0x0000);
    if (rc != 0)
        return rc;

    rc = sx1262_set_cad_params(BRAMBLE_CAD_SYMBOL_NUM_REG, 22, 10, 0x00, 0);
    return rc;
}

/* Claim the TX waiter and wake it. Called from radio_task only. */
static void wake_tx_waiter(void) {
    TaskHandle_t waiter = atomic_exchange(&s_tx_waiter, (TaskHandle_t)NULL);
    if (waiter) {
        xTaskNotifyGive(waiter);
    }
}

/* Grace window for a give that radio_task has already committed to but has
 * not delivered yet. Expressed in ticks, not milliseconds: pdMS_TO_TICKS() of
 * a few ms rounds down to 0 on a 100 Hz tick, which would make the drain
 * below non-blocking and let a genuinely in-flight notification slip past. */
#define TX_DISARM_DRAIN_TICKS 2

/* Disarm the TX waiter from the sender side and make sure no notification
 * survives into the next transmit. If the exchange returns NULL, radio_task
 * already claimed the handle and its give may still be in flight, so wait
 * briefly for it; otherwise a stale notification would make the next
 * radio_transmit_raw return instantly as success while tx_gate debits airtime
 * for a frame that was never confirmed on air. */
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

/* Read one received frame under the RX-sequence lock, so a concurrent
 * radio_reconfigure cannot splice its commands between GetRxBufferStatus,
 * ReadBuffer and GetPacketStatus and return RSSI/SNR that belong to a different
 * radio configuration (issue #225). The user callback runs AFTER the lock is
 * released: it may be slow or re-enter the radio, and holding the sequence lock
 * across it would needlessly serialize it against reconfigure. */
static void radio_handle_rx_done(uint8_t* buf, size_t buf_size) {
    rx_seq_lock();

    uint8_t len = 0, offset = 0;
    sx1262_get_rx_buffer_status(&len, &offset);

    radio_rx_info_t info = {0};
    bool have_frame = false;
    if (len > 0 && len <= buf_size) {
        sx1262_read_buffer(offset, buf, len);
        info.len = len;
        sx1262_get_packet_status(&info.rssi, &info.snr);
        have_frame = true;
    }

    rx_seq_unlock();

    if (have_frame) {
        ESP_LOGD(TAG, "RX: %u bytes, RSSI %d, SNR %d", (unsigned)len, (int)info.rssi,
                 (int)info.snr);
        if (s_rx_cb) {
            s_rx_cb(buf, len, &info);
        }
    }
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
            wake_tx_waiter();
        }

        if (irq & SX1262_IRQ_RX_DONE) {
            if (irq & SX1262_IRQ_CRC_ERR) {
                ESP_LOGD(TAG, "RX CRC error, discarding");
            } else {
                radio_handle_rx_done(buf, sizeof(buf));
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
            /* If TX timed out, wake the waiter */
            wake_tx_waiter();
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Profile defaults                                                   */
/* ------------------------------------------------------------------ */

int radio_reconfigure(const radio_config_t* config) {
    ESP_LOGI(TAG, "Reconfiguring radio: %.1f MHz SF%u BW%" PRIu32 " TX %ddBm",
             config->frequency_mhz, config->sf, config->bw_hz, config->tx_power);

    /* Hold the transmit serialization lock across the whole reconfigure. This
     * is reachable from the UI settings task and the RPC task, and its command
     * sequence (standby, delay, configure_radio's ~8 commands, radio_start_rx)
     * would otherwise splice into an in-flight radio_transmit_raw between its
     * write_buffer, set_packet_params, clear_irq and set_tx, since transmits
     * are serialized on this same lock but reconfigure took no lock at all
     * (issue #82). */
    tx_gate_radio_lock();
    /* Also exclude radio_task's RX-done read: changing the PA, frequency or
     * modulation partway through GetRxBufferStatus -> ReadBuffer ->
     * GetPacketStatus would hand the mesh telemetry for a different config
     * (issue #225). Ordered strictly inside the gate lock; nothing acquires
     * these in the opposite order, so there is no deadlock. */
    rx_seq_lock();

    /* Put radio in standby before reconfiguring (0 = RC oscillator). A failure
     * here is not fatal: reconfiguring is exactly the recovery a freshly reset
     * chip needs, so log and carry on into configure_radio. */
    int standby_rc = radio_standby();
    if (standby_rc != 0) {
        ESP_LOGW(TAG, "standby before reconfigure failed (rc=%d), continuing", standby_rc);
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

    /* Resume RX */
    radio_start_rx();
    rx_seq_unlock();
    tx_gate_radio_unlock();
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

    /* Create the RX-sequence lock before the radio task or DIO1 ISR can fire,
     * so the very first RX-done read is already serialized against a concurrent
     * reconfigure (issue #225). */
    s_rx_seq_mutex = xSemaphoreCreateMutex();
    if (!s_rx_seq_mutex) {
        ESP_LOGE(TAG, "Failed to create RX sequence mutex");
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

    /* Start continuous RX. A failure is logged rather than failing init: the
     * task and ISR are already up, so the reinit path can still recover the
     * chip, whereas bailing here would leave the node with no radio at all. */
    radio_start_rx();
    if (radio_get_state() != RADIO_STATE_RX) {
        ESP_LOGE(TAG, "radio_init: could not enter RX, node is not receiving yet");
    }

    ESP_LOGD(TAG, "radio_init complete");
    return 0;
}

/* Abort an in-progress transmit: disarm the waiter, report the state
 * honestly, and pick a recovery. After a hard reset the chip sits in
 * power-on defaults, so issuing more commands (including radio_start_rx)
 * would just fail against an unconfigured chip; the mesh loop's
 * radio_check_and_clear_reinit() reconfigures and restarts RX instead. */
static int tx_abort(const char* what, int rc) {
    ESP_LOGE(TAG, "TX aborted: %s failed (rc=%d)", what, rc);
    tx_disarm();
    atomic_store(&s_state, RADIO_STATE_IDLE);
    if (rc == SX1262_ERR_RESET) {
        ESP_LOGW(TAG, "SX1262 was hard reset, deferring recovery to radio reinit");
    } else {
        radio_start_rx();
    }
    return -1;
}

int radio_transmit_raw(const uint8_t* data, uint8_t len) {
    ESP_LOGD(TAG, "radio_transmit_raw: %u bytes", len);

    /* Drop anything left over from a previously abandoned TX before arming,
     * so a late notification can never be read as this frame's TxDone. */
    ulTaskNotifyTake(pdTRUE, 0);

    atomic_store(&s_state, RADIO_STATE_TX);
    atomic_store(&s_tx_waiter, xTaskGetCurrentTaskHandle());

    /* Switch to standby */
    int rc = radio_standby();
    if (rc != 0) {
        return tx_abort("set_standby", rc);
    }

    /* Write payload to buffer. If this is skipped the chip transmits whatever
     * is still in the FIFO from the previous frame, so it must be checked. */
    rc = sx1262_write_buffer(0, data, len);
    if (rc != 0) {
        return tx_abort("write_buffer", rc);
    }

    /* Update packet params with actual payload length */
    rc = sx1262_set_packet_params(s_config.preamble, s_config.explicit_header ? 0 : 1, len,
                                  s_config.crc ? 1 : 0, 0);
    if (rc != 0) {
        return tx_abort("set_packet_params", rc);
    }

    /* Clear IRQ and start TX with 3s hardware timeout */
    rc = sx1262_clear_irq_status(0x03FF);
    if (rc != 0) {
        return tx_abort("clear_irq_status", rc);
    }
    rc = sx1262_set_tx(3000);
    if (rc != 0) {
        return tx_abort("set_tx", rc);
    }
    ESP_LOGI(TAG, "TX started: %u bytes, DIO1_GPIO=%d, BUSY_GPIO=%d", len,
             gpio_get_level(board_get_config()->radio.dio1),
             gpio_get_level(board_get_config()->radio.busy));

    /* Reset WDT before the blocking wait; TX can take up to 4s and the
     * caller may have consumed most of the 5s WDT window already. */
    esp_task_wdt_reset();

    /* Wait for TX done (or TX timeout) notification, max 4s FreeRTOS timeout */
    uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(4000));

    if (notified == 0) {
        /* Disarm before anything else. Draining here is what stops a TxDone
         * that lands just after the expiry from being consumed by the next
         * radio_transmit_raw as a phantom success. */
        tx_disarm();

        /* Diagnostic: read back what the radio chip thinks happened */
        uint16_t irq_status = sx1262_get_irq_status();
        int dio1_level = gpio_get_level(board_get_config()->radio.dio1);
        ESP_LOGE(TAG, "TX timeout: DIO1=%d IRQ_reg=0x%04x (TxDone=%d Timeout=%d)", dio1_level,
                 irq_status, (irq_status & SX1262_IRQ_TX_DONE) ? 1 : 0,
                 (irq_status & SX1262_IRQ_TIMEOUT) ? 1 : 0);
        atomic_store(&s_state, RADIO_STATE_IDLE);
        /* radio_start_rx() does its own standby with error handling, so no
         * bare unchecked standby here. */
        radio_start_rx();
        return -1;
    }

    /* radio_task claimed the waiter before notifying, so s_tx_waiter is
     * already NULL here; store it anyway so the invariant does not depend on
     * which IRQ path did the wake. */
    atomic_store(&s_tx_waiter, (TaskHandle_t)NULL);

    /* Call TX done callback */
    if (s_tx_done_cb) {
        s_tx_done_cb();
    }

    /* Return to RX */
    radio_start_rx();
    return 0;
}

void radio_start_rx(void) {
    int rc = radio_standby();
    if (rc == 0) {
        rc = sx1262_clear_irq_status(0x03FF);
    }
    if (rc == 0) {
        rc = sx1262_set_rx(0); /* continuous */
    }

    if (rc != 0) {
        /* Do not claim RX. Setting the state unconditionally used to make
         * radio_get_state() report a healthy receiver while the node was a
         * black hole; IDLE is the honest answer for a chip that is not
         * listening, and the reinit path is what brings it back. */
        if (rc == SX1262_ERR_RESET) {
            ESP_LOGE(TAG, "start_rx: SX1262 was hard reset, awaiting radio reinit");
        } else {
            ESP_LOGE(TAG, "start_rx failed (rc=%d), radio is NOT receiving", rc);
        }
        atomic_store(&s_state, RADIO_STATE_IDLE);
        return;
    }

    atomic_store(&s_state, RADIO_STATE_RX);
}

void radio_cad(void) {
    int rc = radio_standby();
    if (rc == 0) {
        rc = sx1262_clear_irq_status(0x03FF);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "radio_cad: standby/clear_irq failed (rc=%d)", rc);
        atomic_store(&s_state, RADIO_STATE_IDLE);
        return;
    }

    atomic_store(&s_state, RADIO_STATE_CAD);
    rc = sx1262_set_cad();
    if (rc != 0) {
        ESP_LOGE(TAG, "radio_cad: set_cad failed (rc=%d)", rc);
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

    /* radio_cad() already does the standby and IRQ clear, with error handling.
     * If it cannot arm CAD it leaves the state IDLE and no CAD_DONE arrives,
     * so the take below expires and this returns false ("channel clear"),
     * which is the same fail-open the timeout path has always had. Checking
     * radio_get_state() here instead would race the CAD_DONE IRQ, which can
     * land before the check on short spreading factors. */
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
        cad_timeout_action_t action = cad_timeout_policy_on_timeout(&s_cad_timeout_policy);
        if (action == CAD_TIMEOUT_FAIL_CLOSED) {
            /* The radio has missed the CAD budget BRAMBLE_CAD_TIMEOUT_REINIT_THRESHOLD
             * times running: treat it as wedged. Report busy so tx_gate backs
             * off instead of transmitting blind without LBT, and flag a reinit
             * for the next radio_check_and_clear_reinit() to act on. */
            ESP_LOGE(TAG,
                     "CAD check timed out after %u ms (sf=%u bw=%u): %u consecutive, failing "
                     "closed and requesting radio reinit",
                     (unsigned)timeout_ms, (unsigned)s_config.sf, (unsigned)s_config.bw_hz,
                     (unsigned)BRAMBLE_CAD_TIMEOUT_REINIT_THRESHOLD);
            sx1262_request_reinit();
            return true;
        }
        /* Fail open: transmit anyway, as before. A one-off timeout is more
         * likely a transient missed IRQ than a dead radio. */
        ESP_LOGW(TAG, "CAD check timed out after %u ms (sf=%u bw=%u), failing open",
                 (unsigned)timeout_ms, (unsigned)s_config.sf, (unsigned)s_config.bw_hz);
        return false;
    }

    /* A completed CAD (busy or clear) means the radio answered; reset the run. */
    cad_timeout_policy_on_success(&s_cad_timeout_policy);
    return s_cad_result;
}

void radio_set_tx_power(int8_t power) {
    s_config.tx_power = power;
    int rc = sx1262_set_pa_config(power);
    if (rc == 0) {
        rc = sx1262_set_tx_params(power, 0x04);
    }
    if (rc != 0) {
        /* Not fatal: the radio keeps transmitting at its previous power. Log
         * it so a node running at the wrong power is visible in the trace. */
        ESP_LOGE(TAG, "radio_set_tx_power(%d) failed (rc=%d), power unchanged on chip", power, rc);
    }
}

radio_state_t radio_get_state(void) { return (radio_state_t)atomic_load(&s_state); }

void radio_set_rx_callback(radio_rx_callback_t cb) { s_rx_cb = cb; }

void radio_set_tx_done_callback(radio_tx_done_callback_t cb) { s_tx_done_cb = cb; }

void radio_sleep(void) {
    radio_standby();
    int rc = sx1262_set_sleep(0x04); /* warm start (retain config) */
    if (rc != 0) {
        /* The chip is still awake, so do not report SLEEP. */
        ESP_LOGE(TAG, "radio_sleep: set_sleep failed (rc=%d), radio still awake", rc);
        return;
    }
    atomic_store(&s_state, RADIO_STATE_SLEEP);
}

bool radio_check_and_clear_reinit(void) {
    if (!sx1262_needs_reinit())
        return false;
    sx1262_clear_reinit();
    ESP_LOGW(TAG, "Radio reinit after hard reset, reconfiguring");
    int rc = radio_reconfigure(&s_config);
    if (rc != 0) {
        ESP_LOGE(TAG, "Radio reconfigure failed after hard reset: %d", rc);
    }
    return true;
}

#endif /* ESP_PLATFORM */
