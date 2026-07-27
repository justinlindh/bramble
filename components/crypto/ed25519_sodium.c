// libsodium Ed25519 provider (ESP-IDF targets; see bramble_ed25519_provider.h).
// sodium_init() is idempotent (returns 1 when already initialized) and only
// fails on catastrophic misconfiguration.
#include "bramble_ed25519_provider.h"

#include "sodium.h"

int bramble_ed25519_impl_keypair_from_seed(const uint8_t seed[32], uint8_t public_key[32],
                                           uint8_t private_key[64]) {
    if (sodium_init() < 0)
        return -1;
    return (crypto_sign_seed_keypair(public_key, private_key, seed) == 0) ? 0 : -1;
}

int bramble_ed25519_impl_sign(const uint8_t private_key[64], const uint8_t* msg, size_t msg_len,
                              uint8_t sig[64]) {
    if (sodium_init() < 0)
        return -1;
    return (crypto_sign_detached(sig, NULL, msg, msg_len, private_key) == 0) ? 0 : -1;
}

bool bramble_ed25519_impl_verify(const uint8_t public_key[32], const uint8_t* msg, size_t msg_len,
                                 const uint8_t sig[64]) {
    if (sodium_init() < 0)
        return false;
    return crypto_sign_verify_detached(sig, msg, msg_len, public_key) == 0;
}
