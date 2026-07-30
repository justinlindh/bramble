// UART console for the nRF52840 target: blocking TX on the board's console
// pins (BOARD_PIN_CONSOLE_TX/RX in nrf/boards/*.h), 115200 8N1.
#pragma once

#include <stddef.h>
#include <stdint.h>

void console_init(void);
void console_write(const char* buf, size_t len);

// Milliseconds since boot for log timestamps; provided by main on the
// FreeRTOS tick (reads 0 before the scheduler starts).
uint32_t bramble_log_timestamp_ms(void);
