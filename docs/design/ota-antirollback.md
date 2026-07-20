# OTA Anti-Rollback: eFuse Secure Version

**Status:** Software floor reconciliation, boot confirmation, and the guarded
flash flow are implemented and tested on the host (policy suite plus a
hardware-free dry run of the flash guard). eFuse enforcement itself is opt-in
via an overlay and PENDING bench validation (not yet executed on hardware; no
eFuse has been burned anywhere).
**Last verified (host):** 2026-07-19 (`test/test_ota.c` policy and
secure-floor cases, `test/test_ota_version.c`,
`bash scripts/test-antirollback-guard.sh`)
**Issue:** #79
**Related:** `docs/design/ota-signing.md`, `docs/SECURITY-MODEL.md` section 4

## 1. The gap this closes

Bramble's OTA path already verifies an RSA-3072 signature on every image and
rejects downgrades below a version floor. The floor, however, lives in NVS
(`components/ota/ota_rollback.c`), so an attacker with flash access, or any
NVS wipe, erases it and can then install a signed-but-vulnerable older build.
Closing that gap needs a floor that survives a flash rewrite and an NVS erase,
which means the ESP32-S3 eFuse secure-version field enforced by the second
stage bootloader (`CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK`).

Burning eFuses is irreversible and, like Secure Boot V2 and flash encryption
(ledger SEC-H4), is a human-gated bench step. This document specifies the
design, the deliberate enablement path, and the bench-validation procedure as
an explicit pending gate.

## 2. eFuse mechanics: what actually gets burned

Read this section before enabling anything. In plain terms:

- The ESP32-S3 has a dedicated **secure-version eFuse field, 16 bits wide**.
  eFuses are one-time-programmable hardware fuses: a bit can go from 0 to 1
  exactly once and can never be cleared again, by any software, tool, or
  reflash.
- The field is **thermometer-coded**: the device's floor is the number of bits
  set. Moving the floor from epoch N to epoch N+1 permanently burns one more
  bit. The 16-bit width is therefore a **lifetime budget of at most 16 epoch
  bumps per chip**, ever.
- With `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK`, the second-stage bootloader
  refuses to boot any app whose `secure_version` (a field in the app
  descriptor, set from `CONFIG_BOOTLOADER_APP_SECURE_VERSION` at build time)
  is below the burned floor.
- The burn is **not** performed at flash time or download time. It happens
  when a booted app **confirms itself valid** (see section 4): IDF then
  ratchets the eFuse up to the running app's `secure_version`.

Stated plainly: **if you enable this and later flash older firmware, the
device will refuse to boot it, and there is no undo.** Not over OTA, not over
USB, not with an NVS erase, not with a full flash erase. The only way forward
is firmware whose `secure_version` is at or above the burned floor.

### Ways to brick or permanently damage a device with this feature

The flash-encryption bench work (SEC-H4) taught us to write these down before
touching hardware: that campaign hit a signed-app-plus-encryption boot loop
and produced the standing rule that encrypted boards must be flashed with
`--encrypt` or they brick and lose their identity. Same honesty level here:

1. **Flashing any image below the burned floor.** The bootloader refuses to
   boot it. If every image on the device (both OTA slots, or a fresh USB
   flash) is below the floor, the device sits in a refuse-to-boot loop until
   someone flashes an image at or above the floor. If you can no longer build
   such an image (lost signing key, deleted branch), the device is bricked for
   good.
2. **Typo'ing the epoch.** Setting `CONFIG_BOOTLOADER_APP_SECURE_VERSION=10`
   when you meant `1`, then letting it boot and confirm once, permanently sets
   the floor to 10 and burns 10 of the 16 lifetime bits. Every future build
   for that device must declare `secure_version >= 10`. There is no recovery
   of the wasted bits.
3. **Exhausting the 16-bump budget.** After the 16th epoch the field is full;
   no further anti-rollback epochs are possible for the life of the chip.
4. **Mixing with flash encryption on the same board.** The V3 bench Heltec is
   flash-encrypted: it must ALWAYS be flashed with `--encrypt` or it bricks
   and loses its NVS identity, independent of this feature. Enabling
   anti-rollback on an encrypted board stacks two eFuse features; validate
   each feature alone on the sacrificial board before ever combining them, and
   never combine them for the first time on a daily-driver device.
5. **Rolling back the bootloader instead of the app.** Reflashing an older
   bootloader built WITHOUT anti-rollback does not brick the device; it does
   something arguably worse, silently: it stops enforcing the floor (the old
   bootloader never reads the field). This is also the honest statement of the
   residual threat: without Secure Boot V2 the bootloader itself is
   replaceable by a physical attacker, so eFuse anti-rollback fully closes the
   NVS-wipe and remote/API downgrade vectors and raises the cost of a physical
   downgrade to a bootloader swap. Pairing with Secure Boot V2 (separately
   human-gated) closes that too.
6. **Shipping an epoch ahead of the fleet's bootloaders.** Not a brick, but a
   trap: devices whose bootloaders predate anti-rollback ignore the field
   entirely, while any device that HAS the enforcing bootloader and confirms a
   boot of the new epoch ratchets immediately and can never go back. Bump
   epochs only after the target population's bootloaders enforce (section 6).

## 3. Two version quantities, deliberately decoupled

There are two different numbers, and conflating them would either exhaust the
eFuse field or fail to protect anything.

- **`version`** (semver string in `esp_app_desc_t.version`, e.g. `0.5.0`). Set
  from `PROJECT_VER` at build time, driven by semantic-release
  (`firmware-vX.Y.Z` tags, `.releaserc.firmware.cjs`). It moves on every
  release: a patch bump per fix, a minor per feature. This feeds the soft NVS
  floor and full semver precedence comparison (`components/ota/ota_version.c`).

- **`secure_version`** (a `uint32_t` in `esp_app_desc_t.secure_version`, set
  from `CONFIG_BOOTLOADER_APP_SECURE_VERSION`). This is a coarse, monotonic
  **security epoch counter**, not the release version, with the 16-bump
  lifetime budget from section 2.

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
the release that carries it. Because the eFuse floor only ratchets on a
confirmed boot of the higher-epoch image, a device that never takes that
release stays at its old epoch until it does.

## 4. Enforcement: boot time vs OTA-accept time

The design puts two checks in series so they cannot disagree dangerously. Both
live as pure, host-tested functions in `components/ota/ota_rollback_policy.c`:
the pre-existing soft-floor decision `ota_rollback_decide(new_version,
floor_version, allow_downgrade)`, and the hardware-floor check
`ota_rollback_secure_floor_blocks(secure_enforced, candidate_clears_secure_floor)`
added for the eFuse floor. The device wrapper `ota_rollback_gate` calls the
hardware check first, then the soft decision, with values read from NVS, the
image descriptor, and the eFuse.

- **At boot (bootloader, hardware floor).** With
  `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK`, the second-stage bootloader refuses to
  boot any app whose `secure_version` is below the burned eFuse value. This is
  the guarantee that survives a flash rewrite and an NVS wipe.

- **Boot confirmation and the ratchet moment.** Anti-rollback requires IDF's
  app-rollback state machine (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`; kconfig
  gotcha: ANTI_ROLLBACK "depends on" it rather than selecting it, so an
  overlay that omits it silently builds a normal image, which is why the
  overlay sets both). A freshly OTA'd image boots in pending-verify state and
  would revert on the next reboot unless the app confirms itself operable.
  Bramble confirms in `ota_rollback_note_boot()` (called from the main
  bring-up path after NVS init) via
  `esp_ota_mark_app_valid_cancel_rollback()`, compile-guarded so it is absent
  from non-rollback builds. That confirmation is also the moment IDF ratchets
  the eFuse floor up to the running app's `secure_version`; until then the
  bootloader can still fall back to the previous app, so a higher-epoch image
  that fails to reach bring-up does not strand the device.

- **At OTA-accept time (`ota_rollback_gate`, both floors).** Before committing
  airtime to the download, the gate reconciles:
  1. **Hardware floor first.** When compiled with anti-rollback, it calls
     `esp_efuse_check_secure_version(candidate.secure_version)`. If the image is
     below the burned eFuse value it is rejected outright, **regardless of
     `allow_downgrade`**, because the bootloader would refuse to boot it.
     `allow_downgrade` can lower only the soft floor; it can never push an
     image past what the device's own bootloader will run. This is the core
     "cannot disagree" property, tested in
     `test_secure_floor_blocks_only_a_sub_floor_image_when_enforced`
     (`test/test_ota.c`).
  2. **Soft semver floor.** After the hardware floor passes, the NVS semver
     floor applies as before: a below-floor image is rejected unless
     `allow_downgrade` is set, in which case the floor is lowered so the device
     is not stranded. Semver downgrades within the same secure epoch remain a
     supported, authenticated operation.

Rejecting at OTA-accept time is a UX nicety (a clean error, no wasted
download); the bootloader is the actual security boundary.

## 5. Enabling it deliberately: the guarded flash flow

Enablement is intentional at every step; no default flow ever carries a
secure-version burn onto a device silently.

1. **Opt-in overlay.** No `sdkconfig.defaults*` in the tree sets
   `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK`. Enforcement exists only in a build
   made with the overlay, exactly as `sdkconfig.defaults.secure` gates flash
   encryption:
   `idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.antirollback" build`.
   Dev builds, the emulator, and the QEMU/linux targets are untouched: on the
   linux target `ota_rollback.c` is not even compiled, and the pure policy
   sees `secure_enforced = false`
   (`test_secure_not_enforced_ignores_clear_flag`).
2. **Loud build surface.** `scripts/flash.sh` runs
   `scripts/antirollback-guard.sh` after every build; an anti-rollback build
   prints an unmissable banner naming the epoch and the irreversibility, even
   for a plain `build` action (building burns nothing, so it is allowed).
3. **Flash requires an explicit flag plus a typed confirmation.** The only
   sanctioned flash paths are `scripts/flash.sh` and `scripts/flash-all.py`.
   Both refuse to flash an anti-rollback build (guard exit 2) unless
   `--enable-antirollback` is passed, and then additionally require the
   operator to type, exactly, `BURN EPOCH <N>` where N is the image's
   `secure_version`. Anything else, including EOF on stdin, refuses. A
   non-anti-rollback build passes the guard silently, so the everyday flow is
   byte-for-byte unchanged.
4. **The ratchet is its own consent.** Because the phrase embeds the epoch,
   flashing a build that moves an already-enabled device to a HIGHER epoch is
   a fresh typed decision (`BURN EPOCH 2` is not `BURN EPOCH 1`). Before
   prompting, the guard shows the device's current secure-version eFuse state,
   read best-effort via `espefuse.py` when a port is available, and states it
   as UNKNOWN otherwise, so the operator sees old floor to new floor before
   consenting.
5. **Fleet tooling inherits the gate.** `scripts/flash-all.py` inspects every
   built board config; anti-rollback boards are refused outright (USB and OTA
   phases both) without its `--enable-antirollback` flag, and with it the
   operator types the epoch phrase once, upfront and interactively, before any
   parallel work starts; the confirmation is then replayed to each
   `flash.sh` invocation.
6. **Emulated dry-run mode.** A build that adds
   `CONFIG_BOOTLOADER_EFUSE_SECURE_VERSION_EMULATE=y` keeps the field in a
   flash partition instead of real eFuses (reversible by erasing flash). The
   guard runs the same ceremony but labels the mode EMULATED explicitly. This
   is how the bench procedure rehearses before the one real burn.

The guard logic is tested without hardware by
`bash scripts/test-antirollback-guard.sh` (18 cases: silent pass-through,
refusals, wrong phrase, wrong epoch, EOF, emulated labeling, fail-closed on a
missing config).

## 6. Migration for already-fielded devices (unburned eFuses)

Fielded devices today run a bootloader built WITHOUT anti-rollback and have an
unburned secure-version eFuse (value 0). Two facts govern migration:

- **The bootloader is not OTA-updated.** Anti-rollback is enforced by the
  second-stage bootloader, which a normal OTA does not replace. A fielded
  device therefore gains boot-time enforcement only after a bench reflash of a
  bootloader built with the overlay, through the guarded flow above. Until
  then it keeps the soft NVS floor.
- **The ratchet is forward-only and automatic after that.** Once a device runs
  an anti-rollback bootloader, each confirmed boot of a higher-epoch image
  burns the eFuse up to that epoch. It can never move back.

Recommended migration order, per board, at the bench:

1. Keep `CONFIG_BOOTLOADER_APP_SECURE_VERSION = 0` for the first anti-rollback
   build so the initial state is a no-op floor and nothing is stranded.
2. Validate on the sacrificial board first (section 7), in order, before any
   real device.
3. Only after validation, roll the anti-rollback bootloader to real boards as
   a bench operation, still at epoch 0, one board at a time.
4. Bump `secure_version` to 1 in a later, deliberate release once the fleet's
   bootloaders enforce it, so the first real ratchet is intentional.

Do not jump `secure_version` ahead of the fleet's bootloaders (section 2,
hazard 6).

## 7. Bench validation gate (PENDING, NOT YET EXECUTED)

This procedure has NOT been run. It is written here so the owner can execute
it on a sacrificial board, exactly as the flash-encryption work (SEC-H4) was
validated before touching real fleet devices. Nothing below has been performed
by this change; no eFuse has been burned.

Prerequisites: a SACRIFICIAL ESP32-S3 board that is acceptable to brick, never
a fleet device, and never the flash-encrypted V3 bench unit. Flash only via
`scripts/flash.sh` / `scripts/flash-all.py` (the guard is part of the
procedure). The steps deliberately walk enable, verify boot, attempt
downgrade, observe refusal, in that order; do not reorder them, and do not run
any of this on a daily-driver board.

Phase A: emulated eFuse dry run (no real burn, reversible).
1. **Enable (emulated).** Build with the overlay plus
   `CONFIG_BOOTLOADER_EFUSE_SECURE_VERSION_EMULATE=y` at epoch 0. Flash via
   `scripts/flash.sh ... --enable-antirollback`; confirm the guard demands and
   accepts `BURN EPOCH 0` and labels the mode EMULATED.
2. **Verify boot.** Confirm the image boots, `ota_rollback` logs the boot
   confirmation, and the emulated field reads 0.
3. **Ratchet.** Build epoch 1, install it (guard phrase `BURN EPOCH 1`),
   confirm it boots and the emulated field ratchets to 1 after confirmation.
4. **Attempt downgrade, observe refusal.** Try to install the epoch-0 image
   with `allow_downgrade = true` over OTA: expect `ota_rollback_gate` to
   reject at accept time with the eFuse-floor error (the "cannot disagree"
   property). Then force-flash the epoch-0 image and expect the bootloader to
   refuse to boot it.
5. Erase flash to reset the emulated field between iterations.

Phase B: single real burn on the sacrificial board.
1. **Enable (real).** Rebuild without the emulate flag at epoch 0; flash
   through the guard. Confirm boot and that the real secure-version eFuse
   reads 0 (`espefuse.py summary`).
2. **Verify boot, then ratchet.** Build and install epoch 1; confirm boot,
   then confirm the eFuse ratcheted to 1 and cannot be un-burned.
3. **Attempt downgrade, observe refusal.** Confirm the epoch-0 image now fails
   the OTA gate, fails a guarded flash-and-boot, and that nothing (flash
   erase, NVS erase, reflash) restores the ability to run epoch 0.

Exit criteria to promote past PENDING: phases A and B both pass on the
sacrificial board, `espefuse.py summary` output is captured, and this
document's status line and `docs/SECURITY-MODEL.md` section 4 are updated with
the verification date and board. Only then does anti-rollback move from
"compiled, host-tested, guard-tested" to "hardware-enforced".

## 8. What is verified vs pending

Verified (host, no hardware):
- The soft-floor decision (`ota_rollback_decide`) and floor-raise logic, and
  the hardware-floor helper (`ota_rollback_secure_floor_blocks`) proving the
  non-overridable rejection and the secure-not-enforced pass-through
  (`test/test_ota.c` policy and secure-floor cases).
- The OTA path still routes, gates, and fails closed with the new gate
  signature (`test/test_ota.c`).
- Semver precedence unchanged (`test/test_ota_version.c`).
- The flash-guard consent logic, hardware-free
  (`bash scripts/test-antirollback-guard.sh`, 18 cases).
- The firmware builds for the linux/emulator target, for the default esp32s3
  profile, and for esp32s3 WITH the anti-rollback overlay applied (the
  generated config was positively checked to contain the anti-rollback
  symbols, compiling the eFuse-guarded code paths).

Pending (bench, human-gated):
- The eFuse burn, the bootloader boot-time refusal, the ratchet-on-confirm
  behavior, and the guard's espefuse floor display against a real device.
  These require the overlay and a sacrificial board (section 7) and have not
  been executed.

## 9. Operator notes

Nothing changes for operators until a release ships with the overlay applied.
The `bramble.otaUpdate {path, allow_downgrade?}` surface is unchanged. When
anti-rollback is enabled, an `allow_downgrade` request that would cross below
the burned secure epoch is refused with an eFuse-floor error instead of
proceeding, because it would otherwise brick the device; a semver downgrade
within the same epoch still works. OTA failures surface under the `ota` /
`ota_rollback` log tags and as `last_error` on the next `bramble.otaUpdate`.
