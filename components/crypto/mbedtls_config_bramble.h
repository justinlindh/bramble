// Minimal mbedtls configuration for the Bramble nRF52840 target: exactly the
// modules crypto_esp.c uses (AES-256-GCM, SHA-256, HMAC/HKDF, X25519 via
// ECP), tuned for RAM over speed (ROM AES tables, small ECP window). The
// host nRF-backend test suites build against mbedtls's default config; this
// minimal set is validated by the target build plus the boot-time init path,
// which executes keygen, DH, GCM, and hashing on the bench.
#pragma once

// Core modules
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CIPHER_C
// The BLE link layer's crypto (nimble/controller/src/ble_ll_crypto.c) uses
// AES-CMAC; harmless for the ESP fleet and the host suites, which simply do
// not call it.
#define MBEDTLS_CMAC_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA256_C
// AES-CTR-DRBG backs esp_random on the nRF target. The chip's RNG peripheral
// belongs to the BLE link layer once the controller starts, so the DRBG is
// seeded from hardware at boot and serves every draw after that. Reuses the
// AES above, so this costs only ctr_drbg.c itself.
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_HKDF_C
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
// secp256r1 is not used by the mesh: it is the curve LE Secure Connections
// pairing runs its ECDH over (nimble/host/src/ble_sm_alg.c), and without it
// the BLE link cannot be encrypted at all.
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED

// Platform: route mbedtls_calloc/free to the FreeRTOS heap at runtime
// (bramble_mbedtls_platform_init) so the chip has ONE accounted heap.
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_NO_PLATFORM_ENTROPY

// RAM/flash tuning
#define MBEDTLS_AES_ROM_TABLES
#define MBEDTLS_ECP_WINDOW_SIZE 2
#define MBEDTLS_ECP_FIXED_POINT_OPTIM 0
// Fast reduction modulo the NIST primes. This applies only to secp256r1, so
// it costs the mesh's Curve25519 nothing (Montgomery curves take a different
// path entirely) and buys back most of the BLE pairing time: without it a
// pairing spends ~9s in two P-256 scalar multiplications and times out.
#define MBEDTLS_ECP_NIST_OPTIM
