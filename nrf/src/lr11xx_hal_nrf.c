// SWDR001 HAL on nrfx SPIM2 for the Wio-WM1110's LR1110.
//
// Protocol rules implemented here (sources: SWDR001 lr11xx_hal.h docs and
// Seeed's WM1110 vendor HAL, researched 2026-07-27):
// - Wait for BUSY low before EVERY command phase, including the second phase
//   of a read. Exception: after SetSleep (opcode 0x01 0x1B) BUSY is held
//   high by design; skip the wait and reawaken via an NSS glitch.
// - During read phases MOSI must drive only 0x00 (NOPs): nonzero bytes can
//   be interpreted as commands.
// - Reset: NRESET low 5ms, high, then 250ms for the chip firmware to boot.
// - BUSY stuck three times consecutively => hard reset + needs_reinit latch
//   (the same fail-abort contract as sx1262.c BUSY_STUCK_THRESHOLD).
#include "lr11xx_hal_nrf.h"

#include <string.h>

#include <FreeRTOS.h>
#include <task.h>

#include <hal/nrf_gpio.h>
#include <nrfx_spim.h>

#include "esp_log.h"
#include "lr11xx_hal.h"
#include "wio_wm1110_devkit.h"

static const char* TAG = "lr11xx_hal";

#define LR_BUSY_TIMEOUT_MS 1000
#define LR_BUSY_STUCK_THRESHOLD 3
#define LR_RESET_BOOT_MS 250

typedef struct {
    nrfx_spim_t spim;
    bool radio_sleeping;
} lr11xx_hal_ctx_t;

static lr11xx_hal_ctx_t s_ctx = {
    .spim = NRFX_SPIM_INSTANCE(2),
};
static bool s_spim_ready;
static volatile bool s_needs_reinit;
static uint8_t s_busy_stuck_count;

static void nss_assert(void) { nrf_gpio_pin_clear(BOARD_PIN_LORA_NSS); }
static void nss_release(void) { nrf_gpio_pin_set(BOARD_PIN_LORA_NSS); }

void lr11xx_hal_nrf_hard_reset(void) {
    nrf_gpio_pin_clear(BOARD_PIN_LORA_RESET);
    vTaskDelay(pdMS_TO_TICKS(5));
    nrf_gpio_pin_set(BOARD_PIN_LORA_RESET);
    vTaskDelay(pdMS_TO_TICKS(LR_RESET_BOOT_MS));
    s_ctx.radio_sleeping = false;
}

bool lr11xx_hal_nrf_needs_reinit(void) { return s_needs_reinit; }
void lr11xx_hal_nrf_clear_reinit(void) { s_needs_reinit = false; }
void lr11xx_hal_nrf_request_reinit(void) { s_needs_reinit = true; }

// Returns true when BUSY reached low; on the third consecutive timeout it
// hard-resets the chip and latches needs_reinit (caller must abort, not
// retry: the chip is back at POR defaults).
static bool wait_busy_low(void) {
    TickType_t start = xTaskGetTickCount();
    while (nrf_gpio_pin_read(BOARD_PIN_LORA_BUSY)) {
        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(LR_BUSY_TIMEOUT_MS)) {
            if (++s_busy_stuck_count >= LR_BUSY_STUCK_THRESHOLD) {
                ESP_LOGE(TAG, "BUSY stuck %u times, hard reset + reinit request",
                         s_busy_stuck_count);
                s_busy_stuck_count = 0;
                lr11xx_hal_nrf_hard_reset();
                lr11xx_hal_nrf_request_reinit();
            }
            return false;
        }
        taskYIELD();
    }
    s_busy_stuck_count = 0;
    return true;
}

static bool ensure_ready(lr11xx_hal_ctx_t* ctx) {
    if (ctx->radio_sleeping) {
        // Wake with an NSS glitch, then wait ready.
        nss_assert();
        vTaskDelay(1);
        nss_release();
        ctx->radio_sleeping = false;
    }
    return wait_busy_low();
}

static bool spim_xfer(const uint8_t* tx, uint8_t* rx, size_t len) {
    nrfx_spim_xfer_desc_t desc = NRFX_SPIM_XFER_TRX(tx, tx ? len : 0, rx, rx ? len : 0);
    if (tx == NULL) {
        desc = (nrfx_spim_xfer_desc_t)NRFX_SPIM_XFER_RX(rx, len);
    } else if (rx == NULL) {
        desc = (nrfx_spim_xfer_desc_t)NRFX_SPIM_XFER_TX(tx, len);
    }
    return nrfx_spim_xfer(&s_ctx.spim, &desc, 0) == NRFX_SUCCESS;
}

lr11xx_hal_status_t lr11xx_hal_write(const void* context, const uint8_t* command,
                                     const uint16_t command_length, const uint8_t* data,
                                     const uint16_t data_length) {
    lr11xx_hal_ctx_t* ctx = (lr11xx_hal_ctx_t*)(uintptr_t)context;
    if (!ensure_ready(ctx)) {
        return LR11XX_HAL_STATUS_ERROR;
    }
    nss_assert();
    bool ok = spim_xfer(command, NULL, command_length);
    if (ok && data_length > 0) {
        ok = spim_xfer(data, NULL, data_length);
    }
    nss_release();
    // SetSleep (0x01 0x1B): BUSY stays high until wakeup; remember so the
    // next command wakes the chip instead of timing out on BUSY.
    if (ok && command_length >= 2 && command[0] == 0x01 && command[1] == 0x1B) {
        ctx->radio_sleeping = true;
        vTaskDelay(1); // let the chip actually enter sleep
    }
    return ok ? LR11XX_HAL_STATUS_OK : LR11XX_HAL_STATUS_ERROR;
}

lr11xx_hal_status_t lr11xx_hal_read(const void* context, const uint8_t* command,
                                    const uint16_t command_length, uint8_t* data,
                                    const uint16_t data_length) {
    lr11xx_hal_ctx_t* ctx = (lr11xx_hal_ctx_t*)(uintptr_t)context;
    if (!ensure_ready(ctx)) {
        return LR11XX_HAL_STATUS_ERROR;
    }
    // Phase 1: send the command.
    nss_assert();
    bool ok = spim_xfer(command, NULL, command_length);
    nss_release();
    if (!ok) {
        return LR11XX_HAL_STATUS_ERROR;
    }
    // Phase 2: after BUSY drops, clock out 1 dummy byte then the payload,
    // driving NOPs on MOSI throughout (RX-only transfers shift out 0x00 on
    // nrfx SPIM via the ORC character, configured to 0x00 at init).
    if (!wait_busy_low()) {
        return LR11XX_HAL_STATUS_ERROR;
    }
    nss_assert();
    uint8_t dummy;
    ok = spim_xfer(NULL, &dummy, 1);
    if (ok && data_length > 0) {
        ok = spim_xfer(NULL, data, data_length);
    }
    nss_release();
    return ok ? LR11XX_HAL_STATUS_OK : LR11XX_HAL_STATUS_ERROR;
}

lr11xx_hal_status_t lr11xx_hal_direct_read(const void* context, uint8_t* data,
                                           const uint16_t data_length) {
    lr11xx_hal_ctx_t* ctx = (lr11xx_hal_ctx_t*)(uintptr_t)context;
    if (!ensure_ready(ctx)) {
        return LR11XX_HAL_STATUS_ERROR;
    }
    nss_assert();
    bool ok = spim_xfer(NULL, data, data_length);
    nss_release();
    return ok ? LR11XX_HAL_STATUS_OK : LR11XX_HAL_STATUS_ERROR;
}

lr11xx_hal_status_t lr11xx_hal_reset(const void* context) {
    (void)context;
    lr11xx_hal_nrf_hard_reset();
    return LR11XX_HAL_STATUS_OK;
}

lr11xx_hal_status_t lr11xx_hal_wakeup(const void* context) {
    lr11xx_hal_ctx_t* ctx = (lr11xx_hal_ctx_t*)(uintptr_t)context;
    return ensure_ready(ctx) ? LR11XX_HAL_STATUS_OK : LR11XX_HAL_STATUS_ERROR;
}

lr11xx_hal_status_t lr11xx_hal_abort_blocking_cmd(const void* context) {
    (void)context;
    // Not used by the LoRa command set this firmware issues.
    return LR11XX_HAL_STATUS_OK;
}

const void* lr11xx_hal_nrf_init(void) {
    if (!s_spim_ready) {
        nrf_gpio_cfg_output(BOARD_PIN_LORA_NSS);
        nrf_gpio_pin_set(BOARD_PIN_LORA_NSS);
        nrf_gpio_cfg_output(BOARD_PIN_LORA_RESET);
        nrf_gpio_pin_set(BOARD_PIN_LORA_RESET);
        nrf_gpio_cfg_input(BOARD_PIN_LORA_BUSY, NRF_GPIO_PIN_NOPULL);

        nrfx_spim_config_t cfg =
            NRFX_SPIM_DEFAULT_CONFIG(BOARD_PIN_LORA_SCK, BOARD_PIN_LORA_MOSI, BOARD_PIN_LORA_MISO,
                                     NRF_SPIM_PIN_NOT_CONNECTED);
        cfg.frequency = NRFX_MHZ_TO_HZ(8);
        cfg.mode = NRF_SPIM_MODE_0;
        cfg.bit_order = NRF_SPIM_BIT_ORDER_MSB_FIRST;
        cfg.orc = 0x00; // NOPs on MOSI during RX-only phases (protocol rule)
        if (nrfx_spim_init(&s_ctx.spim, &cfg, NULL, NULL) != NRFX_SUCCESS) {
            ESP_LOGE(TAG, "SPIM2 init failed");
            return NULL;
        }
        s_spim_ready = true;
    }
    return &s_ctx;
}
