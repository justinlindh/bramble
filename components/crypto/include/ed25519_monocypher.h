// Ed25519 provider for the nRF52840 backend of crypto_esp.c, implemented on
// Monocypher's optional ed25519 unit (Ed25519 with SHA-512, RFC 8032, the
// same construction and libsodium seed||pub secret-key layout as the other
// two backends).
//
// Why a wrapper TU exists at all: Monocypher exports crypto_ed25519_sign and
// crypto_ed25519_check, which collide with Bramble's own crypto.h names at
// link time. The Monocypher objects are therefore compiled with -D renames
// (mono_ed25519_*), and this wrapper is the only TU that sees Monocypher's
// header; crypto_esp.c sees only these bramble_mono_* functions.
#pragma once

#ifdef BRAMBLE_PLATFORM_NRF

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

int bramble_mono_ed25519_keypair_from_seed(const uint8_t seed[32], uint8_t public_key[32],
                                           uint8_t private_key[64]);
int bramble_mono_ed25519_sign(const uint8_t private_key[64], const uint8_t* msg, size_t msg_len,
                              uint8_t sig[64]);
bool bramble_mono_ed25519_verify(const uint8_t public_key[32], const uint8_t* msg, size_t msg_len,
                                 const uint8_t sig[64]);

#endif // BRAMBLE_PLATFORM_NRF
