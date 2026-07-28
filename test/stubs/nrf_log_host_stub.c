/*
 * Host stand-in for the nRF target's log sink (nrf/shim/console_uart.c owns
 * the real one, which writes blocking UART). Host suites that compile nRF
 * shim sources need the symbol; routing it to stderr keeps failure messages
 * visible in test output without polluting stdout.
 */
#include <stdarg.h>
#include <stdio.h>

void bramble_log_write(char level, const char* tag, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%c (%s) ", level, tag);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}
