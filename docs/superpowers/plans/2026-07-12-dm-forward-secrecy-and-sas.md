# DM Forward Secrecy and SAS Verification Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add per-message forward secrecy to Bramble DMs via a symmetric double-ratchet with a DH ratchet amortized onto `ke_epoch`, and ship the SAS verification UX (identity-bound safety number) on the pager e-paper and webapp surfaces, persisting verified state to NVS.

**Architecture:** A symmetric KDF ratchet (root + two directional chains) rides on top of the existing quad-DH handshake IKM; `RK_0` is bit-identical to today's `dm_session_key_from_ikm` output so epoch 0 is migration-continuous. A DH ratchet reuses the existing INIT/RESP handshake on an epoch schedule for coarse post-compromise recovery. Every session-keyed payload (chat DMs and directed LOCATION shares alike) carries a 3-byte cleartext ratchet header (epoch + message index) authenticated as AEAD associated data; the receiver reads the index directly, derives exactly that one message key (caching skipped keys, bounded by `DM_MAX_SKIP`), and does a single GCM decrypt, degrading into the existing desync-heal path (`maybe_trigger_dm_rehandshake`, commit `8ab55838`) when the index is beyond the skip bound. The SAS is redefined to commit to the two pinned X25519 identity keys (safety-number model), and verified state persists on the TOFU pin entry in NVS. Crypto lands first with heavy host-test gating (KAT vectors plus a mandatory nonce-uniqueness assertion) before any UX.

**Tech Stack:** C11 firmware (ESP-IDF, host tests via CMake + Unity + OpenSSL backend), `components/crypto` primitives only, LVGL pager UI (`components/ui_graphics`), React + TypeScript webapp (`webapp/`, Vitest), Playwright emulator E2E (`emulator/e2e`).

## Global Constraints

- **Crypto primitives:** use ONLY `components/crypto/include/crypto.h` (X25519 DH + low-order check, Ed25519, HKDF-SHA256, HMAC-SHA256/trunc4, AES-256-GCM, SHA-256, `crypto_random`). Introduce no new primitive. Every ratchet step is an HKDF-SHA256 call; every DH ratchet step is one X25519 call.
- **Hard flag day, no negotiation:** bump `BRAMBLE_VERSION` 4 to 5 (`components/packet/include/packet.h:19`). The ratchet is mandatory; there is no capability advertisement and therefore zero downgrade surface. Old v4 frames drop at the RX version gate (`bramble_header_is_supported_version`); existing sessions re-handshake once via the desync-heal path.
- **Both session-keyed payload types:** `dm_session_encrypt`/`dm_session_decrypt` back BOTH chat DMs (`send_dm_packet` at `main/mesh_task.c:4696`, `handle_data` session branch at `main/mesh_task.c:2445-2478`) AND directed LOCATION shares (encrypt at `main/mesh_task.c:661-675`, decrypt at `main/mesh_task.c:960-976`). Every task that changes session encryption MUST cover the LOCATION path, not only chat. The ratchet is applied through new `dm_session_ratchet_encrypt`/`dm_session_ratchet_decrypt` wrappers that BOTH call sites switch to.
- **Nonce discipline:** the GCM nonce stays the node-global monotonic counter (`nonce_counter_next`, under `s_nonce_mutex`). Message keys change per message. A host test MUST assert message keys change per message AND that no (key, nonce) pair ever repeats across a key change (the catastrophic GCM case). This assertion is mandatory and non-negotiable per review gate 5.
- **SAS commits to identity, not session:** the persisted verified bit keys on the identity-bound SAS (`HKDF(salt="bramble-sas-id", info=id_lo_x25519||id_hi_x25519)`), so ratchet steps, epoch bumps, desync-heal, and reboot do NOT force re-verification; only a pin key change does.
- **House rules:** NO em dashes anywhere (code, comments, commits, PR bodies; a hook rejects them). NO AI attribution in commits or PRs. Conventional-commit style; branch `feat/dm-forward-secrecy-sas`. Format C with the CI clang-format v14 runner, not the local binary: `docker run --rm -v "$PWD:/w" -w /w bramble/runner-full:22.04-go126 clang-format -i <files>`.
- **Gates (real exit codes, never `cmd | grep`):** firmware host tests `bash test/run_all_tests.sh`; board build `bash scripts/flash.sh local heltec_v4 build`; webapp `cd webapp && npm run lint && npm run test:unit && npm run build`; CI parity `make ci`.
- **SECURITY-MODEL.md honesty:** claims tie to mechanisms. State what closes (per-message FS for DMs, SAS UX on both surfaces) and what does NOT (PCS is coarse/epoch-bounded, network-key insider unchanged, first-contact TOFU window unchanged). State that the crypto is NOT yet independently reviewed: the five review gates in the spec are claimed-pending-review, NOT verified.

## Constants (chosen values, flagged where tunable)

- `DM_MAX_SKIP = 32` (forward derive bound AND skip-cache size). RAM cost of the ratchet state added to each of the 32 `dm_session_t` slots is roughly `rk(32) + send_chain(35) + recv_chain(35) + skip(32*35) + prev_epoch_recv(35) + prev_skip(32*35)` which is about 2.4 KB per session, roughly 77 KB total. **Flagged for the user/review:** this is the LoRa loss-tolerance vs RAM tradeoff (spec A.4 proposed 64, "a tuning constant, not a security boundary"). If 77 KB is too much on the S3, halve `DM_MAX_SKIP` or reduce `DM_MAX_SESSIONS`. Confirm before merge.
- `DM_EPOCH_GRACE_MSGS = DM_MAX_SKIP` (old-epoch receive chain retained until this many messages seen on the new epoch, then wiped for PCS).
- Message index is `uint16_t`, big-endian on the wire; per-session, per-direction, per-epoch. Exhausting it (65535) forces an epoch bump / re-handshake.

## Resolved spec ambiguity (read before Task 2/3)

The spec says the 3-byte header (epoch, msg_index) is "inside the AEAD plaintext ... an attacker cannot flip them without failing the GCM tag" but does not state HOW the receiver selects a message key before decrypting (if the index is encrypted, the receiver cannot read it until after it has already chosen a key). This plan resolves it with the standard Signal Double Ratchet construction: **the epoch+index ride in the CLEARTEXT header on the wire and are fed into the AEAD associated data (AAD)**, so they are authenticated (unforgeable) but not encrypted. The receiver reads `epoch||index` directly, derives exactly that one message key (walking the receive chain from `next` up to `index`, caching the skipped keys into `skip[]`, bounded by `DM_MAX_SKIP`), and performs a SINGLE GCM decrypt with the header in the AAD. There is no trial-decryption loop.

Why cleartext-index-in-AAD rather than encrypting the index: it is the standard, well-reviewed Double Ratchet layout (the message number is a cleartext header field authenticated by the AEAD), it is one GCM op instead of up to `DM_MAX_SKIP`, and it carries zero cryptographic novelty for a reviewer to reason about. The only cost is that the per-message epoch+index is visible on the wire as metadata. That is negligible here: a network-key insider already sees every DM frame's timing, size, and `src_addr` and can count a peer's messages regardless (the existing shared-key residual in SECURITY-MODEL). Because epoch+index sit in the AAD, an attacker cannot flip them without failing the GCM tag, so integrity is preserved. The bounded-skip DoS refusal is UNCHANGED: an index beyond `next + DM_MAX_SKIP` returns `DM_DECRYPT_TOO_FAR` and degrades to the desync-heal path (exactly the DoS bound the spec already accepts). Surfaced for the user and for review gates 1/3/5.

---

## File Structure

- `components/dm_session/dm_session.c` / `include/dm_session.h` : ratchet KDF (`dm_ratchet_init`, `dm_ratchet_step`, `dm_ratchet_dh`), the `dm_ratchet_t` runtime state embedded in `dm_session_t`, the `dm_session_ratchet_encrypt`/`_decrypt` wrappers, the identity SAS (`dm_derive_identity_sas`). One component, extended.
- `components/packet/include/packet.h` : `BRAMBLE_VERSION` 4 to 5; ratchet-header size define.
- `components/identity/identity_store.c` / `include/identity_store.h` : `verified` bit + `verified_sas[8]` on `identity_pin_t`; `identity_store_set_verified`/`identity_store_is_verified`; NVS serialize/deserialize.
- `components/nvs_keys/include/nvs_keys.h` : new NVS keys for the persisted pin store.
- `main/mesh_task.c` : switch both session payload paths to the ratchet wrappers; DH-ratchet epoch-bump trigger + bookkeeping; clear verified bit on `DM_VERIFY_ERR_PIN_MISMATCH`; snapshot verified bit into `dm_session_t.verified`; pin-store persistence load/save wiring.
- `main/rpc_methods.c` : `bramble.getPeerVerification` / `bramble.setPeerVerified` (authenticated).
- `components/ui_graphics/screens/scr_chat_messages.{c,h}` + a new `scr_sas_verify.{c,h}` : pager SAS UX.
- `webapp/src/pages/Chat/` : SAS verification panel, verified badge, key-change banner; `webapp/src/transport/` + store for the RPC.
- `test/` : new host tests `test_dm_ratchet.c`, `test_dm_ratchet_skip.c`, `test_dm_ratchet_epoch.c`, `test_dm_sas_identity.c`, `test_identity_store_persist.c`; extensions to `test_dm_session.c`, `test_ke_envelope.c`, `test_wire_version.c`.
- `emulator/e2e/specs/sas-verify.spec.ts` : two-pager SAS verification E2E.
- `docs/SECURITY-MODEL.md`, `README.md` : honest residual updates.

---

## Task 1: Ratchet key schedule (pure KDF) + KAT vectors + nonce-uniqueness gate

**Files:**
- Modify: `components/dm_session/dm_session.c` (add after `dm_session_key_from_ikm`, ~line 108), `components/dm_session/include/dm_session.h` (add after line 11)
- Test: `test/test_dm_ratchet.c` (create), `test/CMakeLists.txt` (register, mirror `test_dm_session` block at lines 64-73)

**Interfaces:**
- Consumes: existing static `dm_session_key_from_ikm` and `dm_build_info` in `dm_session.c`; `crypto_hkdf_sha256`, `crypto_x25519_dh` from `crypto.h`.
- Produces:
  - `int dm_ratchet_init(const uint8_t ikm[128], uint32_t addr_a, uint32_t addr_b, uint8_t rk_out[32], uint8_t ck_lohi_out[32], uint8_t ck_hilo_out[32]);` returns 0 on success. `rk_out` equals today's `dm_session_key_from_ikm(ikm, addr_a, addr_b, 0)`.
  - `void dm_ratchet_step(const uint8_t ck_in[32], uint16_t index_n, uint8_t mk_out[32], uint8_t ck_next_out[32]);` derives `mk_n` and `ck_{n+1}`.
  - `int dm_ratchet_dh(const uint8_t rk_e[32], const uint8_t dh[32], uint32_t addr_a, uint32_t addr_b, uint16_t new_epoch, uint8_t rk_next_out[32]);`
  - `#define DM_KEY_SIZE 32` (chain/message/root key size).

- [ ] **Step 1: Write the failing KAT test**

Create `test/test_dm_ratchet.c`. Pin the exact schedule against committed vectors computed from a fixed IKM (`ikm[i] = i`). The expected hex values are filled in Step 4 after the reference implementation runs once (write them as all-zero placeholders now so the test compiles and FAILS):

```c
#include "unity.h"
#include "dm_session.h"
#include "../components/crypto/crypto_host.c"
#include "../components/dm_session/dm_session.c"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* Canonical addresses: lo=0x11111111, hi=0x22222222 (lo < hi already ordered). */
#define ADDR_LO 0x11111111u
#define ADDR_HI 0x22222222u

static void fill_ikm(uint8_t ikm[128]) { for (int i = 0; i < 128; i++) ikm[i] = (uint8_t)i; }

/* RK_0 MUST equal today's session key for epoch 0 (migration continuity). */
void test_rk0_equals_legacy_session_key(void) {
    uint8_t ikm[128]; fill_ikm(ikm);
    uint8_t rk[32], ck_lohi[32], ck_hilo[32];
    TEST_ASSERT_EQUAL(0, dm_ratchet_init(ikm, ADDR_LO, ADDR_HI, rk, ck_lohi, ck_hilo));
    uint8_t legacy[32];
    TEST_ASSERT_EQUAL(0, dm_session_key_from_ikm(ikm, ADDR_LO, ADDR_HI, 0, legacy));
    TEST_ASSERT_EQUAL_MEMORY(legacy, rk, 32);
}

void test_ratchet_init_kat(void) {
    uint8_t ikm[128]; fill_ikm(ikm);
    uint8_t rk[32], ck_lohi[32], ck_hilo[32];
    TEST_ASSERT_EQUAL(0, dm_ratchet_init(ikm, ADDR_LO, ADDR_HI, rk, ck_lohi, ck_hilo));
    /* The two directional chains MUST differ (domain separation). */
    TEST_ASSERT_NOT_EQUAL(0, memcmp(ck_lohi, ck_hilo, 32));
    static const uint8_t k_ck_lohi[32] = {0}; /* FILL from Step 4 reference run */
    static const uint8_t k_ck_hilo[32] = {0}; /* FILL from Step 4 reference run */
    TEST_ASSERT_EQUAL_MEMORY(k_ck_lohi, ck_lohi, 32);
    TEST_ASSERT_EQUAL_MEMORY(k_ck_hilo, ck_hilo, 32);
}

/* Message keys change every message; the chain advances; mk_n != mk_{n+1}. */
void test_chain_advances_and_mk_changes(void) {
    uint8_t ikm[128]; fill_ikm(ikm);
    uint8_t rk[32], ck[32], ck_hilo[32];
    TEST_ASSERT_EQUAL(0, dm_ratchet_init(ikm, ADDR_LO, ADDR_HI, rk, ck, ck_hilo));
    uint8_t prev_mk[32] = {0};
    uint8_t seen_mk[8][32];
    for (uint16_t n = 0; n < 8; n++) {
        uint8_t mk[32], ck_next[32];
        dm_ratchet_step(ck, n, mk, ck_next);
        TEST_ASSERT_NOT_EQUAL(0, memcmp(mk, ck, 32));       /* mk != chain key */
        TEST_ASSERT_NOT_EQUAL(0, memcmp(ck_next, ck, 32));  /* chain advanced */
        if (n > 0) TEST_ASSERT_NOT_EQUAL(0, memcmp(mk, prev_mk, 32));
        memcpy(prev_mk, mk, 32);
        memcpy(seen_mk[n], mk, 32);
        memcpy(ck, ck_next, 32);
    }
    /* All eight message keys are pairwise distinct. */
    for (int i = 0; i < 8; i++)
        for (int j = i + 1; j < 8; j++)
            TEST_ASSERT_NOT_EQUAL(0, memcmp(seen_mk[i], seen_mk[j], 32));
}

/* MANDATORY (review gate 5): no (message key, nonce) pair repeats across a key
 * change. Simulate the sender: a monotonic counter nonce alongside a chain that
 * advances every message. Assert every (mk||nonce) pair is unique, so a GCM
 * key+nonce reuse (the catastrophic case) can never arise. */
void test_no_key_nonce_reuse(void) {
    uint8_t ikm[128]; fill_ikm(ikm);
    uint8_t rk[32], ck[32], ck_hilo[32];
    TEST_ASSERT_EQUAL(0, dm_ratchet_init(ikm, ADDR_LO, ADDR_HI, rk, ck, ck_hilo));
    uint8_t pairs[16][44]; /* 32 key + 12 nonce */
    uint64_t nonce_ctr = 0xABCDEF00ull;
    for (uint16_t n = 0; n < 16; n++) {
        uint8_t mk[32], ck_next[32];
        dm_ratchet_step(ck, n, mk, ck_next);
        memcpy(pairs[n], mk, 32);
        for (int b = 0; b < 12; b++) pairs[n][32 + b] = (uint8_t)(nonce_ctr >> (8 * b));
        nonce_ctr++;                 /* monotonic, never reused */
        memcpy(ck, ck_next, 32);
    }
    for (int i = 0; i < 16; i++)
        for (int j = i + 1; j < 16; j++)
            TEST_ASSERT_NOT_EQUAL(0, memcmp(pairs[i], pairs[j], 44));
}

/* DH ratchet: a fresh DH advances the root; RK_{e+1} != RK_e and is a pure
 * function of (RK_e, dh, epoch). */
void test_dh_ratchet_advances_root(void) {
    uint8_t ikm[128]; fill_ikm(ikm);
    uint8_t rk0[32], ck_lohi[32], ck_hilo[32];
    TEST_ASSERT_EQUAL(0, dm_ratchet_init(ikm, ADDR_LO, ADDR_HI, rk0, ck_lohi, ck_hilo));
    uint8_t dh[32]; memset(dh, 0x5A, 32);
    uint8_t rk1a[32], rk1b[32];
    TEST_ASSERT_EQUAL(0, dm_ratchet_dh(rk0, dh, ADDR_LO, ADDR_HI, 1, rk1a));
    TEST_ASSERT_EQUAL(0, dm_ratchet_dh(rk0, dh, ADDR_LO, ADDR_HI, 1, rk1b));
    TEST_ASSERT_EQUAL_MEMORY(rk1a, rk1b, 32);          /* deterministic */
    TEST_ASSERT_NOT_EQUAL(0, memcmp(rk1a, rk0, 32));   /* advanced */
    static const uint8_t k_rk1[32] = {0};              /* FILL from Step 4 */
    TEST_ASSERT_EQUAL_MEMORY(k_rk1, rk1a, 32);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_rk0_equals_legacy_session_key);
    RUN_TEST(test_ratchet_init_kat);
    RUN_TEST(test_chain_advances_and_mk_changes);
    RUN_TEST(test_no_key_nonce_reuse);
    RUN_TEST(test_dh_ratchet_advances_root);
    return UNITY_END();
}
```

Register in `test/CMakeLists.txt` (copy the `test_dm_session` block at lines 64-73, renamed):

```cmake
add_executable(test_dm_ratchet
    test_dm_ratchet.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/packet/packet.c
)
target_include_directories(test_dm_ratchet PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dm_session/include
)
target_link_libraries(test_dm_ratchet ssl crypto)
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd test/build && cmake .. >/dev/null && make test_dm_ratchet 2>&1 | tail -5`
Expected: FAIL to link/compile with `undefined reference to 'dm_ratchet_init'` (functions not yet defined).

- [ ] **Step 3: Write the minimal implementation**

Add to `components/dm_session/include/dm_session.h` after line 11:

```c
#define DM_KEY_SIZE 32
int dm_ratchet_init(const uint8_t ikm[128], uint32_t addr_a, uint32_t addr_b, uint8_t rk_out[32],
                    uint8_t ck_lohi_out[32], uint8_t ck_hilo_out[32]);
void dm_ratchet_step(const uint8_t ck_in[32], uint16_t index_n, uint8_t mk_out[32],
                     uint8_t ck_next_out[32]);
int dm_ratchet_dh(const uint8_t rk_e[32], const uint8_t dh[32], uint32_t addr_a, uint32_t addr_b,
                  uint16_t new_epoch, uint8_t rk_next_out[32]);
```

Add to `components/dm_session/dm_session.c` after `dm_session_key_from_ikm` (line 108). `dm_build_info` and `dm_session_key_from_ikm` are static and already in this file:

```c
int dm_ratchet_init(const uint8_t ikm[128], uint32_t addr_a, uint32_t addr_b, uint8_t rk_out[32],
                    uint8_t ck_lohi_out[32], uint8_t ck_hilo_out[32]) {
    /* RK_0 IS the legacy epoch-0 session key: migration continuity (spec A.6). */
    if (dm_session_key_from_ikm(ikm, addr_a, addr_b, 0, rk_out) != 0)
        return -1;
    const char* lohi = "bramble-dm-chain-lohi";
    const char* hilo = "bramble-dm-chain-hilo";
    if (crypto_hkdf_sha256(rk_out, 32, NULL, 0, (const uint8_t*)lohi, strlen(lohi), ck_lohi_out,
                           32) != 0)
        return -1;
    if (crypto_hkdf_sha256(rk_out, 32, NULL, 0, (const uint8_t*)hilo, strlen(hilo), ck_hilo_out,
                           32) != 0)
        return -1;
    return 0;
}

void dm_ratchet_step(const uint8_t ck_in[32], uint16_t index_n, uint8_t mk_out[32],
                     uint8_t ck_next_out[32]) {
    uint8_t mk_info[16];
    const char* mk_label = "bramble-dm-mk";
    size_t ll = strlen(mk_label);
    memcpy(mk_info, mk_label, ll);
    mk_info[ll] = (uint8_t)(index_n >> 8);
    mk_info[ll + 1] = (uint8_t)(index_n & 0xFF);
    (void)crypto_hkdf_sha256(ck_in, 32, NULL, 0, mk_info, ll + 2, mk_out, 32);
    const char* ck_label = "bramble-dm-ck";
    (void)crypto_hkdf_sha256(ck_in, 32, NULL, 0, (const uint8_t*)ck_label, strlen(ck_label),
                             ck_next_out, 32);
}

int dm_ratchet_dh(const uint8_t rk_e[32], const uint8_t dh[32], uint32_t addr_a, uint32_t addr_b,
                  uint16_t new_epoch, uint8_t rk_next_out[32]) {
    uint8_t info[10];
    dm_build_info(addr_a, addr_b, new_epoch, info);
    return crypto_hkdf_sha256(rk_e, 32, dh, 32, info, sizeof(info), rk_next_out, 32);
}
```

- [ ] **Step 4: Fill the KAT vectors, then run to verify it passes**

Temporarily add a debug print (or run under a tiny throwaway `main`) to dump `ck_lohi`, `ck_hilo`, and `rk1` as hex, paste the real bytes into the `k_ck_lohi`/`k_ck_hilo`/`k_rk1` arrays in the test, remove the debug print. This is the standard "compute the vector once, commit it" pattern (`test/test_dm_session.c:107-119` `test_sas_known_vector` does exactly this).

Run: `cd test/build && cmake .. >/dev/null && make test_dm_ratchet 2>&1 | tail -3 && ./test_dm_ratchet`
Expected: `5 Tests 0 Failures 0 Ignored` / `OK`.

- [ ] **Step 5: Format and commit**

```bash
docker run --rm -v "$PWD:/w" -w /w bramble/runner-full:22.04-go126 clang-format -i components/dm_session/dm_session.c components/dm_session/include/dm_session.h test/test_dm_ratchet.c
git add components/dm_session/ test/test_dm_ratchet.c test/CMakeLists.txt
git commit -m "feat(dm): add symmetric ratchet key schedule with committed KAT vectors"
```

---

## Task 2: Wire format, version flag day, sender-side ratchet encrypt

**Files:**
- Modify: `components/packet/include/packet.h:19` (`BRAMBLE_VERSION`), add a header-size define near line 390
- Modify: `components/dm_session/dm_session.c` / `include/dm_session.h` (add `dm_ratchet_t`, extend `dm_session_t`, add `dm_session_ratchet_encrypt`)
- Test: `test/test_dm_ratchet.c` (extend), `test/test_wire_version.c` (bump expected version)

**Interfaces:**
- Consumes: `dm_ratchet_init`, `dm_ratchet_step` (Task 1); `dm_session_encrypt` (existing, `dm_session.c:493`); `bramble_build_aead_aad`.
- Produces:
  - New wire define `#define DM_RATCHET_HEADER_SIZE 3` in `packet.h`.
  - `dm_ratchet_t` struct (see below) and new fields on `dm_session_t`.
  - `int dm_session_ratchet_encrypt(dm_session_t* s, const bramble_header_t* h, uint32_t src_addr, const uint8_t* pt, size_t pt_len, const uint8_t nonce[12], uint8_t* framed_ct_out, uint8_t* tag_out, size_t* framed_len_out);` writes the 3-byte header (`epoch||msg_index`) in the CLEARTEXT as the first bytes of `framed_ct_out`, appends those 3 bytes to the AEAD AAD, encrypts ONLY the payload under the next send message key into the bytes after the header, advances the send chain, returns `framed_len_out = DM_RATCHET_HEADER_SIZE + pt_len` (`cleartext header || ciphertext`).
  - `void dm_session_ratchet_init_state(dm_session_t* s, const uint8_t ikm[128], uint32_t addr_self, uint32_t addr_peer);` and a non-static `dm_compute_ikm` (see below).

The 3-byte header layout is `epoch(1) || msg_index(2, big-endian)`, matching the low byte of `ke_epoch` (the handshake `key_id`) and the per-direction index. On the wire it sits in the CLEARTEXT immediately after the 12-byte nonce and before the ciphertext (`nonce(12) || header(3) || ciphertext(pt_len) || tag(16)`) and is appended to the AEAD AAD so it is authenticated but not encrypted. `framed_ct_out` holds `header(3) || ciphertext`; the receiver reads the header before decrypting.

- [ ] **Step 1: Write the failing test**

Extend `test/test_dm_ratchet.c` (add these tests + `RUN_TEST`s):

```c
#include "packet.h"

/* The framed ciphertext is exactly 3 bytes longer than the plaintext, and the
 * send index advances. */
void test_ratchet_encrypt_frames_header(void) {
    bramble_identity_t a, b, a_eph, b_eph;
    crypto_generate_identity(&a); crypto_generate_identity(&b);
    crypto_generate_identity(&a_eph); crypto_generate_identity(&b_eph);
    uint8_t ikm[128];
    TEST_ASSERT_EQUAL(0, dm_compute_ikm(a.private_key, a_eph.private_key, b.public_key,
                                        b_eph.public_key, ikm));
    dm_session_t s; memset(&s, 0, sizeof(s));
    s.peer_addr = b.address;
    s.state = DM_STATE_ACTIVE;
    dm_session_ratchet_init_state(&s, ikm, a.address, b.address);

    bramble_header_t h = {0};
    h.version = BRAMBLE_VERSION; h.type = PKT_TYPE_DATA; h.flags = FLAG_ENCRYPT;
    h.hop_limit = 8; h.dest_addr = b.address; h.packet_id = 0x1234;
    const uint8_t pt[] = "hi";
    uint8_t nonce[12]; memset(nonce, 0x01, 12);
    uint8_t ct[DM_RATCHET_HEADER_SIZE + sizeof(pt)]; uint8_t tag[16]; size_t flen = 0;
    TEST_ASSERT_EQUAL(0, dm_session_ratchet_encrypt(&s, &h, a.address, pt, sizeof(pt), nonce, ct,
                                                    tag, &flen));
    TEST_ASSERT_EQUAL(DM_RATCHET_HEADER_SIZE + sizeof(pt), flen);
    TEST_ASSERT_EQUAL_UINT16(1, s.ratchet.send.index); /* advanced 0 -> 1 */
}

/* Two encrypts use two distinct message keys (the chain advanced), so the same
 * plaintext + reused nonce yields a different tag: proves per-message keying. */
void test_ratchet_encrypt_advances_key(void) {
    bramble_identity_t a, b, a_eph, b_eph;
    crypto_generate_identity(&a); crypto_generate_identity(&b);
    crypto_generate_identity(&a_eph); crypto_generate_identity(&b_eph);
    uint8_t ikm[128];
    TEST_ASSERT_EQUAL(0, dm_compute_ikm(a.private_key, a_eph.private_key, b.public_key,
                                        b_eph.public_key, ikm));
    dm_session_t s; memset(&s, 0, sizeof(s));
    s.peer_addr = b.address; s.state = DM_STATE_ACTIVE;
    dm_session_ratchet_init_state(&s, ikm, a.address, b.address);
    bramble_header_t h = {0};
    h.version = BRAMBLE_VERSION; h.type = PKT_TYPE_DATA; h.flags = FLAG_ENCRYPT;
    h.hop_limit = 8; h.dest_addr = b.address; h.packet_id = 0x1234;
    const uint8_t pt[] = "same";
    uint8_t nonce[12]; memset(nonce, 0x09, 12);
    uint8_t c0[DM_RATCHET_HEADER_SIZE + sizeof(pt)], c1[DM_RATCHET_HEADER_SIZE + sizeof(pt)];
    uint8_t t0[16], t1[16]; size_t f0, f1;
    dm_session_ratchet_encrypt(&s, &h, a.address, pt, sizeof(pt), nonce, c0, t0, &f0);
    dm_session_ratchet_encrypt(&s, &h, a.address, pt, sizeof(pt), nonce, c1, t1, &f1);
    TEST_ASSERT_NOT_EQUAL(0, memcmp(t0, t1, 16)); /* different key -> different tag */
}
```

Add to `main()`: `RUN_TEST(test_ratchet_encrypt_frames_header); RUN_TEST(test_ratchet_encrypt_advances_key);`.

In `test/test_wire_version.c`, bump the expected supported version constant from 4 to 5 (find the `TEST_ASSERT_EQUAL(4, ...)` / `BRAMBLE_VERSION` assertions and update them to 5).

- [ ] **Step 2: Run to verify it fails**

Run: `cd test/build && cmake .. >/dev/null && make test_dm_ratchet 2>&1 | tail -5`
Expected: FAIL, `undefined reference to 'dm_session_ratchet_encrypt'` / `dm_session_ratchet_init_state` / `dm_compute_ikm`, and `'dm_session_t' has no member named 'ratchet'`.

- [ ] **Step 3: Write the minimal implementation**

In `components/packet/include/packet.h`, line 19:

```c
/* was 4; DM forward-secrecy flag day: DM/LOCATION session payloads now carry a
 * 3-byte cleartext ratchet header (authenticated via the AEAD AAD) and are keyed
 * by a per-message ratchet. Old v4 session frames drop at the RX version gate and
 * re-handshake once. */
#define BRAMBLE_VERSION 5
```

Near line 390 (with the other `BRAMBLE_DATA_*` defines):

```c
/* Ratchet header carried in the CLEARTEXT of DM/LOCATION session frames, on the
 * wire between the nonce and the ciphertext, and authenticated via the AEAD AAD:
 * epoch(1) || msg_index(2, big-endian). Read before decrypt. */
#define DM_RATCHET_HEADER_SIZE 3
```

In `components/dm_session/include/dm_session.h`, add the ratchet state types before `dm_session_t` (line 122) and new members inside it:

```c
#define DM_MAX_SKIP 32

typedef struct {
    uint8_t ck[32];   /* current chain key */
    uint16_t index;   /* next index to send / next expected to receive */
    uint8_t epoch;    /* low byte of ke_epoch this chain belongs to */
    uint8_t valid;    /* 0 until dm_session_ratchet_init_state establishes it */
} dm_chain_t;

typedef struct {
    uint16_t index;
    uint8_t mk[32];
    uint8_t used;
} dm_skip_entry_t;

typedef struct {
    uint8_t rk[32];
    dm_chain_t send;
    dm_chain_t recv;
    dm_skip_entry_t skip[DM_MAX_SKIP];
    /* Previous-epoch receive retention during the DH-ratchet grace (Task 4). */
    dm_chain_t prev_recv;
    dm_skip_entry_t prev_skip[DM_MAX_SKIP];
    uint16_t new_epoch_msgs;   /* messages seen on the new epoch; grace expiry */
} dm_ratchet_t;
```

Add `dm_ratchet_t ratchet;` as a member of `dm_session_t` (after `verified`, line 136). Also expose `dm_compute_ikm` (the mesh DH-ratchet path in Task 4 needs it linked, and Tasks 3/4 host tests use it): change its `static` at `dm_session.c:53` to non-static and declare it in the header:

```c
int dm_compute_ikm(const uint8_t my_id_priv[32], const uint8_t my_eph_priv[32],
                   const uint8_t peer_id_pub[32], const uint8_t peer_eph_pub[32],
                   uint8_t ikm_out[128]);
void dm_session_ratchet_init_state(dm_session_t* s, const uint8_t ikm[128], uint32_t addr_self,
                                   uint32_t addr_peer);
int dm_session_ratchet_encrypt(dm_session_t* s, const bramble_header_t* h, uint32_t src_addr,
                               const uint8_t* pt, size_t pt_len, const uint8_t nonce[12],
                               uint8_t* framed_ct_out, uint8_t* tag_out, size_t* framed_len_out);
```

Implementation in `components/dm_session/dm_session.c` (after the ratchet KDF from Task 1). Direction: the sender's chain is the one whose label matches the sender's position in `addr_lo/addr_hi` order:

```c
void dm_session_ratchet_init_state(dm_session_t* s, const uint8_t ikm[128], uint32_t addr_self,
                                   uint32_t addr_peer) {
    uint32_t addr_a = addr_self < addr_peer ? addr_self : addr_peer;
    uint32_t addr_b = addr_self < addr_peer ? addr_peer : addr_self;
    uint8_t ck_lohi[32], ck_hilo[32];
    dm_ratchet_init(ikm, addr_a, addr_b, s->ratchet.rk, ck_lohi, ck_hilo);
    int self_is_lo = (addr_self == addr_a); /* lo sends lohi, receives hilo */
    memcpy(s->ratchet.send.ck, self_is_lo ? ck_lohi : ck_hilo, 32);
    memcpy(s->ratchet.recv.ck, self_is_lo ? ck_hilo : ck_lohi, 32);
    s->ratchet.send.index = 0;
    s->ratchet.recv.index = 0;
    s->ratchet.send.epoch = (uint8_t)(s->ke_epoch & 0xFF);
    s->ratchet.recv.epoch = (uint8_t)(s->ke_epoch & 0xFF);
    s->ratchet.send.valid = 1;
    s->ratchet.recv.valid = 1;
    memset(s->ratchet.skip, 0, sizeof(s->ratchet.skip));
    memset(&s->ratchet.prev_recv, 0, sizeof(s->ratchet.prev_recv));
    memset(s->ratchet.prev_skip, 0, sizeof(s->ratchet.prev_skip));
    s->ratchet.new_epoch_msgs = 0;
    /* Retain the legacy static session_key as RK_0 provenance only; the ratchet
     * chains are authoritative for every message now. */
    memcpy(s->session_key, s->ratchet.rk, 32);
    memset(ck_lohi, 0, 32); memset(ck_hilo, 0, 32);
}

int dm_session_ratchet_encrypt(dm_session_t* s, const bramble_header_t* h, uint32_t src_addr,
                               const uint8_t* pt, size_t pt_len, const uint8_t nonce[12],
                               uint8_t* framed_ct_out, uint8_t* tag_out, size_t* framed_len_out) {
    if (!s->ratchet.send.valid)
        return -1;
    if (pt_len > 255)
        return -1;
    uint8_t mk[32], ck_next[32];
    dm_ratchet_step(s->ratchet.send.ck, s->ratchet.send.index, mk, ck_next);

    /* Cleartext ratchet header: epoch || msg_index (big-endian). */
    uint8_t hdr[DM_RATCHET_HEADER_SIZE];
    hdr[0] = s->ratchet.send.epoch;
    hdr[1] = (uint8_t)(s->ratchet.send.index >> 8);
    hdr[2] = (uint8_t)(s->ratchet.send.index & 0xFF);

    /* AAD = base packet AAD || the 3 cleartext header bytes. The header is
     * authenticated (an attacker cannot flip epoch/index without failing the
     * GCM tag) but NOT encrypted. */
    uint8_t aad[HEADER_SIZE + 4 + DM_RATCHET_HEADER_SIZE];
    if (bramble_build_aead_aad(h, src_addr, aad, HEADER_SIZE + 4) != ESP_OK)
        return -1;
    memcpy(aad + HEADER_SIZE + 4, hdr, DM_RATCHET_HEADER_SIZE);

    /* Encrypt ONLY the payload; the header stays in the clear at the front of
     * the frame, ahead of the ciphertext. */
    int rc = crypto_aes256gcm_encrypt(mk, nonce, pt, pt_len, aad, sizeof(aad),
                                      framed_ct_out + DM_RATCHET_HEADER_SIZE, tag_out);
    if (rc == 0) {
        memcpy(framed_ct_out, hdr, DM_RATCHET_HEADER_SIZE);
        memcpy(s->ratchet.send.ck, ck_next, 32); /* advance; old ck overwritten */
        s->ratchet.send.index++;
        *framed_len_out = DM_RATCHET_HEADER_SIZE + pt_len;
    }
    memset(mk, 0, sizeof(mk));
    memset(ck_next, 0, sizeof(ck_next));
    return rc;
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `cd test/build && cmake .. >/dev/null && make test_dm_ratchet test_wire_version 2>&1 | tail -3 && ./test_dm_ratchet && ./test_wire_version`
Expected: both `OK`.

- [ ] **Step 5: Format and commit**

```bash
docker run --rm -v "$PWD:/w" -w /w bramble/runner-full:22.04-go126 clang-format -i components/dm_session/dm_session.c components/dm_session/include/dm_session.h components/packet/include/packet.h test/test_dm_ratchet.c
git add components/ test/test_dm_ratchet.c test/test_wire_version.c
git commit -m "feat(dm): bump wire to v5, add ratchet header framing and sender encrypt"
```

---

## Task 3: Receiver skip/out-of-order window (cleartext index, single decrypt) + desync degrade

**Files:**
- Modify: `components/dm_session/dm_session.c` / `include/dm_session.h` (add `dm_session_ratchet_decrypt`)
- Test: `test/test_dm_ratchet_skip.c` (create), `test/CMakeLists.txt` (register)

**Interfaces:**
- Consumes: `dm_ratchet_step` (Task 1); `dm_session_ratchet_encrypt` + `dm_session_ratchet_init_state` + `dm_compute_ikm` (Task 2); `crypto_aes256gcm_decrypt`.
- Produces:
  - `#define DM_DECRYPT_OK 0`, `#define DM_DECRYPT_FAIL -1`, `#define DM_DECRYPT_REPLAY -2`, `#define DM_DECRYPT_TOO_FAR -3`.
  - `int dm_session_ratchet_decrypt(dm_session_t* s, const bramble_header_t* h, uint32_t src_addr, const uint8_t nonce[12], const uint8_t* framed_ct, size_t framed_ct_len, const uint8_t* tag, uint8_t* pt_out, size_t* pt_len_out);` reads the cleartext 3-byte header (`framed_ct` is `header || ciphertext`), derives exactly the one message key the index names, does a single GCM decrypt with the header in the AAD, and returns `DM_DECRYPT_OK` with only the payload in `pt_out`; `DM_DECRYPT_TOO_FAR`/`DM_DECRYPT_FAIL` are the signals the mesh caller maps to the existing decrypt-failure -> `maybe_trigger_dm_rehandshake` path; `DM_DECRYPT_REPLAY` drops silently.

Receive policy (spec A.4, current epoch shown; the previous-epoch grace pass is added in Task 4 and reuses this same walk against `prev_recv`/`prev_skip`). The receiver reads `epoch||index` from the cleartext header FIRST, so key selection is known up front and there is NO trial-decryption loop:
- If `index < next` (out-of-order straggler): look the index up in the skip cache. On a hit, do ONE decrypt with that cached `mk`; on success evict the entry (single-use) and return. A miss (not cached) returns `DM_DECRYPT_TOO_FAR` (the mesh caller runs the nonce-window replay defense before this layer; see the implementer note).
- If `next <= index <= next + DM_MAX_SKIP`: walk the receive chain from `next` up to `index`, caching each skipped `mk` for indices `[next .. index-1]` (LRU into `skip[]`), derive the message key AT `index`, and do ONE GCM decrypt with the header in the AAD. On success set `next = index + 1`, commit the cached skipped keys, advance the stored chain, and return the payload. On GCM failure (forged or wrong-epoch frame) leave chain/skip state untouched and return `DM_DECRYPT_FAIL`.
- If `index > next + DM_MAX_SKIP`: return `DM_DECRYPT_TOO_FAR` WITHOUT deriving anything (bounded work; the DoS bound). The mesh caller degrades this into `maybe_trigger_dm_rehandshake`.

- [ ] **Step 1: Write the failing tests**

Create `test/test_dm_ratchet_skip.c`:

```c
#include "unity.h"
#include "dm_session.h"
#include "packet.h"
#include "../components/crypto/crypto_host.c"
#include "../components/dm_session/dm_session.c"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* Build two sessions (A sends, B receives) sharing one handshake IKM. */
static void make_pair(dm_session_t* sa, dm_session_t* sb, bramble_identity_t* a,
                      bramble_identity_t* b) {
    bramble_identity_t a_eph, b_eph;
    crypto_generate_identity(a); crypto_generate_identity(b);
    crypto_generate_identity(&a_eph); crypto_generate_identity(&b_eph);
    uint8_t ikm[128];
    dm_compute_ikm(a->private_key, a_eph.private_key, b->public_key, b_eph.public_key, ikm);
    memset(sa, 0, sizeof(*sa)); memset(sb, 0, sizeof(*sb));
    sa->peer_addr = b->address; sa->state = DM_STATE_ACTIVE;
    sb->peer_addr = a->address; sb->state = DM_STATE_ACTIVE;
    dm_session_ratchet_init_state(sa, ikm, a->address, b->address);
    dm_session_ratchet_init_state(sb, ikm, b->address, a->address);
}

static bramble_header_t hdr(uint32_t dst) {
    bramble_header_t h = {0};
    h.version = BRAMBLE_VERSION; h.type = PKT_TYPE_DATA; h.flags = FLAG_ENCRYPT;
    h.hop_limit = 8; h.dest_addr = dst; h.packet_id = 0xC0DE;
    return h;
}

/* One frame: A encrypts, B decrypts, header stripped, plaintext exact. */
void test_in_order_roundtrip(void) {
    dm_session_t sa, sb; bramble_identity_t a, b; make_pair(&sa, &sb, &a, &b);
    bramble_header_t h = hdr(b.address);
    const uint8_t pt[] = "message zero";
    uint8_t nonce[12]; memset(nonce, 0x01, 12);
    uint8_t ct[DM_RATCHET_HEADER_SIZE + sizeof(pt)]; uint8_t tag[16]; size_t flen;
    TEST_ASSERT_EQUAL(0, dm_session_ratchet_encrypt(&sa, &h, a.address, pt, sizeof(pt), nonce, ct,
                                                    tag, &flen));
    uint8_t out[64]; size_t olen;
    TEST_ASSERT_EQUAL(DM_DECRYPT_OK, dm_session_ratchet_decrypt(&sb, &h, a.address, nonce, ct, flen,
                                                               tag, out, &olen));
    TEST_ASSERT_EQUAL(sizeof(pt), olen);
    TEST_ASSERT_EQUAL_MEMORY(pt, out, sizeof(pt));
}

/* Deliver 0,1,2,3 out of order (2,0,3,1), all within MAX_SKIP: all decrypt. */
void test_out_of_order_within_bound(void) {
    dm_session_t sa, sb; bramble_identity_t a, b; make_pair(&sa, &sb, &a, &b);
    bramble_header_t h = hdr(b.address);
    uint8_t ct[4][DM_RATCHET_HEADER_SIZE + 8]; uint8_t tag[4][16]; size_t flen[4];
    uint8_t nonce[4][12];
    for (int i = 0; i < 4; i++) {
        memset(nonce[i], 0x10 + i, 12);
        uint8_t pt[8]; memset(pt, 'a' + i, 8);
        dm_session_ratchet_encrypt(&sa, &h, a.address, pt, 8, nonce[i], ct[i], tag[i], &flen[i]);
    }
    int order[4] = {2, 0, 3, 1};
    for (int k = 0; k < 4; k++) {
        int i = order[k];
        uint8_t out[16]; size_t olen;
        TEST_ASSERT_EQUAL(DM_DECRYPT_OK, dm_session_ratchet_decrypt(&sb, &h, a.address, nonce[i],
                                                                   ct[i], flen[i], tag[i], out,
                                                                   &olen));
        TEST_ASSERT_EQUAL(8, olen);
        uint8_t exp[8]; memset(exp, 'a' + i, 8);
        TEST_ASSERT_EQUAL_MEMORY(exp, out, 8);
    }
}

/* A jumps to index next+DM_MAX_SKIP+1 (one past the bound): B refuses
 * (DM_DECRYPT_TOO_FAR) without deriving anything, no crash. This is the DoS
 * bound and the desync-heal trigger. The tested frame is the LAST one encrypted,
 * whose cleartext index is DM_MAX_SKIP+1 while B's next is still 0. */
void test_beyond_max_skip_refused(void) {
    dm_session_t sa, sb; bramble_identity_t a, b; make_pair(&sa, &sb, &a, &b);
    bramble_header_t h = hdr(b.address);
    uint8_t ct[DM_RATCHET_HEADER_SIZE + 4]; uint8_t tag[16]; size_t flen; uint8_t nonce[12];
    for (int i = 0; i <= DM_MAX_SKIP + 1; i++) { /* indices 0 .. DM_MAX_SKIP+1 */
        memset(nonce, 0x20 + (i & 0x1F), 12);
        uint8_t pt[4] = {1, 2, 3, 4};
        dm_session_ratchet_encrypt(&sa, &h, a.address, pt, 4, nonce, ct, tag, &flen);
    }
    uint8_t out[16]; size_t olen;
    TEST_ASSERT_EQUAL(DM_DECRYPT_TOO_FAR, dm_session_ratchet_decrypt(&sb, &h, a.address, nonce, ct,
                                                                    flen, tag, out, &olen));
    TEST_ASSERT_EQUAL_UINT16(0, sb.ratchet.recv.index); /* recv chain not advanced past bound */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_in_order_roundtrip);
    RUN_TEST(test_out_of_order_within_bound);
    RUN_TEST(test_beyond_max_skip_refused);
    return UNITY_END();
}
```

Register `test_dm_ratchet_skip` in `test/CMakeLists.txt` (copy the Task 1 block, rename).

- [ ] **Step 2: Run to verify it fails**

Run: `cd test/build && cmake .. >/dev/null && make test_dm_ratchet_skip 2>&1 | tail -5`
Expected: FAIL, `undefined reference to 'dm_session_ratchet_decrypt'` and the `DM_DECRYPT_*` macros undefined.

- [ ] **Step 3: Write the minimal implementation**

Add the return-code defines to `include/dm_session.h` and the declaration:

```c
#define DM_DECRYPT_OK 0
#define DM_DECRYPT_FAIL (-1)
#define DM_DECRYPT_REPLAY (-2)
#define DM_DECRYPT_TOO_FAR (-3)
int dm_session_ratchet_decrypt(dm_session_t* s, const bramble_header_t* h, uint32_t src_addr,
                               const uint8_t nonce[12], const uint8_t* framed_ct,
                               size_t framed_ct_len, const uint8_t* tag, uint8_t* pt_out,
                               size_t* pt_len_out);
```

Implement in `dm_session.c`. The index is known from the cleartext header, so the walk derives exactly the one target key (caching the skipped keys it passes) and does a single GCM decrypt over the ciphertext (which no longer contains the header):

```c
/* Walk one receive chain (chain/skip pair) to the KNOWN cleartext index and do a
 * single decrypt. DM_DECRYPT_OK on success (updates chain + skip); DM_DECRYPT_FAIL
 * if the one derived key does not authenticate (forged / wrong-epoch frame, chain
 * left untouched); DM_DECRYPT_TOO_FAR if index is beyond [next .. next+DM_MAX_SKIP]
 * or is an already-consumed straggler not in the skip cache. */
static int dm_recv_walk(dm_chain_t* chain, dm_skip_entry_t* skip, uint16_t index,
                        const uint8_t* aad, size_t aad_len, const uint8_t nonce[12],
                        const uint8_t* ct, size_t ct_len, const uint8_t* tag, uint8_t* pt_out,
                        size_t* pt_len_out) {
    /* 1) straggler behind the cursor: only the skip cache can hold its key. */
    if (index < chain->index) {
        for (int i = 0; i < DM_MAX_SKIP; i++) {
            if (skip[i].used && skip[i].index == index) {
                if (crypto_aes256gcm_decrypt(skip[i].mk, nonce, ct, ct_len, aad, aad_len, tag,
                                             pt_out) != 0)
                    return DM_DECRYPT_FAIL;
                *pt_len_out = ct_len;
                memset(&skip[i], 0, sizeof(skip[i])); /* single-use: evict */
                return DM_DECRYPT_OK;
            }
        }
        return DM_DECRYPT_TOO_FAR; /* not cached: replay/forgery or already consumed */
    }
    /* 2) too far ahead: refuse without deriving (bounded work / DoS bound). */
    if (index > (uint16_t)(chain->index + DM_MAX_SKIP))
        return DM_DECRYPT_TOO_FAR;
    /* 3) derive keys [next .. index]; only commit chain/skip on a successful tag. */
    uint8_t ck[32]; memcpy(ck, chain->ck, 32);
    dm_skip_entry_t pending[DM_MAX_SKIP]; int npending = 0;
    memset(pending, 0, sizeof(pending));
    uint8_t mk[32], ck_next[32];
    for (uint16_t walk = chain->index; walk < index; walk++) {
        dm_ratchet_step(ck, walk, mk, ck_next);
        if (npending < DM_MAX_SKIP) {
            pending[npending].index = walk;
            memcpy(pending[npending].mk, mk, 32);
            pending[npending].used = 1;
            npending++;
        }
        memcpy(ck, ck_next, 32);
    }
    dm_ratchet_step(ck, index, mk, ck_next); /* the target message key */
    int rc = crypto_aes256gcm_decrypt(mk, nonce, ct, ct_len, aad, aad_len, tag, pt_out);
    if (rc == 0) {
        for (int p = 0; p < npending; p++) { /* cache the skipped keys */
            int slot = pending[p].index % DM_MAX_SKIP;
            skip[slot] = pending[p]; skip[slot].used = 1;
        }
        memcpy(chain->ck, ck_next, 32);
        chain->index = (uint16_t)(index + 1);
        *pt_len_out = ct_len;
    }
    memset(ck, 0, 32); memset(mk, 0, 32); memset(ck_next, 0, 32);
    memset(pending, 0, sizeof(pending));
    return rc == 0 ? DM_DECRYPT_OK : DM_DECRYPT_FAIL;
}

int dm_session_ratchet_decrypt(dm_session_t* s, const bramble_header_t* h, uint32_t src_addr,
                               const uint8_t nonce[12], const uint8_t* framed_ct,
                               size_t framed_ct_len, const uint8_t* tag, uint8_t* pt_out,
                               size_t* pt_len_out) {
    if (!s->ratchet.recv.valid)
        return DM_DECRYPT_FAIL;
    if (framed_ct_len < DM_RATCHET_HEADER_SIZE)
        return DM_DECRYPT_FAIL;
    /* Read the cleartext ratchet header: epoch (framed_ct[0]) || msg_index (BE). */
    uint16_t index = (uint16_t)((framed_ct[1] << 8) | framed_ct[2]);
    const uint8_t* ct = framed_ct + DM_RATCHET_HEADER_SIZE;
    size_t ct_len = framed_ct_len - DM_RATCHET_HEADER_SIZE;
    /* AAD = base packet AAD || the 3 cleartext header bytes (authenticated). */
    uint8_t aad[HEADER_SIZE + 4 + DM_RATCHET_HEADER_SIZE];
    if (bramble_build_aead_aad(h, src_addr, aad, HEADER_SIZE + 4) != ESP_OK)
        return DM_DECRYPT_FAIL;
    memcpy(aad + HEADER_SIZE + 4, framed_ct, DM_RATCHET_HEADER_SIZE);
    int rc = dm_recv_walk(&s->ratchet.recv, s->ratchet.skip, index, aad, sizeof(aad), nonce, ct,
                          ct_len, tag, pt_out, pt_len_out);
    /* Task 4 adds a previous-epoch grace pass here: on rc != DM_DECRYPT_OK, retry
     * dm_recv_walk against prev_recv/prev_skip (the header epoch identifies which
     * chain owns the frame; it is in the AAD, so it is authenticated). */
    return rc;
}
```

Implementer note on `DM_DECRYPT_REPLAY`: a frame whose index is behind `next` and not in the skip cache is indistinguishable from a forged one at this layer, so `dm_recv_walk` returns `DM_DECRYPT_TOO_FAR` for it. The authoritative replay defense is the existing per-sender nonce window (`components/replay_window`), which the mesh caller consults BEFORE the ratchet decrypt; the mesh caller maps a nonce-window hit to `DM_DECRYPT_REPLAY` and drops silently (no heal). The `DM_DECRYPT_REPLAY` code exists for that caller mapping. Do not add a second replay oracle inside the ratchet.

- [ ] **Step 4: Run to verify it passes**

Run: `cd test/build && cmake .. >/dev/null && make test_dm_ratchet_skip 2>&1 | tail -3 && ./test_dm_ratchet_skip`
Expected: `3 Tests 0 Failures 0 Ignored` / `OK`.

- [ ] **Step 5: Format and commit**

```bash
docker run --rm -v "$PWD:/w" -w /w bramble/runner-full:22.04-go126 clang-format -i components/dm_session/dm_session.c components/dm_session/include/dm_session.h test/test_dm_ratchet_skip.c
git add components/dm_session/ test/test_dm_ratchet_skip.c test/CMakeLists.txt
git commit -m "feat(dm): bounded skipped-key receive window keyed by cleartext index with DoS bound"
```

---

## Task 4: DH ratchet on ke_epoch + epoch-transition grace + wire both payload paths through the ratchet

**Files:**
- Modify: `components/dm_session/dm_session.c` / `include/dm_session.h` (epoch-bump state transition + previous-epoch grace pass in `dm_session_ratchet_decrypt`; teardown wipe already via `memset` in `dm_session_teardown` at `dm_session.c:419`)
- Modify: `main/mesh_task.c` : `send_dm_packet` (`:4696`), `handle_data` session branch (`:2445-2478`), directed LOCATION encrypt (`:661-675`) and decrypt (`:960-976`), `process_ke_init`/`process_ke_resp` (call `dm_session_ratchet_init_state` after a successful handshake), `initiate_dm_handshake` (the `ke_epoch=0` build), a new scheduled epoch-bump trigger, and the desync-heal `maybe_trigger_dm_rehandshake` mapping for `DM_DECRYPT_TOO_FAR`
- Test: `test/test_dm_ratchet_epoch.c` (create), `test/test_ke_envelope.c` (extend the round-trip to use the ratchet wrappers)

**Interfaces:**
- Consumes: `dm_ratchet_dh`, `dm_session_ratchet_init_state`, `dm_session_ratchet_encrypt`, `dm_session_ratchet_decrypt`, `dm_compute_ikm` (Tasks 1-3); the existing handshake `dm_build_init` (rekey path with `peer_id_pub` non-null) / `dm_verify_resp`; `maybe_trigger_dm_rehandshake` (`main/mesh_task.c`, commit `8ab55838`).
- Produces:
  - `void dm_session_epoch_bump(dm_session_t* s, const uint8_t new_dh[32], uint32_t addr_self, uint32_t addr_peer, uint16_t new_epoch);` rolls the root forward with fresh DH, moves the current receive chain into `prev_recv`/`prev_skip` for the grace, resets send/recv chains to index 0 on the new epoch.
  - The previous-epoch grace pass inside `dm_session_ratchet_decrypt`.
  - `#define DM_EPOCH_GRACE_MSGS DM_MAX_SKIP`.

The epoch bump reuses the INIT/RESP handshake: the initiator generates a fresh ephemeral, sends an INIT with `key_id = epoch+1` (the `dm_build_init` B1 rekey path that already exists at `dm_session.c:198-240` but is unwired), the responder answers RESP; both compute a fresh handshake IKM whose DH1 term is the new ephemerals, feed the DH into `dm_ratchet_dh`, and reset chains. A lost rekey leaves both sides on the old epoch (chains still valid), so no message is stranded.

- [ ] **Step 1: Write the failing tests**

Create `test/test_dm_ratchet_epoch.c` (reuse `make_pair`/`hdr` from Task 3, copied in, with its own `#include`s + `setUp`/`tearDown`/`main`):

```c
/* After an epoch bump, in-flight OLD-epoch messages still decrypt during the
 * grace, NEW-epoch messages decrypt, and the recv epoch advanced. */
void test_epoch_transition_grace_then_wipe(void) {
    dm_session_t sa, sb; bramble_identity_t a, b; make_pair(&sa, &sb, &a, &b);
    bramble_header_t h = hdr(b.address);
    /* A sends one old-epoch message; B has not received it yet (in flight). */
    const uint8_t old_pt[] = "old-epoch";
    uint8_t onc[12]; memset(onc, 0x40, 12);
    uint8_t oct[DM_RATCHET_HEADER_SIZE + sizeof(old_pt)]; uint8_t otag[16]; size_t oflen;
    dm_session_ratchet_encrypt(&sa, &h, a.address, old_pt, sizeof(old_pt), onc, oct, otag, &oflen);

    uint8_t dh[32]; memset(dh, 0x77, 32);
    dm_session_epoch_bump(&sa, dh, a.address, b.address, 1);
    dm_session_epoch_bump(&sb, dh, b.address, a.address, 1);

    uint8_t out[32]; size_t olen;
    TEST_ASSERT_EQUAL(DM_DECRYPT_OK, dm_session_ratchet_decrypt(&sb, &h, a.address, onc, oct, oflen,
                                                               otag, out, &olen)); /* grace */
    TEST_ASSERT_EQUAL_MEMORY(old_pt, out, sizeof(old_pt));

    const uint8_t new_pt[] = "new-epoch";
    uint8_t nnc[12]; memset(nnc, 0x41, 12);
    uint8_t nct[DM_RATCHET_HEADER_SIZE + sizeof(new_pt)]; uint8_t ntag[16]; size_t nflen;
    dm_session_ratchet_encrypt(&sa, &h, a.address, new_pt, sizeof(new_pt), nnc, nct, ntag, &nflen);
    TEST_ASSERT_EQUAL(DM_DECRYPT_OK, dm_session_ratchet_decrypt(&sb, &h, a.address, nnc, nct, nflen,
                                                               ntag, out, &olen));
    TEST_ASSERT_EQUAL_MEMORY(new_pt, out, sizeof(new_pt));
    TEST_ASSERT_EQUAL_UINT8(1, sb.ratchet.recv.epoch);
}

/* The root changed: an epoch bump yields a different root (PCS: a captured
 * pre-bump root cannot derive post-bump keys). */
void test_epoch_bump_changes_root(void) {
    dm_session_t sa, sb; bramble_identity_t a, b; make_pair(&sa, &sb, &a, &b);
    uint8_t rk_before[32]; memcpy(rk_before, sa.ratchet.rk, 32);
    uint8_t dh[32]; memset(dh, 0x5A, 32);
    dm_session_epoch_bump(&sa, dh, a.address, b.address, 1);
    TEST_ASSERT_NOT_EQUAL(0, memcmp(rk_before, sa.ratchet.rk, 32));
}
```

Register `test_dm_ratchet_epoch` in `test/CMakeLists.txt`. Also extend `test/test_ke_envelope.c` `test_ke_envelope_round_trip_to_session` (`:40-126`) so the chat round-trip at lines 101-125 uses `dm_session_ratchet_init_state` + `dm_session_ratchet_encrypt`/`_decrypt` instead of the raw `dm_session_encrypt`/`_decrypt`, proving the envelope-to-ratchet path end to end.

- [ ] **Step 2: Run to verify it fails**

Run: `cd test/build && cmake .. >/dev/null && make test_dm_ratchet_epoch 2>&1 | tail -5`
Expected: FAIL, `undefined reference to 'dm_session_epoch_bump'`.

- [ ] **Step 3: Write the implementation**

Add to `include/dm_session.h`:

```c
#define DM_EPOCH_GRACE_MSGS DM_MAX_SKIP
void dm_session_epoch_bump(dm_session_t* s, const uint8_t new_dh[32], uint32_t addr_self,
                           uint32_t addr_peer, uint16_t new_epoch);
```

Implement in `dm_session.c`:

```c
void dm_session_epoch_bump(dm_session_t* s, const uint8_t new_dh[32], uint32_t addr_self,
                           uint32_t addr_peer, uint16_t new_epoch) {
    uint32_t addr_a = addr_self < addr_peer ? addr_self : addr_peer;
    uint32_t addr_b = addr_self < addr_peer ? addr_peer : addr_self;
    /* Retain the current receive chain + skip cache as the previous epoch for
     * the grace window (in-flight old-epoch frames still decrypt). */
    s->ratchet.prev_recv = s->ratchet.recv;
    memcpy(s->ratchet.prev_skip, s->ratchet.skip, sizeof(s->ratchet.prev_skip));
    s->ratchet.new_epoch_msgs = 0;
    uint8_t rk_next[32];
    dm_ratchet_dh(s->ratchet.rk, new_dh, addr_a, addr_b, new_epoch, rk_next);
    memcpy(s->ratchet.rk, rk_next, 32);
    uint8_t ck_lohi[32], ck_hilo[32];
    const char* lohi = "bramble-dm-chain-lohi";
    const char* hilo = "bramble-dm-chain-hilo";
    crypto_hkdf_sha256(s->ratchet.rk, 32, NULL, 0, (const uint8_t*)lohi, strlen(lohi), ck_lohi, 32);
    crypto_hkdf_sha256(s->ratchet.rk, 32, NULL, 0, (const uint8_t*)hilo, strlen(hilo), ck_hilo, 32);
    int self_is_lo = (addr_self == addr_a);
    memcpy(s->ratchet.send.ck, self_is_lo ? ck_lohi : ck_hilo, 32);
    memcpy(s->ratchet.recv.ck, self_is_lo ? ck_hilo : ck_lohi, 32);
    s->ratchet.send.index = 0; s->ratchet.recv.index = 0;
    s->ratchet.send.epoch = (uint8_t)(new_epoch & 0xFF);
    s->ratchet.recv.epoch = (uint8_t)(new_epoch & 0xFF);
    s->ke_epoch = new_epoch;
    memset(s->ratchet.skip, 0, sizeof(s->ratchet.skip));
    memset(rk_next, 0, 32); memset(ck_lohi, 0, 32); memset(ck_hilo, 0, 32);
}
```

Add the previous-epoch grace pass to `dm_session_ratchet_decrypt` where the Task 3 comment marks it: on a current-epoch miss (`rc != DM_DECRYPT_OK`), if `s->ratchet.prev_recv.valid`, run `dm_recv_walk(&s->ratchet.prev_recv, s->ratchet.prev_skip, ...)`; on success return `DM_DECRYPT_OK`. On a current-epoch success, increment `s->ratchet.new_epoch_msgs`; once it reaches `DM_EPOCH_GRACE_MSGS`, wipe the previous epoch: `memset(&s->ratchet.prev_recv, 0, sizeof(s->ratchet.prev_recv)); memset(s->ratchet.prev_skip, 0, sizeof(s->ratchet.prev_skip));`. The wipe order (only AFTER the new chain is established and the grace is spent) is what delivers PCS and is called out for review gate 3.

**mesh_task.c wiring (all four call sites + triggers):**
1. `process_ke_init`/`process_ke_resp`: after the handshake succeeds and the session key is set, call `dm_session_ratchet_init_state(sess, ikm, my_addr, peer_addr)` (the IKM is already computed there for `dm_derive_session_key`; thread it out or recompute via `dm_compute_ikm`). If the INIT/RESP carried `key_id > sess->ke_epoch`, it is a rekey: after `init_state`, call `dm_session_epoch_bump` with the fresh DH so both sides land on the new epoch. Set `sess->verified` from the pin (Task 7), not to 0.
2. `send_dm_packet` (`:4696`): replace the `dm_session_encrypt` call with `dm_session_ratchet_encrypt`; the buffer is 3 bytes larger, so reduce the usable payload cap checked against `FRAG_MAX_PLAINTEXT` by `DM_RATCHET_HEADER_SIZE`. Keep the `nonce_counter_next`/`s_nonce_mutex` nonce exactly as is.
3. `handle_data` session branch (`:2445-2478`): replace `dm_session_decrypt` with `dm_session_ratchet_decrypt`; on `DM_DECRYPT_TOO_FAR`/`DM_DECRYPT_FAIL` fall through to the SAME existing decrypt-failure branch that already calls `maybe_trigger_dm_rehandshake`; on `DM_DECRYPT_REPLAY` drop silently (no heal, no delivery).
4. Directed LOCATION encrypt (`:661-675`) and decrypt (`:960-976`): make the identical swap to the ratchet wrappers. This is the CRITICAL global-constraint path: a directed location share is a session payload exactly like a chat DM and must ride the same ratchet.
5. Scheduled epoch bump: add a proactive trigger (near the existing periodic DM maintenance / `maybe_trigger_dm_rehandshake`) that, every N sent messages or T minutes per active session, initiates a rekey INIT with `key_id = ke_epoch + 1`, rate-limited exactly like the desync-heal (reuse the 15 s/peer, neighbor-gated guard). Choose N/T conservatively (e.g. N=256 messages or T=30 min); the exact PCS-latency bound is flagged for review gate 1.

Teardown already wipes the whole slot (`dm_session_teardown` `memset`s `sizeof(*s)` at `:423`), which now includes the embedded `ratchet`, so FS on teardown holds; add a one-line comment there confirming the ratchet state is covered by the existing memset.

- [ ] **Step 4: Run to verify it passes**

Run: `cd test/build && cmake .. >/dev/null && make test_dm_ratchet_epoch test_ke_envelope 2>&1 | tail -3 && ./test_dm_ratchet_epoch && ./test_ke_envelope`
Board build to prove the mesh_task wiring compiles: `bash scripts/flash.sh local heltec_v4 build 2>&1 | tail -5`
Expected: host tests `OK`; board build `Project build complete`.

- [ ] **Step 5: Format and commit**

```bash
docker run --rm -v "$PWD:/w" -w /w bramble/runner-full:22.04-go126 clang-format -i components/dm_session/dm_session.c components/dm_session/include/dm_session.h main/mesh_task.c test/test_dm_ratchet_epoch.c test/test_ke_envelope.c
git add components/dm_session/ main/mesh_task.c test/
git commit -m "feat(dm): DH ratchet on ke_epoch with grace window, wire chat and location through the ratchet"
```

---

## Task 5: Identity-bound SAS (safety number)

**Files:**
- Modify: `components/dm_session/dm_session.c` / `include/dm_session.h` (add `dm_derive_identity_sas`)
- Test: `test/test_dm_sas_identity.c` (create), `test/CMakeLists.txt` (register)

**Interfaces:**
- Consumes: `crypto_hkdf_sha256`.
- Produces: `int dm_derive_identity_sas(const uint8_t id_x25519_a[32], const uint8_t id_x25519_b[32], uint32_t addr_a, uint32_t addr_b, char sas_out[8]);` renders a 7-digit decimal safety number from `HKDF(salt="bramble-sas-id", ikm="", info=id_lo||id_hi)` where lo/hi are the two X25519 identity keys ordered by their owners' addresses (`addr_lo`/`addr_hi`), so both peers compute the same string regardless of argument order.

- [ ] **Step 1: Write the failing tests**

Create `test/test_dm_sas_identity.c`:

```c
#include "unity.h"
#include "dm_session.h"
#include "../components/crypto/crypto_host.c"
#include "../components/dm_session/dm_session.c"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* Order independence: (a,b) and (b,a) produce the same SAS. */
void test_identity_sas_order_independent(void) {
    uint8_t ka[32], kb[32];
    for (int i = 0; i < 32; i++) { ka[i] = (uint8_t)i; kb[i] = (uint8_t)(255 - i); }
    char s1[8], s2[8];
    TEST_ASSERT_EQUAL(0, dm_derive_identity_sas(ka, kb, 0x1111, 0x2222, s1));
    TEST_ASSERT_EQUAL(0, dm_derive_identity_sas(kb, ka, 0x2222, 0x1111, s2));
    TEST_ASSERT_EQUAL_STRING(s1, s2);
    TEST_ASSERT_EQUAL(7, strlen(s1));
}

/* Stability across sessions: same identity keys -> same SAS regardless of any
 * session/ephemeral state (the point of the identity-bound redefinition). */
void test_identity_sas_stable_across_sessions(void) {
    uint8_t ka[32], kb[32];
    for (int i = 0; i < 32; i++) { ka[i] = (uint8_t)(i * 3); kb[i] = (uint8_t)(i * 7 + 1); }
    char s1[8], s2[8];
    TEST_ASSERT_EQUAL(0, dm_derive_identity_sas(ka, kb, 0xAAAA, 0xBBBB, s1));
    TEST_ASSERT_EQUAL(0, dm_derive_identity_sas(ka, kb, 0xAAAA, 0xBBBB, s2));
    TEST_ASSERT_EQUAL_STRING(s1, s2);
}

/* MitM detection: a different peer identity key -> different SAS. */
void test_identity_sas_detects_key_substitution(void) {
    uint8_t ka[32], kb[32], kb_mitm[32];
    for (int i = 0; i < 32; i++) { ka[i] = (uint8_t)i; kb[i] = (uint8_t)(i + 5); }
    memcpy(kb_mitm, kb, 32); kb_mitm[0] ^= 0x01;
    char honest[8], mitm[8];
    TEST_ASSERT_EQUAL(0, dm_derive_identity_sas(ka, kb, 0x1111, 0x2222, honest));
    TEST_ASSERT_EQUAL(0, dm_derive_identity_sas(ka, kb_mitm, 0x1111, 0x2222, mitm));
    TEST_ASSERT_NOT_EQUAL(0, strcmp(honest, mitm));
}

/* Known vector (fill in Step 4). */
void test_identity_sas_known_vector(void) {
    uint8_t ka[32], kb[32];
    for (int i = 0; i < 32; i++) { ka[i] = (uint8_t)i; kb[i] = (uint8_t)(128 + i); }
    char s[8];
    TEST_ASSERT_EQUAL(0, dm_derive_identity_sas(ka, kb, 0x1111, 0x2222, s));
    TEST_ASSERT_EQUAL_STRING("0000000", s); /* FILL from Step 4 reference run */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_identity_sas_order_independent);
    RUN_TEST(test_identity_sas_stable_across_sessions);
    RUN_TEST(test_identity_sas_detects_key_substitution);
    RUN_TEST(test_identity_sas_known_vector);
    return UNITY_END();
}
```

Register `test_dm_sas_identity` in `test/CMakeLists.txt`.

- [ ] **Step 2: Run to verify it fails**

Run: `cd test/build && cmake .. >/dev/null && make test_dm_sas_identity 2>&1 | tail -5`
Expected: FAIL, `undefined reference to 'dm_derive_identity_sas'`.

- [ ] **Step 3: Write the implementation**

Declare in `include/dm_session.h`:

```c
int dm_derive_identity_sas(const uint8_t id_x25519_a[32], const uint8_t id_x25519_b[32],
                           uint32_t addr_a, uint32_t addr_b, char sas_out[8]);
```

Implement in `dm_session.c` (mirror `dm_derive_sas` at `:120-131` for the rendering):

```c
int dm_derive_identity_sas(const uint8_t id_x25519_a[32], const uint8_t id_x25519_b[32],
                           uint32_t addr_a, uint32_t addr_b, char sas_out[8]) {
    const uint8_t* lo = addr_a < addr_b ? id_x25519_a : id_x25519_b;
    const uint8_t* hi = addr_a < addr_b ? id_x25519_b : id_x25519_a;
    uint8_t info[64];
    memcpy(info, lo, 32);
    memcpy(info + 32, hi, 32);
    const char* salt = "bramble-sas-id";
    uint8_t okm[4];
    if (crypto_hkdf_sha256((const uint8_t*)salt, strlen(salt), NULL, 0, info, sizeof(info), okm,
                           sizeof(okm)) != 0)
        return -1;
    uint32_t v =
        ((uint32_t)okm[0] << 24) | ((uint32_t)okm[1] << 16) | ((uint32_t)okm[2] << 8) | okm[3];
    snprintf(sas_out, 8, "%07u", (unsigned)(v % 10000000u));
    return 0;
}
```

- [ ] **Step 4: Fill the known vector and run to verify it passes**

Dump the computed SAS once, paste it into `test_identity_sas_known_vector`. Run: `cd test/build && cmake .. >/dev/null && make test_dm_sas_identity 2>&1 | tail -3 && ./test_dm_sas_identity`
Expected: `4 Tests 0 Failures 0 Ignored` / `OK`.

- [ ] **Step 5: Format and commit**

```bash
docker run --rm -v "$PWD:/w" -w /w bramble/runner-full:22.04-go126 clang-format -i components/dm_session/dm_session.c components/dm_session/include/dm_session.h test/test_dm_sas_identity.c
git add components/dm_session/ test/test_dm_sas_identity.c test/CMakeLists.txt
git commit -m "feat(dm): identity-bound safety-number SAS stable across sessions"
```

---

## Task 6: Persist the identity pin store + verified bits to NVS

**Files:**
- Modify: `components/identity/include/identity_store.h` (`verified` + `verified_sas[8]` on `identity_pin_t`; new API), `components/identity/identity_store.c` (setter/getter + serialize/deserialize)
- Modify: `components/nvs_keys/include/nvs_keys.h` (new key under `NVS_NS_IDENTITY`)
- Test: `test/test_identity_store_persist.c` (create); register in `test/CMakeLists.txt` (model on the existing `test_identity_store` block, which links `identity_store.c` directly)
- Reference persistence pattern: `components/identity/identity.c:36-53` (`nvs_get_blob`/`nvs_set_blob` + `nvs_commit` under `NVS_NS_IDENTITY`); the per-index blob pattern in `components/channel/channel_storage.c` for a 32-slot table.

**Interfaces:**
- Consumes: existing `identity_store_t`, `identity_pin_t`, `identity_store_lookup`, `identity_store_pin`, `identity_store_init` (`identity_store.h`).
- Produces:
  - New `identity_pin_t` fields: `bool verified;` and `char verified_sas[8];`.
  - `bool identity_store_set_verified(identity_store_t* s, uint32_t address, const char sas[8]);` sets the bit + records the SAS-at-verification; returns false if no pin for `address`.
  - `bool identity_store_clear_verified(identity_store_t* s, uint32_t address);` clears it (used by Task 7 on a key-change red flag).
  - `bool identity_store_is_verified(const identity_store_t* s, uint32_t address);`
  - `int identity_store_serialize(const identity_store_t* s, uint8_t* buf, size_t buf_len);` returns bytes written or -1; `int identity_store_deserialize(identity_store_t* s, const uint8_t* buf, size_t len, uint32_t now_ms);` returns 0 on success. Both pure (no NVS); the firmware caller in `mesh_task.c` does the `nvs_open`/`nvs_set_blob`/`nvs_commit`.

Host tests stay NVS-free (pure serialize/deserialize round-trip); the actual `nvs_*` calls live in `mesh_task.c` and are exercised by the board build + emulator.

- [ ] **Step 1: Write the failing tests**

Create `test/test_identity_store_persist.c`:

```c
#include "unity.h"
#include "identity_store.h"
#include "../components/identity/identity_store.c"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void mkkeys(uint8_t ed[32], uint8_t x[32], uint8_t seed) {
    for (int i = 0; i < 32; i++) { ed[i] = seed + i; x[i] = seed * 2 + i; }
}

/* A verified bit + SAS survives a serialize/deserialize cycle (the NVS-free
 * core of persistence). */
void test_verified_bit_survives_roundtrip(void) {
    identity_store_t s; identity_store_init(&s, 1000);
    uint8_t ed[32], x[32]; mkkeys(ed, x, 0x10);
    TEST_ASSERT_EQUAL(IDENTITY_PIN_NEW, identity_store_pin(&s, 0xAABBCCDD, ed, x, 1000));
    TEST_ASSERT_TRUE(identity_store_set_verified(&s, 0xAABBCCDD, "1234567"));
    TEST_ASSERT_TRUE(identity_store_is_verified(&s, 0xAABBCCDD));

    uint8_t buf[4096];
    int n = identity_store_serialize(&s, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);

    identity_store_t s2;
    TEST_ASSERT_EQUAL(0, identity_store_deserialize(&s2, buf, n, 2000));
    TEST_ASSERT_TRUE(identity_store_is_verified(&s2, 0xAABBCCDD));
    const identity_pin_t* e = identity_store_lookup(&s2, 0xAABBCCDD);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_MEMORY(x, e->x25519_pub, 32);
    TEST_ASSERT_EQUAL_STRING("1234567", e->verified_sas);
}

/* set_verified on an unknown address returns false; clear_verified toggles off. */
void test_set_and_clear_verified(void) {
    identity_store_t s; identity_store_init(&s, 0);
    TEST_ASSERT_FALSE(identity_store_set_verified(&s, 0xDEAD, "0000000"));
    uint8_t ed[32], x[32]; mkkeys(ed, x, 0x20);
    identity_store_pin(&s, 0xBEEF, ed, x, 0);
    TEST_ASSERT_TRUE(identity_store_set_verified(&s, 0xBEEF, "7654321"));
    TEST_ASSERT_TRUE(identity_store_clear_verified(&s, 0xBEEF));
    TEST_ASSERT_FALSE(identity_store_is_verified(&s, 0xBEEF));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_verified_bit_survives_roundtrip);
    RUN_TEST(test_set_and_clear_verified);
    return UNITY_END();
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd test/build && cmake .. >/dev/null && make test_identity_store_persist 2>&1 | tail -5`
Expected: FAIL, `identity_pin_t has no member named 'verified'` and undefined `identity_store_set_verified`.

- [ ] **Step 3: Write the implementation**

Add to `identity_pin_t` in `identity_store.h`:

```c
    bool verified;         /* SAS confirmed out of band (survives reboot via NVS) */
    char verified_sas[8];  /* the 7-digit identity SAS at verification time */
```

Add the API declarations and implement in `identity_store.c`:
- `identity_store_set_verified` / `identity_store_clear_verified` / `identity_store_is_verified` via `find_entry`/`identity_store_lookup`.
- `identity_store_serialize` writes a 1-byte format version, then for each `used` entry a fixed record: `address(4) || ed25519_pub(32) || x25519_pub(32) || verified(1) || verified_sas(8)`. Skip `pinned_at_ms`/`last_confirmed_ms` (they reset on reboot anyway, per the struct comment).
- `identity_store_deserialize` calls `identity_store_init(s, now_ms)` then rebuilds via `identity_store_pin` + `identity_store_set_verified` for each record. Reject a wrong format-version byte (clean flag day).

Add the NVS key to `nvs_keys.h` under the identity section:

```c
#define ID_KEY_PIN_STORE "pin_store"   /* serialized verified TOFU pin table blob */
```

In `mesh_task.c`, load the blob at boot (after `identity_store_init`) via `nvs_get_blob(NVS_NS_IDENTITY, ID_KEY_PIN_STORE, ...)` -> `identity_store_deserialize`, and save via `identity_store_serialize` -> `nvs_set_blob` + `nvs_commit` whenever a NEW pin is added or a verified bit changes (follow `identity.c:36-53`). Do NOT persist on a REFRESHED re-attestation (those only bump the RAM LRU, per the struct comment).

- [ ] **Step 4: Run to verify it passes**

Run: `cd test/build && cmake .. >/dev/null && make test_identity_store_persist test_identity_store 2>&1 | tail -3 && ./test_identity_store_persist && ./test_identity_store`
Board build for the NVS wiring: `bash scripts/flash.sh local heltec_v4 build 2>&1 | tail -3`
Expected: host tests `OK`; board build complete.

- [ ] **Step 5: Format and commit**

```bash
docker run --rm -v "$PWD:/w" -w /w bramble/runner-full:22.04-go126 clang-format -i components/identity/identity_store.c components/identity/include/identity_store.h components/nvs_keys/include/nvs_keys.h main/mesh_task.c test/test_identity_store_persist.c
git add components/identity/ components/nvs_keys/ main/mesh_task.c test/
git commit -m "feat(identity): persist TOFU pin store and verified bits to NVS"
```

---

## Task 7: Wire verified state into the session + clear on key change

**Files:**
- Modify: `main/mesh_task.c` (source `sess->verified` from the pin instead of the hardcoded `= 0`; on `DM_VERIFY_ERR_PIN_MISMATCH` / `dm_pin_disagrees` -> `dm_session_teardown`, also clear the pin's verified bit + set a re-verify signal)
- Modify: `components/dm_session/dm_session.c` / `include/dm_session.h` (add the pure `dm_verified_should_clear` decision helper)
- Test: extend `test/test_dm_session.c`

**Interfaces:**
- Consumes: `identity_store_is_verified`/`identity_store_clear_verified` (Task 6); `identity_store_lookup`; `dm_derive_identity_sas` (Task 5); `dm_pin_disagrees`/`dm_session_teardown` (existing, `dm_session.c:419-431`); `dm_session_t.verified`.
- Produces: `bool dm_verified_should_clear(const dm_session_t* s, const uint8_t pinned_x25519[32]);` true iff `s->verified` AND `dm_pin_disagrees`. No other new public function; the behavior is that `dm_session_t.verified` reflects the persisted pin bit on (re)establishment, and a genuine identity-key change clears it.

- [ ] **Step 1: Write the failing test**

Add to `test/test_dm_session.c`:

```c
void test_verified_cleared_on_pin_disagreement(void) {
    dm_session_t s; memset(&s, 0, sizeof(s));
    s.state = DM_STATE_ACTIVE; s.verified = 1;
    memset(s.peer_id_pub, 0x11, 32);
    uint8_t new_pin[32]; memset(new_pin, 0x22, 32);  /* key changed */
    TEST_ASSERT_TRUE(dm_verified_should_clear(&s, new_pin));
    uint8_t same_pin[32]; memset(same_pin, 0x11, 32);
    TEST_ASSERT_FALSE(dm_verified_should_clear(&s, same_pin)); /* same key: keep verified */
    s.verified = 0;
    TEST_ASSERT_FALSE(dm_verified_should_clear(&s, new_pin));  /* nothing to clear */
}
```

Add `RUN_TEST(test_verified_cleared_on_pin_disagreement);` to `test_dm_session.c` `main()`.

- [ ] **Step 2: Run to verify it fails**

Run: `cd test/build && cmake .. >/dev/null && make test_dm_session 2>&1 | tail -5`
Expected: FAIL, `undefined reference to 'dm_verified_should_clear'`.

- [ ] **Step 3: Write the implementation**

Add to `dm_session.c` + header:

```c
bool dm_verified_should_clear(const dm_session_t* s, const uint8_t pinned_x25519[32]) {
    return s->verified && dm_pin_disagrees(s, pinned_x25519);
}
```

In `mesh_task.c`:
- In `process_ke_init`/`process_ke_resp`, replace the `sess->verified = 0` line (the "SAS confirmation is a separate UX step, not wired here" comment) with `sess->verified = identity_store_is_verified(&s_identity_store, peer_addr) ? 1 : 0;` so a previously-verified peer re-establishes as verified across reboot / desync-heal / epoch bump (all keep the same pinned identity key, so the identity SAS is unchanged).
- In the existing pin-mismatch handler (`dm_pin_disagrees` -> `dm_session_teardown`), when `dm_verified_should_clear` is true also call `identity_store_clear_verified(&s_identity_store, peer_addr)`, persist (Task 6 save path), and set a re-verify-needed flag surfaced to both UIs (Tasks 8-9). This is the ONE event that both tears down the session (existing) and clears verified (new), because it is the one event where the identity SAS genuinely changed.

- [ ] **Step 4: Run to verify it passes**

Run: `cd test/build && cmake .. >/dev/null && make test_dm_session 2>&1 | tail -3 && ./test_dm_session`
Board build: `bash scripts/flash.sh local heltec_v4 build 2>&1 | tail -3`
Expected: host test `OK`; board build complete.

- [ ] **Step 5: Format and commit**

```bash
docker run --rm -v "$PWD:/w" -w /w bramble/runner-full:22.04-go126 clang-format -i components/dm_session/dm_session.c components/dm_session/include/dm_session.h main/mesh_task.c test/test_dm_session.c
git add components/dm_session/ main/mesh_task.c test/test_dm_session.c
git commit -m "feat(dm): source session verified bit from the pin, clear on identity key change"
```

---

## Task 8: Pager e-paper SAS verification UX

**Files:**
- Create: `components/ui_graphics/screens/scr_sas_verify.c` / `scr_sas_verify.h`
- Modify: `components/ui_graphics/screens/scr_chat_messages.c` (verify glyph in the DM header, a "Verify" entry point, the key-change interstitial); `scr_chat_messages.h` if a new open-variant is needed
- Reference: `components/ui_graphics/include/ui_confirm.h` (`ui_confirm_show`, Cancel focused by default = fail-safe No), `scr_chat_messages_open_dm(bramble_layout_t*, uint32_t peer_addr)` (`scr_chat_messages.h`), a peer-detail screen to model on (`scr_node_detail.c`)
- Test: the emulator E2E (Task 10) is the gating verification; add a host assertion only if `test/test_ui.c` exposes a mockable screen seam

**Interfaces:**
- Consumes: a mesh accessor for `{identity SAS string, verified bool, keyChanged bool}` for a peer (built on `dm_derive_identity_sas` + `identity_store_is_verified` + the Task 7 re-verify flag); `ui_confirm_show`.
- Produces: `void scr_sas_verify_open(bramble_layout_t* layout, uint32_t peer_addr);` the full-screen SAS + confirm flow.

Pager flow per spec B.3: unverified glyph in the DM header; a "Verify" context action opens the SAS screen (7 digits grouped `XXX XXXX`, peer short address, "Read this aloud. It must match on both devices."); SELECT opens the confirm prompt via `ui_confirm_show("Codes match?", "Yes", on_confirm, ...)` which already focuses Cancel by default (satisfies the fail-safe default-No requirement); Yes sets the verified bit and returns showing the verified glyph. On a key-change red flag the header glyph flips to a distinct warning mark and the next DM open interstitials a "This contact's key changed. Re-verify." screen before messages.

- [ ] **Step 1: Write the failing check**

If `test/test_ui.c` exercises screen transitions with a mockable layout, add an assertion that `scr_sas_verify_open` renders the SAS grouped and that the confirm callback sets verified. If the pager UI is only verifiable through LVGL rendering (no host seam), state that explicitly and make the Task 10 emulator E2E the red/green gate; write that failing assertion in Task 10 Step 1 first and cite it here. Do NOT fabricate a host test that cannot compile.

- [ ] **Step 2: Run to verify it fails**

Run the emulator spec (Task 10) or `bash test/run_all_tests.sh 2>&1 | tail -5`.
Expected: the SAS-verify assertion fails (screen/glyph not present yet).

- [ ] **Step 3: Write the implementation**

Create `scr_sas_verify.c`/`.h` following `scr_node_detail.c` (the closest per-peer detail screen). Render the SAS via the mesh accessor, wire SELECT to `ui_confirm_show`, and on confirm call the verified-set path (the Task 9 RPC/mesh setter). In `scr_chat_messages.c`, add the header glyph (verified / unverified / key-changed) and the "Verify" entry point in the DM view opened by `scr_chat_messages_open_dm`, plus the key-change interstitial guard before rendering messages.

- [ ] **Step 4: Run to verify it passes**

Run the Task 10 emulator spec; board build `bash scripts/flash.sh local bramble_pager build 2>&1 | tail -3` (confirm the exact pager board id from `main/boards/bramble_pager.h`).
Expected: SAS screen renders, confirm sets verified, board builds.

- [ ] **Step 5: Format and commit**

```bash
docker run --rm -v "$PWD:/w" -w /w bramble/runner-full:22.04-go126 clang-format -i components/ui_graphics/screens/scr_sas_verify.c components/ui_graphics/screens/scr_sas_verify.h components/ui_graphics/screens/scr_chat_messages.c
git add components/ui_graphics/
git commit -m "feat(pager): SAS verification screen and DM-header verified glyph"
```

---

## Task 9: Webapp SAS verification UX

**Files:**
- Create: `webapp/src/pages/Chat/VerifySafetyNumber.tsx` (+ `.module.css`), a self-contained (no external CDN) QR helper
- Modify: `webapp/src/pages/Chat/Chat.tsx` (header affordance + key-change banner), `webapp/src/pages/Chat/ConversationList.tsx` (verified badge), the transport/store layer (`webapp/src/transport/`, a peer/verification store) to call the RPC
- Modify: `main/rpc_methods.c` (register `bramble.getPeerVerification` and `bramble.setPeerVerified`, authenticated, near `rpc_register("bramble.getIdentity", ...)` at `:2837`, following the `handle_get_identity` shape at `:237-255`)
- Test: `webapp/src/pages/Chat/__tests__/VerifySafetyNumber.test.tsx` (Vitest + React Testing Library, model on `webapp/src/pages/Chat/ConversationList.test.tsx`)

**Interfaces:**
- Consumes: the webapp transport RPC client (`webapp/src/transport/*Transport.ts`); the mesh RPC methods below.
- Produces:
  - Firmware RPC: `bramble.getPeerVerification {address}` -> `{sas: "1234567", verified: bool, keyChanged: bool}`; `bramble.setPeerVerified {address, verified: bool}` -> `{ok: bool}` (writes the pin bit via Task 6's `identity_store_set_verified`/`_clear_verified`, persisted). Authenticated (webapp over BLE/WS carries the auth token; serial is full-privilege).
  - React `VerifySafetyNumber` panel: 7-digit SAS large + grouped, QR of the SAS/fingerprint, "Mark verified" button, and the three status states (unverified / verified / key-changed).

- [ ] **Step 1: Write the failing test**

Create `webapp/src/pages/Chat/__tests__/VerifySafetyNumber.test.tsx`:

```tsx
import { render, screen, fireEvent } from '@testing-library/react';
import { describe, it, expect, vi } from 'vitest';
import { VerifySafetyNumber } from '../VerifySafetyNumber';

describe('VerifySafetyNumber', () => {
  it('shows the grouped SAS and marks verified', () => {
    const setVerified = vi.fn().mockResolvedValue({ ok: true });
    render(
      <VerifySafetyNumber
        peerAddress="aabbccdd"
        sas="1234567"
        verified={false}
        keyChanged={false}
        onSetVerified={setVerified}
      />,
    );
    expect(screen.getByText(/123\s?4567/)).toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', { name: /mark verified/i }));
    expect(setVerified).toHaveBeenCalledWith('aabbccdd', true);
  });

  it('renders the key-changed warning banner', () => {
    render(
      <VerifySafetyNumber
        peerAddress="aabbccdd"
        sas="1234567"
        verified={false}
        keyChanged={true}
        onSetVerified={vi.fn()}
      />,
    );
    expect(screen.getByText(/safety number changed/i)).toBeInTheDocument();
  });
});
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd webapp && npx vitest run src/pages/Chat/__tests__/VerifySafetyNumber.test.tsx 2>&1 | tail -15`
Expected: FAIL, cannot resolve `../VerifySafetyNumber`.

- [ ] **Step 3: Write the implementation**

Create `VerifySafetyNumber.tsx` with the grouped SAS, a self-contained inline-SVG QR (a small vendored encoder, no runtime CDN fetch), the "Mark verified" button calling `onSetVerified(peerAddress, true)`, and a key-changed banner ("The safety number changed. This can happen if your contact reinstalled, or it can mean someone is intercepting. Re-verify before trusting."). Wire it into `Chat.tsx` (header "Verify safety number" affordance + banner) and `ConversationList.tsx` (verified badge). Add the transport call in the peer/verification store. Register the two RPCs in `main/rpc_methods.c`.

- [ ] **Step 4: Run to verify it passes**

Run: `cd webapp && npx vitest run src/pages/Chat/__tests__/VerifySafetyNumber.test.tsx 2>&1 | tail -8 && npm run lint && npm run build 2>&1 | tail -5`
Board build for the RPC: `bash scripts/flash.sh local heltec_v4 build 2>&1 | tail -3`
Expected: Vitest passes; lint/build clean; board builds.

- [ ] **Step 5: Format and commit**

```bash
cd webapp && npx prettier --write src/pages/Chat/VerifySafetyNumber.tsx src/pages/Chat/__tests__/VerifySafetyNumber.test.tsx && cd ..
docker run --rm -v "$PWD:/w" -w /w bramble/runner-full:22.04-go126 clang-format -i main/rpc_methods.c
git add webapp/ main/rpc_methods.c
git commit -m "feat(webapp): SAS safety-number panel, verified badge, key-change banner"
```

---

## Task 10: Emulator two-pager SAS verification E2E

**Files:**
- Create: `emulator/e2e/specs/sas-verify.spec.ts`
- Reference: `emulator/e2e/specs/functionality.spec.ts` (structure), `emulator/e2e/lib/` (`uiActions` = `loadScenario`/`clickButton`/`holdReset`, `glyphMatch` = `findText`, `canvasRead` = `readCanvasGrid`/`canvasSelector`, `wsCapture` = `attachWsCapture`/`waitFor`), `emulator/e2e/run_e2e.sh`

**Interfaces:**
- Consumes: the pager SAS UX (Task 8), the webapp SAS panel/RPC (Task 9), the full ratchet + verified-bit firmware (Tasks 1-7).
- Produces: an E2E proving two virtual pagers establish a DM, compare the SAS (identical on both), mark verified on both, then a forced identity-key change clears the verified bit and prompts re-verify.

- [ ] **Step 1: Write the failing spec**

Create `emulator/e2e/specs/sas-verify.spec.ts` modelled on `functionality.spec.ts`: load a two-node DM scenario, drive both device views over CDP (the rendered WebView per `CLAUDE.md`, not screenshots), and assert via `findText`/`readCanvasGrid` that:
1. Both pagers show the same 7-digit SAS for the peer.
2. Marking verified on both flips the header glyph to verified on both.
3. Forcing an identity-key change (re-provision one node's key or deliver a conflicting attestation) surfaces the key-changed interstitial and clears the verified glyph.

Write the assertions concretely against the rendered canvas text; this is the red gate for Tasks 8 and 9.

- [ ] **Step 2: Run to verify it fails**

Run: `cd emulator/e2e && ./run_e2e.sh sas-verify 2>&1 | tail -20` (or `npx playwright test specs/sas-verify.spec.ts`).
Expected: FAIL (SAS not surfaced / glyph absent) until Tasks 8-9 land.

- [ ] **Step 3: Implementation is in Tasks 8-9.** No new product code here; if the spec reveals a wiring gap, fix it in the relevant task's files and note it.

- [ ] **Step 4: Run to verify it passes**

Run: `cd emulator/e2e && ./run_e2e.sh sas-verify 2>&1 | tail -20`
Expected: all steps green; both pagers agree on the SAS, verify, and re-prompt on key change.

- [ ] **Step 5: Commit**

```bash
git add emulator/e2e/specs/sas-verify.spec.ts
git commit -m "test(emulator): two-pager SAS verification and key-change re-verify E2E"
```

---

## Task 11: Honest SECURITY-MODEL.md + README update (same PR)

**Files:**
- Modify: `docs/SECURITY-MODEL.md` (asset 4 area `:143-154`, the DM E2E section `:575-593`, the "no SAS UX" residual `:1373-1377`), `README.md:37`

**Interfaces:**
- Consumes: nothing; documents the mechanisms from Tasks 1-10.

- [ ] **Step 1: Write the doc changes**

Update, tying every claim to a mechanism and staying honest about review status:
- `README.md:37`: change "No forward secrecy yet; the SAS-comparison UX does not ship yet" to state that DMs now have per-message forward secrecy via a symmetric ratchet with a coarse epoch DH ratchet for post-compromise recovery, and that SAS verification (identity-bound safety number) ships on both the pager and webapp; add "the DM ratchet and SAS redefinition are pending independent cryptographic review."
- `docs/SECURITY-MODEL.md` DM E2E section (`:575-593`): add the FS mechanism (directional HKDF chains, per-message key, 3-byte cleartext ratchet header authenticated via the AEAD AAD, bounded skip window degrading to desync-heal), the coarse epoch DH ratchet, and the SAS UX. Replace the "no out-of-band SAS comparison UX ships yet" residual with the new UX; keep the network-key insider residual.
- Residuals to state explicitly: PCS is coarse/epoch-bounded (not per-message); the network-key insider is unchanged (FS protects content, not the fact/timing/size/`src_addr`); the first-contact TOFU window is unchanged (SAS detects it only if users compare); verified state + pins are now NVS-persisted but sit in plaintext NVS (device-thief adversary, no worse than the identity keys they certify); skip-cache/replay bounds are DoS-shaped, never confidentiality-shaped.
- Add a "Pending cryptographic review" note listing the five spec review gates (key schedule / decoupled DH ratchet + PCS bound; directional-chain labelling for the role-symmetric handshake; epoch-transition state machine + wipe ordering; SAS redefinition; nonce discipline under the ratchet) as claimed-pending-review, NOT verified.

- [ ] **Step 2: Verify the em-dash hook and no stale claims**

Run: `grep -nP '\x{2014}' docs/SECURITY-MODEL.md README.md` (expect no output; the hook rejects em dashes) and re-read the edited sections to confirm no residual still says "no forward secrecy" / "no SAS UX".
Expected: no em dashes; no contradicting leftover claims.

- [ ] **Step 3: Commit**

```bash
git add docs/SECURITY-MODEL.md README.md
git commit -m "docs: DM forward secrecy and SAS UX, honest residuals and pending-review note"
```

---

## Final gate (before PR)

```bash
bash test/run_all_tests.sh
bash scripts/flash.sh local heltec_v4 build
cd webapp && npm run lint && npm run test:unit && npm run build && cd ..
cd emulator/e2e && ./run_e2e.sh sas-verify && cd ../..
make ci
```

Expected: all green. The crypto host tests (`test_dm_ratchet`, `test_dm_ratchet_skip`, `test_dm_ratchet_epoch`, `test_dm_sas_identity`, `test_identity_store_persist`) MUST all pass, including the mandatory nonce-uniqueness assertion (`test_no_key_nonce_reuse`).

---

## Self-Review

**Spec coverage:** Part A forward secrecy -> Tasks 1-4 (key schedule, wire format + version flag day, skip window, DH ratchet + both payload paths). Part B SAS -> Task 5 (identity SAS), Task 6 (NVS persistence), Task 7 (verified wiring + clear-on-key-change), Task 8 (pager UX), Task 9 (webapp UX). Testing/verification strategy -> host tests in Tasks 1/3/4/5/6, emulator in Task 10. Residuals/review gates + doc house rule -> Task 11. All three user-approved decisions (hard flag day, NVS persistence, identity-bound SAS) are the spine of Tasks 2, 6, and 5 respectively.

**Placeholder scan:** KAT/SAS known-vector values are intentionally computed-once-then-committed (Task 1 Step 4, Task 5 Step 4), matching the existing `test_sas_known_vector` convention, not TBD. Task 8's host-test seam is honestly conditional on whether `test_ui.c` has a mockable screen state (the emulator E2E is the fallback gate), not a fabricated test.

**Type consistency:** `dm_ratchet_t`/`dm_chain_t`/`dm_skip_entry_t` defined in Task 2, consumed unchanged in Tasks 3-4. `dm_session_ratchet_encrypt`/`_decrypt` signatures stable across Tasks 2-4 and the mesh wiring. `dm_compute_ikm` de-static'd in Task 2, used in Tasks 3-4. `identity_pin_t.verified`/`verified_sas[8]` defined in Task 6, consumed in Tasks 7-9. `dm_derive_identity_sas` signature stable across Tasks 5, 8, 9. RPC names `bramble.getPeerVerification`/`bramble.setPeerVerified` stable across Tasks 9-10.

**Known open risk carried into review (not a plan gap):** the `DM_MAX_SKIP` RAM cost (~77 KB at 32) is flagged in the Constants block, and the cleartext-index-in-AAD key selection (the standard Double Ratchet layout, which puts each message's epoch+index on the wire as metadata) is documented in the Resolved-ambiguity block; both are surfaced for the user/review.
