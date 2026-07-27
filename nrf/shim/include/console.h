// UART console for the nRF52840 target: blocking TX to the pin pair selected
// by BRAMBLE_CONSOLE_UART (1 or 2, see nrf/boards/*.h), 115200 8N1.
#pragma once

#include <stddef.h>
#include <stdint.h>

void console_init(void);
void console_write(const char* buf, size_t len);

// Milliseconds since boot for log timestamps. Weak default returns 0 until
// the FreeRTOS tick provides it.
uint32_t bramble_log_timestamp_ms(void);
