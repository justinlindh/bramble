#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Constant-time string comparison to prevent timing side-channels.
 * Returns 0 if strings match, non-zero otherwise. */
static inline int ct_strcmp(const char *a, const char *b)
{
    size_t len_a = strlen(a);
    size_t len_b = strlen(b);
    volatile uint8_t result = (len_a != len_b) ? 1 : 0;
    size_t min_len = len_a < len_b ? len_a : len_b;
    for (size_t i = 0; i < min_len; i++) {
        result |= ((volatile uint8_t)a[i]) ^ ((volatile uint8_t)b[i]);
    }
    return result;
}
