/*
 * Dev-only bench provisioning for the nRF52840 target (see nrf_provision.c).
 *
 * This function's return code is stamped straight into the flash boot trace,
 * so the meaning of each value is part of the diagnostic contract rather
 * than an implementation detail. A caller that declares it inline with a
 * bare `extern int f(void);` gets no such contract, which is how a routine
 * "no dev token in this build" result ended up recorded as a failing -1 on
 * every healthy production boot.
 */
#pragma once

/* Returned when the build carries no compile-time token to seed, which is
 * the normal case for anything but a bench build. Distinct from both
 * success and failure precisely so a boot trace can say so. */
#define BRAMBLE_TOKEN_SEED_SKIPPED 1

/* Seeds the RPC auth token from -DBRAMBLE_NRF_DEV_AUTH_TOKEN when the build
 * has one, and only when no token is already stored. 0 when a token is in
 * place (seeded here or already present), BRAMBLE_TOKEN_SEED_SKIPPED when
 * the build carries no token, -1 on a real failure. */
int nrf_seed_auth_token_from_build(void);
