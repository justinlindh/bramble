// nrfx SPIM2 implementation of SWDR001's 6-function HAL contract for the
// LR1110 on the Wio-WM1110. The `context` passed to every SWDR001 call must
// be the value returned by lr11xx_hal_nrf_init().
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Initializes SPIM2 (8MHz, mode 0, software NSS) and the NSS/RESET/BUSY
// GPIO. Returns the SWDR001 context pointer, or NULL on driver init failure.
const void* lr11xx_hal_nrf_init(void);

// BUSY-stuck accounting, mirroring sx1262.c's three-strikes contract: after
// BUSY_STUCK_THRESHOLD consecutive BUSY-wait timeouts the HAL hard-resets
// the chip and latches needs_reinit; the radio layer polls and reconfigures.
bool lr11xx_hal_nrf_needs_reinit(void);
void lr11xx_hal_nrf_clear_reinit(void);
void lr11xx_hal_nrf_request_reinit(void);
void lr11xx_hal_nrf_hard_reset(void);
