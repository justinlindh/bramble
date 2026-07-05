# Phase 0 report: Ed25519 signature primitive

Status: DONE. No wire/packet/behavior change; primitive + tests only.

## API added (components/crypto/include/crypto.h)

Constants:
- `BRAMBLE_ED25519_PUBKEY_SIZE 32`
- `BRAMBLE_ED25519_SECKEY_SIZE 64`
- `BRAMBLE_ED25519_SIG_SIZE 64`

Functions:
- `int crypto_ed25519_keypair(uint8_t public_key[32], uint8_t private_key[64]);` 0 ok, nonzero on entropy failure (fail closed, no key material written on failure).
- `int crypto_ed25519_sign(const uint8_t private_key[64], const uint8_t* msg, size_t msg_len, uint8_t sig[64]);` 0 ok.
- `bool crypto_ed25519_verify(const uint8_t public_key[32], const uint8_t* msg, size_t msg_len, const uint8_t sig[64]);` true iff valid.

Secret-key format is the libsodium 64-byte layout: seed (32) || public key (32). The host backend consumes only the seed half, so keys are portable between backends. The round-trip test asserts the `sk[32..63] == pk` invariant on generated keys.

## Backends

- Device (`components/crypto/crypto_esp.c`): espressif/libsodium managed component. `crypto_sign_seed_keypair` / `crypto_sign_detached` / `crypto_sign_verify_detached`, each behind an idempotent `sodium_init()` guard. Seed wiped with `mbedtls_platform_zeroize` after keygen.
- Host (`components/crypto/crypto_host.c`): OpenSSL `EVP_PKEY_ED25519` via `EVP_PKEY_new_raw_private_key` / `EVP_DigestSign` / `EVP_DigestVerify` one-shots. Seed wiped with `OPENSSL_cleanse`.

## Entropy-gated keygen seed (SEC-L1 preserved)

Device `crypto_ed25519_keypair` draws its 32-byte seed via `crypto_random(seed, 32)` (crypto_esp.c), which is `crypto_entropy_fill(buf, len, esp_random)` (crypto_esp.c `crypto_random`, components/crypto/crypto_entropy.c). That is the exact same gated source `crypto_generate_identity` uses for the X25519 key. When the gate is shut, `crypto_entropy_fill` zeroes the buffer and returns -1; the keypair function draws into a stack scratch buffer and returns -1 before touching the caller's key buffers (mirrors the `crypto_generate_identity` scratch-buffer pattern). Host `crypto_ed25519_keypair` uses the same `crypto_random()` call shape (RAND_bytes on host, which is ungated by design, matching the existing host `crypto_generate_identity` precedent).

## Dependency wiring

- New `components/crypto/idf_component.yml`: `espressif/libsodium: "^1.0.20"` (resolved to 1.0.22~1 from the component registry during the board build).
- `components/crypto/CMakeLists.txt`: `REQUIRES mbedtls espressif__libsodium`.

## Tests (test/test_ed25519.c, host suite; TDD red -> green)

Written first; confirmed failing (API absent, compile error), then implemented, then green.

Known-answer vectors, RFC 8032 Section 7.1 (sign must reproduce exact signature bytes AND verify must accept):
- TEST 1 (empty message)
- TEST 2 (1 byte, 0x72)
- TEST 3 (2 bytes, af82)
- TEST SHA(abc) (64-byte SHA-512 digest message)

Malleability / tampering:
- S' = S + L malleated signature REJECTED (RFC 8032 canonical-S rule; carry-checked little-endian add of the group order L to the S half; sanity-verifies the untampered sig first).
- Single flipped byte in R half and in S half each rejected.
- Round trip: keypair -> sign -> verify true; wrong public key false; tampered message false; distinct keypairs; pk not all-zeros; sk layout invariant.

Host==device parity: the vectors pin the host backend byte-for-byte to RFC 8032; libsodium's crypto_sign is an RFC 8032 implementation with the same detached-signature layout, and both backends enforce canonical S (libsodium sc25519 canonical check, OpenSSL s < L check), so the identical vector set pins both. There is no on-device test runner in this repo (host-only suite per test/run_all_tests.sh), so device conformance is pinned by construction (same vectors, standard libraries) rather than executed on target.

Documented divergence (deliberately not asserted): libsodium additionally rejects small-order public keys / R points; OpenSSL does not. This only differs for attacker-crafted keys. Later phases must not build logic that depends on accept/reject of small-order-point signatures; flagged as a standing concern for Phase 3 (verify+TOFU-pin).

## Board build linked libsodium

`make ci-quality-board-build` (heltec-v3): exit 0, component manager fetched `espressif/libsodium (1.0.22~1)` into managed_components/, compiled it, and linked bramble.elf. Verified concretely:
- `crypto_esp.c.obj` defines `crypto_ed25519_{keypair,sign,verify}` and has undefined refs to `crypto_sign_seed_keypair` / `crypto_sign_detached` / `crypto_sign_verify_detached` / `sodium_init` (proves the header/REQUIRES wiring compiled).
- `libespressif__libsodium.a` provides all four symbols (T), and the final link succeeded; ld resolves all undefined symbols of included objects before --gc-sections.
- The crypto_ed25519_* functions are GC'd from the final .elf because nothing calls them yet; that is the expected Phase 0 no-behavior-change state.

## CI (all green, run locally)

- Host suite: 97/97 suites (includes new test_ed25519: 7/7).
- Board build heltec-v3: exit 0 (re-run after formatting; still green).
- cppcheck (bramble/runner-full:22.04-go126 image, same flags as Makefile): clean, exit 0.
- clang-format v14 strict (runner image, worktree + main .git mounted): PASS, 370 files. Note: host clang-format 22 disagrees on pre-existing main/lv_malloc_psram.c; CI pins v14, untouched by this change.
- check-rpc-contract: OK, 53 methods (no RPC change).
- gosim: go build + go test -count=1 green (unaffected).

## Concerns

1. Small-order-point accept/reject divergence between OpenSSL (host) and libsodium (device), documented above; constrain Phase 3 verify semantics to honest-key inputs or add explicit small-order checks at that layer if it ever matters.
2. Flash cost: libsodium adds compile time but the unused crypto_sign code is currently GC'd; expect roughly 20-30 KB flash once Phase 1+ actually calls the primitive (app partition currently 56% free, no risk).
3. The managed-component fetch needs registry network access on first configure on CI runners; version range `^1.0.20` resolved to 1.0.22~1 here. If CI runners are offline, the component must be cached (same situation as the existing espressif/mdns and lvgl dependencies, so no new failure mode).
