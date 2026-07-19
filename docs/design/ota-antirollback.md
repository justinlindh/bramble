# OTA Anti-Rollback: eFuse Secure Version

**Status:** Software floor reconciliation implemented and host-tested; eFuse
enforcement is compile-gated, opt-in, and PENDING bench validation (not yet
executed on hardware).
**Last verified (host):** 2026-07-19 (`test/test_ota_rollback_policy.c`,
`test/test_ota.c`, `test/test_ota_version.c`)
**Issue:** #79
**Related:** `docs/design/ota-signing.md`, `docs/SECURITY-MODEL.md` section 4

## 1. The gap this closes

Bramble's OTA path already verifies an RSA-3072 signature on every image and
rejects downgrades below a version floor. The floor, however, lives in NVS
(`components/ota/ota_rollback.c`). An attacker with flash access, or any NVS
wipe, erases it and can then install a signed-but-vulnerable older build.
Closing that gap needs a floor that survives a flash rewrite and an NVS erase,
which means the ESP32-S3 eFuse secure-version field enforced by the second
stage bootloader (`CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK`).

Burning eFuses is irreversible and, like Secure Boot V2 and flash encryption
(ledger SEC-H4), is a human-gated bench step. This document specifies the
design, ships the parts that are safe to ship with enforcement compiled out by
default, and writes down the bench-validation procedure as an explicit pending
gate.

## 2. Two version quantities, deliberately decoupled

There are two different numbers, and conflating them would either exhaust the
eFuse field or fail to protect anything.

- **`version`** (semver string in `esp_app_desc_t.version`, e.g. `0.5.0`). Set
  from `PROJECT_VER` at build time, driven by semantic-release
  (`firmware-vX.Y.Z` tags, `.releaserc.firmware.cjs`). It moves on every
  release: a patch bump per fix, a minor per feature. This feeds the soft NVS
  floor and full semver precedence comparison (`components/ota/ota_version.c`).

- **`secure_version`** (a `uint32_t` in `esp_app_desc_t.secure_version`, set
  from `CONFIG_BOOTLOADER_APP_SECURE_VERSION`). This is a coarse, monotonic
  **security epoch counter**, not the release version. It is enforced by the
  bootloader against a burned eFuse field, and every increment consumes one
  irreversible eFuse bit (the field is 16 bits wide by default in the overlay,
  so there is a hard lifetime budget of increments).

**Mapping rule.** `secure_version` is bumped only by a deliberate human
decision, in its own PR, when a shipped fix closes a vulnerability that a
device must never be rolled back past. It is NOT bumped per release.
semantic-release computes the semver and never touches `secure_version`. The
current epoch and its rationale live in the table below, which is the single
source of truth alongside `sdkconfig.defaults.antirollback`.

| secure_version | First shipped in (firmware release) | Reason for the epoch bump |
|---|---|---|
| 0 | all builds to date (baseline) | Baseline. No anti-rollback-worthy fix has forced an epoch yet. |

When a future fix warrants it, add a row, bump
`CONFIG_BOOTLOADER_APP_SECURE_VERSION` in the overlay, and note the semver of
the release that carries it. Because the eFuse floor only ratchets up on a
successful boot of the higher-epoch image, a device that never takes that
release stays at its old epoch until it does.

## 3. Enforcement: boot time vs OTA-accept time

The design puts two checks in series so they cannot disagree dangerously. Both
are exercised by the pure, host-tested policy in
`components/ota/ota_rollback_policy.c` (`ota_rollback_decide`), which
`ota_rollback.c` calls with values read from NVS, the image descriptor, and
the eFuse.

- **At boot (bootloader, hardware floor).** With
  `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK`, the second-stage bootloader refuses to
  boot any app whose `secure_version` is below the burned eFuse value, and
  after a validated boot it ratchets the eFuse up to the running app's
  `secure_version`. This is the guarantee that survives a flash rewrite and an
  NVS wipe: even if an attacker writes an older signed image and erases NVS,
  the bootloader will not run it.

- **At OTA-accept time (`ota_rollback_gate`, both floors).** Before committing
  airtime to the download, the gate reconciles:
  1. **Hardware floor first.** When compiled with anti-rollback, it calls
     `esp_efuse_check_secure_version(candidate.secure_version)`. If the image is
     below the burned eFuse value it is rejected outright, **regardless of
     `allow_downgrade`**, because the bootloader would refuse to boot it and
     brick the device into a boot loop. `allow_downgrade` can lower only the
     soft floor; it can never push an image past what the device's own
     bootloader will run. This is the core "cannot disagree" property, tested
     in `test_secure_floor_not_cleared_rejected_even_with_override`.
  2. **Soft semver floor.** After the hardware floor passes, the NVS semver
     floor applies as before: a below-floor image is rejected unless
     `allow_downgrade` is set, in which case the floor is lowered so the device
     is not stranded. Semver downgrades within the same secure epoch remain a
     supported, authenticated operation.

Rejecting at OTA-accept time is a UX nicety (a clean error, no wasted
download); the bootloader is the actual security boundary. Enabling one without
the other is safe but not recommended: the gate without the bootloader is just
today's soft floor, and the bootloader without the gate would let an
`allow_downgrade` request download a full image only to brick on reboot.

## 4. Default-safe posture

Enforcement is off in every shipping build. No `sdkconfig.defaults*` in the
tree sets `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK`, so:

- Dev builds, the emulator, and the QEMU/linux targets behave exactly as
  before. On the linux target `ota_rollback.c` is not even compiled
  (`components/ota/CMakeLists.txt`), and the pure policy sees
  `secure_enforced = false`, so `candidate_clears_secure_floor` is ignored and
  the decision matches the historical soft-floor-only behavior
  (`test_secure_not_enforced_ignores_clear_flag`).
- Enabling enforcement is a deliberate release decision: apply the opt-in
  overlay `sdkconfig.defaults.antirollback`, exactly as `sdkconfig.defaults.secure`
  gates flash encryption. The overlay carries the same IRREVERSIBLE warning.
- All eFuse API calls in `ota_rollback.c` are inside
  `#if CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK`, so they are absent from every
  current build and cannot affect a dev flash.

## 5. Migration for already-fielded devices (unburned eFuses)

Fielded devices today run a bootloader built WITHOUT anti-rollback and have an
unburned secure-version eFuse (value 0). Two facts govern migration:

- **The bootloader is not OTA-updated.** Anti-rollback is enforced by the
  second-stage bootloader, which a normal OTA does not replace. A fielded
  device therefore gains boot-time enforcement only after a bench reflash of a
  bootloader built with the overlay. Until then it keeps the soft NVS floor.
  This is the "human-gated hardware step" the issue calls out.
- **The ratchet is forward-only and automatic.** Once a device runs an
  anti-rollback bootloader, the first successful boot of an image with
  `secure_version = N` burns the eFuse up to `N`. There is no burn on the OTA
  download itself; it happens on the validated boot. So a migrated device moves
  its floor forward naturally as it takes higher-epoch releases, and can never
  move it back.

Recommended migration order, per board, at the bench:

1. Keep `CONFIG_BOOTLOADER_APP_SECURE_VERSION = 0` for the first anti-rollback
   build so the initial burn is a no-op ceiling (floor stays 0) and nothing is
   stranded.
2. Reflash the bootloader + app with the overlay on the sacrificial board and
   validate (section 6).
3. Only after validation, roll the anti-rollback bootloader to real boards as a
   bench operation, still at epoch 0.
4. Bump `secure_version` to 1 in a later, deliberate release once the fleet's
   bootloaders enforce it, so the first real ratchet is intentional.

Do not jump `secure_version` ahead of the fleet's bootloaders: an image at
epoch N will simply fail to boot on a device whose bootloader predates
anti-rollback is irrelevant (it ignores the field), but a device that HAS
burned its eFuse to N can never run an older epoch again, so over-bumping
permanently narrows the recovery options.

## 6. Bench validation gate (PENDING, NOT YET EXECUTED)

This procedure has NOT been run. It is written here so the owner can execute it
on a sacrificial board, exactly as the flash-encryption work (SEC-H4) was
validated before touching real fleet devices. Nothing below has been performed
by this change; the code path it exercises is compiled out until the overlay is
applied by a human.

Prerequisites: a SACRIFICIAL ESP32-S3 board that is acceptable to brick, never
a fleet device, and never the flash-encrypted V3 bench unit. Flash only via
`scripts/flash.sh` / `scripts/flash-all.py`.

Phase A: emulated eFuse dry run (no real burn).
1. Build with the overlay plus `CONFIG_BOOTLOADER_EFUSE_SECURE_VERSION_EMULATE=y`,
   which keeps the secure-version field in a flash partition instead of real
   eFuses, so the ratchet is fully reversible by erasing flash.
2. Confirm: an image at epoch 0 boots; the emulated field reads 0.
3. Build an image at `secure_version = 1`, OTA or flash it, confirm it boots and
   the emulated field ratchets to 1.
4. Attempt to install the epoch-0 image with `allow_downgrade = true`. Expect
   `ota_rollback_gate` to reject it at accept time with the eFuse-floor error
   (the "cannot disagree" property), and expect the bootloader to refuse it if
   force-flashed.
5. Erase flash to reset the emulated field between iterations.

Phase B: single real burn on the sacrificial board.
1. Rebuild without the emulate flag (real eFuses) at `secure_version = 0`.
2. Flash bootloader + app; confirm boot and that the real secure-version eFuse
   reads 0 (`espefuse.py summary`).
3. Build and install `secure_version = 1`; confirm boot and that the eFuse
   ratcheted to 1 and cannot be un-burned.
4. Confirm the epoch-0 image now fails both the OTA gate and a forced boot.

Exit criteria to promote past PENDING: phases A and B both pass on the
sacrificial board, `espefuse.py summary` output is captured, and this document's
status line and `docs/SECURITY-MODEL.md` section 4 are updated with the
verification date and board. Only then does anti-rollback move from "compiled,
host-tested" to "hardware-enforced".

## 7. What is verified vs pending

Verified (host):
- The floor reconciliation policy, including the non-overridable hardware floor,
  the soft-floor override, fail-closed on unparseable versions, and the
  secure-not-enforced pass-through (`test/test_ota_rollback_policy.c`, 13 cases).
- The OTA path still routes, gates, and fails closed with the new gate
  signature (`test/test_ota.c`).
- Semver precedence unchanged (`test/test_ota_version.c`).
- The linux/emulator firmware still builds with the gate signature change
  (`emulator/ci/run_scenarios.sh` build step).

Pending (bench, human-gated):
- The eFuse burn, the bootloader boot-time refusal, and the automatic ratchet.
  These require the overlay and a sacrificial board (section 6) and have not
  been executed.

## 8. Operator notes

Nothing changes for operators until a release ships with the overlay applied.
The `bramble.otaUpdate {path, allow_downgrade?}` surface is unchanged. When
anti-rollback is enabled, an `allow_downgrade` request that would cross below
the burned secure epoch is refused with an eFuse-floor error instead of
proceeding, because it would otherwise brick the device; a semver downgrade
within the same epoch still works. OTA failures surface under the `ota` /
`ota_rollback` log tags and as `last_error` on the next `bramble.otaUpdate`.
