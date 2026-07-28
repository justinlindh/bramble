// Integrator glue for Apache NimBLE on this target; see nimble_glue.c.
#pragma once

#include <stdbool.h>

// Starts the low-frequency clock the BLE controller's RTC0 time base needs.
// Returns true when the 32.768kHz crystal is driving it, false when the RC
// oscillator is (less accurate; BLE_LL_SCA in the syscfg is set for RC).
bool nimble_glue_start_lfclk(void);

void nrf52_clock_hfxo_request(void);
void nrf52_clock_hfxo_release(void);
