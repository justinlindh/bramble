/**
 * @file ota_rollback_policy.h
 * @brief Pure anti-rollback decision policy reconciling the soft NVS semver
 *        floor with the hardware (eFuse) secure-version floor.
 *
 * No ESP-IDF dependencies; host-testable. The device wrapper
 * (`ota_rollback.c`) reads the NVS floor, the running/candidate versions and,
 * when compiled with CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK, the eFuse
 * secure-version state, then delegates the accept/reject decision here so the
 * reconciliation logic can be exercised without hardware.
 *
 * Two independent floors exist and they must never disagree dangerously:
 *
 *   - Soft floor: an NVS-stored semver ("1.4.0"). Advisory. It only rises
 *     automatically and can be lowered by an authenticated, explicit
 *     downgrade. A flash attacker or an NVS wipe can erase it.
 *
 *   - Hardware floor: the monotonic eFuse SECURE_VERSION enforced by the
 *     bootloader (CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK). It survives an NVS
 *     wipe and a flash rewrite. An image below it is refused by the bootloader
 *     at the next boot, which would brick the device into a boot loop.
 *
 * The hardware floor is therefore absolute: allow_downgrade may lower the soft
 * floor but must never let an image through that the device's own bootloader
 * would then refuse to run.
 */

#pragma once

#include <stdbool.h>

typedef enum {
    /** Accept; no floor side effect required. */
    OTA_GATE_ACCEPT = 0,
    /** Accept a deliberate downgrade below the soft floor; the caller must
     *  lower the stored soft floor so the device is not stranded afterwards. */
    OTA_GATE_ACCEPT_DOWNGRADE,
    /** Reject: candidate version string is not parseable semver (fail closed). */
    OTA_GATE_REJECT_UNPARSEABLE,
    /** Reject: below the soft NVS floor and allow_downgrade was not set. */
    OTA_GATE_REJECT_BELOW_SOFT_FLOOR,
    /** Reject: below the hardware eFuse floor. Not overridable; the bootloader
     *  would refuse to boot this image. */
    OTA_GATE_REJECT_BELOW_SECURE_FLOOR,
} ota_gate_decision_t;

typedef struct {
    /** Candidate image version string (esp_app_desc_t.version), may be NULL. */
    const char* candidate_version;
    /** Whether a soft floor is stored; soft_floor_version is read only if so. */
    bool has_soft_floor;
    /** NVS-stored soft floor semver. Valid iff has_soft_floor. */
    const char* soft_floor_version;
    /** Caller explicitly authorized a downgrade (rides an authenticated RPC). */
    bool allow_downgrade;
    /** True only in a build compiled with CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK
     *  where the eFuse secure-version state was read successfully. */
    bool secure_enforced;
    /** Result of esp_efuse_check_secure_version(image secure_version): true if
     *  the candidate's secure_version is at or above the burned eFuse value.
     *  Ignored when secure_enforced is false. */
    bool candidate_clears_secure_floor;
} ota_gate_input_t;

/**
 * Decide whether an incoming OTA image passes both anti-rollback floors.
 *
 * Evaluation order, so the two floors cannot disagree dangerously:
 *   1. Hardware floor (if secure_enforced): reject a sub-floor image outright,
 *      regardless of allow_downgrade. This check does not depend on the semver
 *      string, so it also blocks unparseable-versioned images.
 *   2. Parse the candidate semver; unparseable is fail-closed unless
 *      allow_downgrade is set.
 *   3. Soft floor: a below-floor image is rejected unless allow_downgrade, in
 *      which case the caller lowers the soft floor (OTA_GATE_ACCEPT_DOWNGRADE).
 */
ota_gate_decision_t ota_rollback_decide(const ota_gate_input_t* in);
