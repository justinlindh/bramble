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
#define MBEDTLS_HKDF_C
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED

// Platform: route mbedtls_calloc/free to the FreeRTOS heap at runtime
// (bramble_mbedtls_platform_init) so the chip has ONE accounted heap.
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_NO_PLATFORM_ENTROPY

// RAM/flash tuning
#define MBEDTLS_AES_ROM_TABLES
#define MBEDTLS_ECP_WINDOW_SIZE 2
#define MBEDTLS_ECP_FIXED_POINT_OPTIM 0
