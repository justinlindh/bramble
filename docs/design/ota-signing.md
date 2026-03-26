# OTA Firmware Signature Verification — Design Document

**Status:** Draft  
**Date:** 2026-03-25  
**Author:** ops-security-auditor (automated)  
**Audit ref:** S15 (HIGH) — No firmware signature verification for OTA updates  

---

## 1. Problem Statement

Bramble's current OTA implementation (`components/ota/ota.c`) downloads firmware
images and writes them directly to the update partition without verifying
authenticity or integrity. An attacker who can serve a malicious firmware image
(via MITM on HTTP, compromised server, or DNS hijack) can replace the device
firmware with arbitrary code.

The HTTPS path (`ota_https_start`) provides transport-level authentication via
TLS certificate validation, but does **not** verify that the firmware itself was
produced by a trusted build. The HTTP path (`ota_http_start`, gated by
`CONFIG_BRAMBLE_OTA_ALLOW_HTTP`) has no protection at all.

## 2. Goals

- Devices reject firmware not signed by a Bramble project key.
- Signing happens at build time in CI; no manual steps for routine releases.
- Key compromise has a bounded blast radius and a recovery path.
- Solution fits ESP32-S3 constraints (RAM, flash, boot time).
- Design interacts cleanly with S26 (`CONFIG_BRAMBLE_OTA_ALLOW_HTTP`).

## 3. Non-Goals

- ESP-IDF Secure Boot V2 (eFuse-based hardware root of trust). This requires
  one-time eFuse programming per device and is a separate future initiative.
  This design covers **application-level** OTA image signing that works on
  already-deployed devices without eFuse changes.
- Encrypted firmware images (confidentiality). Signing provides authenticity
  and integrity only.
- Code signing for BLE OTA (not yet implemented; will follow the same pattern
  when ready).

---

## 4. Threat Model

### 4.1 Attacks Prevented

| Attack | How Signing Helps |
|---|---|
| **MITM firmware injection** (HTTP or compromised CDN) | Device rejects image with invalid/missing signature |
| **Tampered firmware on update server** | Signature computed offline; server compromise alone is insufficient |
| **Rollback to known-vulnerable version** | Version field in signed metadata prevents downgrade (see §7.3) |
| **Bit-flip / corrupt download** | Signature verification implicitly detects corruption |

### 4.2 Attacks NOT Prevented

| Attack | Why |
|---|---|
| **Signing key compromise** | Attacker can produce valid signatures until key is rotated |
| **Physical access / JTAG** | Hardware attacks bypass all software protections |
| **Supply-chain compromise of build environment** | Signing happens post-build; if CI is owned, signed malicious images are possible |
| **eFuse / bootloader attacks** | Application-level signing doesn't protect the bootloader chain; needs Secure Boot V2 |
| **Side-channel on signature verification** | Ed25519 verify is constant-time in libsodium/mbedTLS but hardware side-channels remain |

### 4.3 Interaction with S26 (`CONFIG_BRAMBLE_OTA_ALLOW_HTTP`)

`CONFIG_BRAMBLE_OTA_ALLOW_HTTP` gates the plaintext HTTP OTA path used only for
local development. With signature verification in place:

- **HTTP + signing:** Firmware authenticity is still verified. An attacker on
  the network can observe the firmware (no confidentiality) but cannot inject a
  replacement. This is acceptable for dev/test but not recommended for
  production.
- **HTTPS + signing:** Defense in depth — TLS protects the transport while the
  signature protects the payload. Recommended for all production OTA.
- **Recommendation:** S26 should ensure `CONFIG_BRAMBLE_OTA_ALLOW_HTTP` is
  never set in release sdkconfig profiles. Signature verification should be
  **mandatory regardless of transport** (no `#ifdef` gating). Even HTTP OTA
  must present a valid signature.

---

## 5. Algorithm Choice: Ed25519

### 5.1 Why Ed25519

| Property | Ed25519 | RSA-2048 | ECDSA P-256 |
|---|---|---|---|
| Public key size | 32 bytes | 256 bytes | 64 bytes |
| Signature size | 64 bytes | 256 bytes | 64 bytes |
| Verify speed (ESP32-S3) | ~2–4 ms | ~15–30 ms | ~8–15 ms |
| Key generation | Fast, deterministic | Slow | Moderate |
| Side-channel resistance | Good (constant-time) | Needs care | Needs care |
| Library support | mbedTLS 3.x, libsodium | mbedTLS | mbedTLS |

Ed25519 is ideal for embedded:
- **32-byte public key** fits in firmware header or NVS with negligible overhead.
- **64-byte signature** adds trivially to image size.
- **Fast verification** — ~3 ms on ESP32-S3 @ 240 MHz — no perceptible OTA delay.
- ESP-IDF's bundled mbedTLS 3.x includes Ed25519 support
  (`MBEDTLS_PSA_CRYPTO` + `PSA_WANT_ALG_PURE_EDDSA`). Alternatively,
  [TweetNaCl](https://tweetnacl.cr.yp.to/) is ~4 KB of code with zero
  dependencies.

### 5.2 Library Recommendation

Use **mbedTLS PSA Crypto API** already bundled with ESP-IDF. This avoids adding
a new dependency, gets hardware acceleration where available, and is maintained
by the ESP-IDF team.

If mbedTLS Ed25519 support proves problematic on the current ESP-IDF version,
fall back to **TweetNaCl** (`crypto_sign_ed25519_open`, ~100 lines, ~4 KB
flash). Either way, the verification interface is identical:

```c
bool ota_verify_signature(const uint8_t *image, size_t image_len,
                          const uint8_t *signature, /* 64 bytes */
                          const uint8_t *public_key /* 32 bytes */);
```

---

## 6. Key Infrastructure

### 6.1 Key Types

| Key | Purpose | Storage | Who Has Access |
|---|---|---|---|
| **Signing key (private)** | Signs firmware images at build time | CI secrets vault (GitHub Actions / Gitea secrets) | CI only; never on developer machines in production |
| **Verification key (public)** | Embedded in firmware; used by device to verify OTA images | Compiled into firmware binary + NVS backup | All devices (public, not secret) |
| **Rotation key (public)** | Pre-provisioned "next" public key for rotation | NVS partition | All devices |

### 6.2 Key Generation

```bash
# One-time key generation (do NOT run in CI; run on trusted machine)
# Using openssl (available in ESP-IDF toolchain)
openssl genpkey -algorithm Ed25519 -out bramble-ota-signing.pem
openssl pkey -in bramble-ota-signing.pem -pubout -out bramble-ota-verify.pem

# Extract raw 32-byte public key for embedding
openssl pkey -in bramble-ota-verify.pem -pubout -outform DER | tail -c 32 > bramble-ota-verify.bin
```

### 6.3 Key Storage on Device

The 32-byte Ed25519 public key is stored in two locations for resilience:

1. **Compiled into firmware** — `components/ota/ota_signing_key.h` containing
   the key as a `const uint8_t[32]`. Updated with each firmware build. This is
   the primary verification source.

2. **NVS partition** — Key `ota_pubkey` in namespace `ota_sign`. Written on
   first boot or during key rotation OTA. Acts as backup and enables key
   rotation without full reflash.

Verification checks NVS first (may contain rotated key), falls back to
compiled-in key.

---

## 7. Signing Process (Build Time)

### 7.1 Firmware Image Format

Current ESP-IDF OTA images are raw app binaries. We add a **signature trailer**
(appended after the image) to avoid modifying the ESP-IDF image format:

```
┌──────────────────────────────┐
│  ESP-IDF App Image           │  (existing binary, variable size)
│  (firmware payload)          │
├──────────────────────────────┤
│  Signature Block (140 bytes) │
│  ┌──────────────────────────┐│
│  │ Magic: 0x42 0x52 0x53 0x47││  "BRSG" — Bramble Signature
│  │ Version: uint8 (0x01)    ││  Schema version
│  │ Flags: uint8             ││  Bit 0: has-min-version
│  │ Min FW version: uint16   ││  Anti-rollback (0 = no check)
│  │ Timestamp: uint32        ││  Build unix timestamp
│  │ Image SHA-256: 32 bytes  ││  Hash of firmware payload
│  │ Signature: 64 bytes      ││  Ed25519(privkey, header + hash)
│  │ Public key hint: 4 bytes ││  First 4 bytes of pubkey (key ID)
│  │ Reserved: 32 bytes       ││  Future use (zero-filled)
│  └──────────────────────────┘│
└──────────────────────────────┘
Total trailer: 4 + 1 + 1 + 2 + 4 + 32 + 64 + 4 + 32 = 144 bytes
```

The **signed message** is: `magic || version || flags || min_fw_version ||
timestamp || image_sha256` (44 bytes). The Ed25519 signature covers this
44-byte header, which transitively covers the full image via the SHA-256 hash.

### 7.2 Build-Time Signing Script

```bash
#!/usr/bin/env bash
# scripts/sign-firmware.sh — Sign a built firmware image
# Usage: sign-firmware.sh <firmware.bin> <signing-key.pem> <output.bin>

set -euo pipefail

FIRMWARE="$1"
SIGNING_KEY="$2"
OUTPUT="$3"
MIN_VERSION="${4:-0}"

# Compute SHA-256 of firmware
IMAGE_HASH=$(sha256sum "$FIRMWARE" | cut -d' ' -f1)

# Build header (44 bytes)
# ... (Python helper or binary tool to construct header)
python3 scripts/ota_sign_helper.py \
    --image "$FIRMWARE" \
    --key "$SIGNING_KEY" \
    --min-version "$MIN_VERSION" \
    --output "$OUTPUT"
```

CI pipeline integration:
1. Build firmware via `scripts/flash.sh` (existing flow).
2. Run `scripts/sign-firmware.sh` on the output `.bin`.
3. Upload signed binary as release artifact.
4. OTA server serves only signed binaries.

### 7.3 Anti-Rollback

The `min_fw_version` field (uint16) in the signature block prevents downgrade
attacks:

- Each signed firmware declares the minimum version it will accept as a
  predecessor.
- During OTA verification, the device compares `min_fw_version` against its
  current running firmware version number.
- If the new image's embedded version is less than the device's stored minimum
  acceptable version, OTA is rejected.

Version tracking uses a monotonic counter stored in NVS (`ota_sign/fw_version`),
incremented only on successful verified OTA.

---

## 8. Verification Process (Device Side)

### 8.1 Verification Flow

```
ota_wifi_start(url)
  │
  ├── Download image to buffer/streaming
  │
  ├── Read last 144 bytes → signature block
  │   ├── Check magic == "BRSG"
  │   ├── Check schema version supported
  │   └── Parse fields
  │
  ├── Compute SHA-256 of image (excluding trailer)
  │   └── Compare against header's image_sha256
  │
  ├── Reconstruct signed message (44 bytes)
  │   └── Ed25519 verify(pubkey, signature, message)
  │       ├── Try NVS pubkey first (rotated key)
  │       └── Fall back to compiled-in pubkey
  │
  ├── Anti-rollback check
  │   └── new_version >= stored min_version?
  │
  ├── IF all pass → esp_ota_write() + esp_ota_end()
  └── IF any fail → esp_ota_abort(), log reason, return error
```

### 8.2 Streaming vs. Buffered Verification

ESP32-S3 has ~512 KB available RAM. Firmware images can be 1–4 MB. Two
approaches:

**Option A: Two-pass streaming (recommended)**
1. First pass: stream image, compute SHA-256 incrementally, save last 144
   bytes as trailer.
2. Verify signature against the hash.
3. Second pass: stream image again, write to OTA partition.

**Option B: Download-then-verify**
1. Download to OTA partition directly (using `esp_ota_write` streaming).
2. Read back from flash to compute SHA-256 and extract trailer.
3. If verification fails, abort (partition not yet set as boot).

**Recommendation:** Option B is simpler and leverages existing download code.
The OTA partition is not marked bootable until `esp_ota_set_boot_partition()`
succeeds, so writing unverified data to it is safe — the device won't boot it
if verification fails. The SHA-256 read-back adds ~1 second for a 2 MB image
(flash read is fast).

### 8.3 Integration with Existing Code

The verification hooks into the existing OTA flow in `components/ota/ota.c`:

```c
// After downloading is complete but BEFORE esp_ota_set_boot_partition():
int rc = ota_verify_image(update_partition, total_bytes_written);
if (rc != 0) {
    ESP_LOGE(TAG, "OTA signature verification failed (rc=%d)", rc);
    esp_ota_abort(ota_handle);
    return -1;
}
// Proceed to esp_ota_set_boot_partition()
```

Both `ota_https_start()` and `ota_http_start()` paths must call verification.
There is **no bypass** — even HTTP dev builds verify signatures. In dev mode,
a well-known dev key can be compiled in.

---

## 9. Key Rotation

### 9.1 Rotation Mechanism

Key rotation uses a **signed key-update message** embedded in a special OTA
image:

1. **Pre-provision:** Each firmware build embeds the current public key AND
   optionally a "next" public key (or a hash of it) in NVS.

2. **Rotation OTA:** A firmware update signed with the **current** key includes
   a new public key in an extension field of the signature block (using the
   reserved 32 bytes).

3. **On successful OTA with rotation flag:**
   - Device writes new public key to NVS (`ota_pubkey`).
   - Subsequent OTA updates must be signed with the new key.
   - The compiled-in key in the new firmware also contains the new key.

### 9.2 Rotation Flow

```
1. Generate new keypair (offline, trusted machine)
2. Build firmware with new public key compiled in
3. Sign firmware with OLD private key (devices still trust old key)
4. Set rotation flag + embed new public key in signature block
5. Deploy OTA → devices:
   a. Verify with old key (NVS/compiled) ✓
   b. Install new firmware (contains new compiled key)
   c. Write new key to NVS
6. Next OTA: sign with NEW key → devices verify with new NVS key ✓
```

### 9.3 Emergency Key Revocation

If the signing key is compromised:

1. Generate new keypair immediately.
2. Use the **old** (compromised) key to sign a rotation firmware that installs
   the new key. This is a race against the attacker.
3. If the attacker has already deployed malicious firmware to some devices:
   those devices are compromised and need physical reflash.
4. **Mitigation:** Keep signing keys in a hardware security module (HSM) or at
   minimum in CI secrets with MFA-gated access. Limit the window of
   vulnerability.

### 9.4 Dual-Key Grace Period

During rotation, support a grace period where both old and new keys are
accepted:

- Verification tries NVS key first, then compiled-in key.
- After rotation, NVS has the new key, compiled-in may still have the old one
  (for devices that haven't updated yet).
- This naturally supports fleet-wide gradual rollout.

---

## 10. ESP32-S3 Constraints

| Resource | Available | Used by Signing |
|---|---|---|
| Flash | 8 MB total, ~4 MB for app | +144 bytes per image (trailer), +~4 KB code (verify) |
| RAM | ~512 KB free | +~1 KB stack for SHA-256 + Ed25519 verify |
| NVS | ~20 KB typical | +68 bytes (pubkey + version counter) |
| Boot time | Not critical for OTA | +~50 ms (hash + verify) |
| CPU | Dual-core 240 MHz | SHA-256: hardware accelerated; Ed25519: ~3 ms |

**Impact: Negligible.** The signing infrastructure adds < 5 KB flash, < 2 KB
RAM, and < 100 ms to OTA verification time.

---

## 11. Implementation Plan

### Phase 1: Core Signing Infrastructure (Effort: ~3 days)

| Step | Task | Files |
|---|---|---|
| 1.1 | Add Ed25519 verify function using mbedTLS PSA | `components/ota/ota_verify.c`, `ota_verify.h` |
| 1.2 | Define signature block format (header struct) | `components/ota/ota_sign_format.h` |
| 1.3 | Create build-time signing script (Python) | `scripts/ota_sign_helper.py` |
| 1.4 | Embed dev public key in firmware | `components/ota/ota_signing_key.h` |
| 1.5 | Unit tests for verify function | `test/test_ota_verify.c` |

### Phase 2: OTA Integration (Effort: ~2 days)

| Step | Task | Files |
|---|---|---|
| 2.1 | Add post-download verification to `ota_https_start()` | `components/ota/ota.c` |
| 2.2 | Add post-download verification to `ota_http_start()` | `components/ota/ota.c` |
| 2.3 | Add NVS key storage/retrieval | `components/ota/ota_verify.c` |
| 2.4 | Anti-rollback version check | `components/ota/ota_verify.c` |
| 2.5 | Integration tests with signed test images | `test/test_ota_verify.c` |

### Phase 3: CI & Tooling (Effort: ~1 day)

| Step | Task | Files |
|---|---|---|
| 3.1 | Add signing step to CI build pipeline | `.gitea/workflows/`, `scripts/sign-firmware.sh` |
| 3.2 | Generate and store production signing key | CI secrets (Gitea/GitHub) |
| 3.3 | Update OTA server / release process to serve signed images | Infra docs |

### Phase 4: Key Rotation Support (Effort: ~2 days)

| Step | Task | Files |
|---|---|---|
| 4.1 | Implement rotation field in signature block | `components/ota/ota_sign_format.h` |
| 4.2 | NVS key rotation on verified OTA with rotation flag | `components/ota/ota_verify.c` |
| 4.3 | Key rotation test scenarios | `test/test_ota_verify.c` |
| 4.4 | Document key rotation runbook | `docs/runbooks/ota-key-rotation.md` |

### Phase 5: Hardening (Effort: ~1 day)

| Step | Task | Files |
|---|---|---|
| 5.1 | Ensure S26 fix prevents `CONFIG_BRAMBLE_OTA_ALLOW_HTTP` in release | Kconfig / CI |
| 5.2 | Add Kconfig option `CONFIG_BRAMBLE_OTA_REQUIRE_SIGNATURE` (default y) | Kconfig |
| 5.3 | Logging and error reporting for signature failures | `components/ota/ota.c` |

**Total estimated effort: ~9 developer-days**

### Dependency Order

```
Phase 1 → Phase 2 → Phase 3 → Phase 5
                  ↘ Phase 4 (can parallel with Phase 3)
```

---

## 12. Future Considerations

- **ESP-IDF Secure Boot V2:** Hardware root of trust via eFuse. Complements
  this design by also protecting the bootloader. Requires per-device
  provisioning.
- **Encrypted OTA:** Firmware encryption for IP protection. Orthogonal to
  signing; can layer on top.
- **Certificate-based signing:** Instead of raw Ed25519, use X.509 certs for
  key identity. Adds complexity; not needed at current scale.
- **BLE OTA:** When implemented, reuse the same signature format and
  verification code.
- **Multi-key signing:** Require M-of-N signatures for production releases.
  Significant complexity; consider only if team grows.

---

## Appendix A: Reference Implementation Sketch

```c
// ota_verify.h
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OTA_SIGN_MAGIC       0x47535242  // "BRSG" little-endian
#define OTA_SIGN_VERSION     0x01
#define OTA_SIGN_TRAILER_LEN 144
#define OTA_PUBKEY_LEN       32
#define OTA_SIGNATURE_LEN    64

typedef struct __attribute__((packed)) {
    uint8_t  magic[4];         // "BRSG"
    uint8_t  version;          // schema version
    uint8_t  flags;            // bit 0: has-min-version, bit 1: has-rotation-key
    uint16_t min_fw_version;   // anti-rollback
    uint32_t timestamp;        // build timestamp
    uint8_t  image_hash[32];   // SHA-256 of firmware payload
    uint8_t  signature[64];    // Ed25519 signature
    uint8_t  pubkey_hint[4];   // first 4 bytes of signing pubkey
    uint8_t  reserved[32];     // future: rotation key, etc.
} ota_sign_trailer_t;

_Static_assert(sizeof(ota_sign_trailer_t) == OTA_SIGN_TRAILER_LEN,
               "trailer size mismatch");

/**
 * Verify a signed firmware image on the OTA partition.
 * Returns 0 on success, negative on failure.
 */
int ota_verify_image(const void *partition, size_t image_len);
```

---

## Appendix B: Threat Comparison With/Without Signing

| Scenario | Without Signing | With Signing |
|---|---|---|
| Compromised HTTP server | Full device takeover | Rejected — invalid signature |
| MITM on HTTP OTA | Full device takeover | Rejected — invalid signature |
| MITM on HTTPS OTA (cert pinning bypass) | Full device takeover | Rejected — invalid signature |
| Compromised CI + signing key | N/A | Full device takeover (mitigate with HSM) |
| Corrupt download | Undefined behavior / brick | Clean rejection, retry |
| Downgrade to old vulnerable version | Succeeds | Rejected by anti-rollback |
