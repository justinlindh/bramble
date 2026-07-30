// Link-time Ed25519 provider seam for the device crypto backend
// (crypto_esp.c). Exactly one provider TU links per platform:
// ed25519_sodium.c (ESP-IDF targets, libsodium) or ed25519_monocypher.c
// (nRF52840, Monocypher). Both implement RFC 8032 Ed25519 with the libsodium
// 64-byte secret-key layout (seed || public key); the shared RFC 8032 vector
// suites (test_ed25519 and test_ed25519_nrf_backend) pin them to identical
// behavior. crypto_esp.c calls only these three functions, so provider
// selection lives entirely at the link line, never in preprocessor forks.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

int bramble_ed25519_impl_keypair_from_seed(const uint8_t seed[32], uint8_t public_key[32],
                                           uint8_t private_key[64]);
int bramble_ed25519_impl_sign(const uint8_t private_key[64], const uint8_t* msg, size_t msg_len,
                              uint8_t sig[64]);
bool bramble_ed25519_impl_verify(const uint8_t public_key[32], const uint8_t* msg, size_t msg_len,
                                 const uint8_t sig[64]);
