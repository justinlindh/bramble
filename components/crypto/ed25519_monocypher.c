// See ed25519_monocypher.h for why this wrapper TU exists. This file must be
// compiled with the same mono_ed25519_* rename defines as the Monocypher
// sources (the build applies them to both), and must never include crypto.h.
#ifdef BRAMBLE_PLATFORM_NRF

#include "ed25519_monocypher.h"

#include <string.h>

#include <monocypher-ed25519.h>
#include <monocypher.h>

int bramble_mono_ed25519_keypair_from_seed(const uint8_t seed[32], uint8_t public_key[32],
                                           uint8_t private_key[64]) {
    // Monocypher wipes the seed buffer it is given; the Bramble contract says
    // the caller owns the seed's lifetime, so feed it a copy.
    uint8_t seed_copy[32];
    memcpy(seed_copy, seed, sizeof(seed_copy));
    crypto_ed25519_key_pair(private_key, public_key, seed_copy);
    // seed_copy was wiped by Monocypher itself.
    return 0;
}

int bramble_mono_ed25519_sign(const uint8_t private_key[64], const uint8_t* msg, size_t msg_len,
                              uint8_t sig[64]) {
    crypto_ed25519_sign(sig, private_key, msg, msg_len);
    return 0;
}

bool bramble_mono_ed25519_verify(const uint8_t public_key[32], const uint8_t* msg, size_t msg_len,
                                 const uint8_t sig[64]) {
    return crypto_ed25519_check(sig, public_key, msg, msg_len) == 0;
}

#endif // BRAMBLE_PLATFORM_NRF
