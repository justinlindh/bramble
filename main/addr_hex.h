#pragma once

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/**
 * Format a 32-bit address as an 8-digit uppercase hex string.
 *
 * Dependency-free (no cJSON, no traffic_debug) on purpose, so files that need
 * only the address format can share one definition. topology_export.c is the
 * reason it lives apart from util.h: it compiles into the Go simulator
 * (simulator/gosim/all.c) and must not drag util.h's heavier includes along.
 */
static inline const char* addr_hex(uint32_t addr, char* buf, size_t len) {
    snprintf(buf, len, "%08" PRIX32, addr);
    return buf;
}
