/*
 * Backend selection for the crypto suites, in one place: the default build
 * pins the host (OpenSSL) backend; the _nrf_backend build of the same test
 * source pins the nRF52840 backend (crypto_esp.c: mbedtls + Monocypher
 * Ed25519 via the provider seam) to identical vectors. The backend .c is
 * included into the test TU, matching the suites' existing pattern.
 */
#pragma once

#ifdef BRAMBLE_TEST_NRF_BACKEND
#include "../components/crypto/crypto_esp.c"
#else
#include "../components/crypto/crypto_host.c"
#endif
