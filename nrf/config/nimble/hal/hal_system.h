// Mynewt HAL system shim.
#pragma once
#include <nrfx.h>

static inline void hal_system_reset(void) { NVIC_SystemReset(); }
