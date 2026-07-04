#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * Format a 32-bit address as an 8-digit uppercase hex string.
 */
const char* addr_hex(uint32_t addr, char* buf, size_t len);
