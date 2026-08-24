#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Upper bound for compared strings. Must be >= the auth token storage
 * size (AUTH_TOKEN_MAX in ws_server.c is 128, and bramble.setAuthToken
 * rejects anything >= 128 bytes), so stored secrets are never truncated.
 * Inputs longer than the bound compare on their first 128 bytes only;
 * that cannot create a false accept against an in-bounds secret because
 * the truncated lengths then differ. */
#define CT_STRCMP_BOUND 128

/* Constant-time string comparison for secrets (auth tokens).
 * Returns 0 if the strings match, non-zero otherwise.
 *
 * A compare that iterated min(len_a, len_b) times would let an attacker
 * discover the secret's length by probing with growing inputs and timing
 * the plateau. This always executes exactly CT_STRCMP_BOUND iterations
 * regardless of either operand, and folds the length difference into the
 * result instead of branching on it.
 *
 * Approach: fixed-bound padded compare (reads are predicated on index <
 * length, out-of-range positions contribute zero). It keeps this header
 * dependency-free for the BLE auth path; hash-then-compare would also work
 * but drags a crypto dependency into every caller for no additional benefit
 * at these sizes. The per-iteration bounds predicates compile to branchless
 * selects on our targets; any residual microarchitectural signal is orders
 * of magnitude below the byte-granular plateau a min-length loop leaks. */
static inline int ct_strcmp(const char* a, const char* b) {
    size_t len_a = strnlen(a, CT_STRCMP_BOUND);
    size_t len_b = strnlen(b, CT_STRCMP_BOUND);
    /* Both lengths are <= 128, so the XOR fits in one byte. */
    volatile uint8_t diff = (uint8_t)(len_a ^ len_b);
    for (size_t i = 0; i < CT_STRCMP_BOUND; i++) {
        uint8_t ca = (i < len_a) ? (uint8_t)a[i] : 0;
        uint8_t cb = (i < len_b) ? (uint8_t)b[i] : 0;
        diff |= (uint8_t)(ca ^ cb);
    }
    return diff;
}
