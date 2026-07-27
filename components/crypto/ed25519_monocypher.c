// Monocypher Ed25519 provider (nRF52840; see bramble_ed25519_provider.h).
// Monocypher's optional ed25519 unit is Ed25519 with SHA-512, wire-identical
// to libsodium/OpenSSL.
//
// Symbol-rename contract: Monocypher exports crypto_ed25519_sign and
// crypto_ed25519_check, which collide with Bramble's crypto.h names at link
// time. The Monocypher objects AND this TU are therefore compiled with
// mono_ed25519_* -D renames (see bramble_add_monocypher_ed25519 in
// crypto_deps.cmake), and this file must never include crypto.h.
#include "bramble_ed25519_provider.h"

#include <string.h>

#include <monocypher-ed25519.h>
#include <monocypher.h>

int bramble_ed25519_impl_keypair_from_seed(const uint8_t seed[32], uint8_t public_key[32],
                                           uint8_t private_key[64]) {
    // Monocypher wipes the seed buffer it is given; the Bramble contract says
    // the caller owns the seed's lifetime, so feed it a copy.
    uint8_t seed_copy[32];
    memcpy(seed_copy, seed, sizeof(seed_copy));
    crypto_ed25519_key_pair(private_key, public_key, seed_copy);
    // seed_copy was wiped by Monocypher itself.
    return 0;
}

int bramble_ed25519_impl_sign(const uint8_t private_key[64], const uint8_t* msg, size_t msg_len,
                              uint8_t sig[64]) {
    crypto_ed25519_sign(sig, private_key, msg, msg_len);
    return 0;
}

bool bramble_ed25519_impl_verify(const uint8_t public_key[32], const uint8_t* msg, size_t msg_len,
                                 const uint8_t sig[64]) {
    return crypto_ed25519_check(sig, public_key, msg, msg_len) == 0;
}
