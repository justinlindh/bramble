/**
 * @file ota_rollback_policy.h
 * @brief Pure anti-rollback decision logic behind ota_rollback.c's gate.
 *
 * ota_rollback_gate() is device-only: it reads and writes the NVS-stored
 * version floor. The policy question it answers (given a candidate image
 * version, the stored floor and the caller's allow_downgrade flag, accept or
 * reject, and does the floor move?) is pure and host-testable. This header
 * exposes exactly that question so the security boundary can be covered by
 * the host suite instead of a stub.
 *
 * The policy: fail closed on an unparseable candidate, accept at or above the
 * floor, reject below it, and lower the floor only on an explicit
 * authenticated downgrade.
 */

#pragma once

#include <stdbool.h>

typedef enum {
    /** Accept, floor unchanged: at or above the floor, or no usable floor. */
    OTA_ROLLBACK_ACCEPT,
    /** Accept an unparseable candidate version because allow_downgrade is set. */
    OTA_ROLLBACK_ACCEPT_UNPARSEABLE,
    /** Accept below the floor because allow_downgrade is set; lower the floor. */
    OTA_ROLLBACK_ACCEPT_LOWER_FLOOR,
    /** Reject: candidate version is not semver and allow_downgrade is not set. */
    OTA_ROLLBACK_REJECT_UNPARSEABLE,
    /** Reject: candidate version is below the floor and allow_downgrade is not set. */
    OTA_ROLLBACK_REJECT_BELOW_FLOOR,
} ota_rollback_decision_t;

/**
 * Decide whether an OTA candidate version clears the anti-rollback floor.
 *
 * @param new_version   Candidate image version. NULL counts as unparseable.
 * @param floor_version Stored floor, or NULL when none is recorded. A floor
 *                      that fails to parse is treated as no floor: the floor
 *                      is only ever written by this firmware, so garbage there
 *                      means "not set yet", and gating on it would brick OTA.
 * @param allow_downgrade Caller explicitly authorised a downgrade.
 */
ota_rollback_decision_t ota_rollback_decide(const char* new_version, const char* floor_version,
                                            bool allow_downgrade);

/**
 * Decide whether booting @p running_version should raise the stored floor.
 * Returns true when the floor should be rewritten to the running version.
 */
bool ota_rollback_should_raise_floor(const char* running_version, const char* floor_version);

/**
 * Hardware (eFuse) anti-rollback floor check, kept pure for host testing.
 *
 * The soft floor above lives in NVS and an authenticated allow_downgrade can
 * lower it. The hardware floor is different: in a build compiled with
 * CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK the bootloader refuses to boot an app
 * whose secure_version is below the burned eFuse value, and that value
 * survives a flash rewrite or an NVS wipe.
 *
 * Returns true when an incoming image must be REJECTED because of the hardware
 * floor, i.e. enforcement is compiled in and the candidate does not clear it.
 * This rejection is absolute: unlike the soft floor it is never overridable by
 * allow_downgrade, because letting a sub-floor image install would only brick
 * the device on the next boot. Evaluate it BEFORE the soft-floor decision so
 * the two floors can never disagree dangerously.
 *
 * @param secure_enforced               True only in a build compiled with
 *                                      CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK.
 * @param candidate_clears_secure_floor Result of
 *                                      esp_efuse_check_secure_version(image
 *                                      secure_version); ignored when
 *                                      secure_enforced is false.
 */
bool ota_rollback_secure_floor_blocks(bool secure_enforced, bool candidate_clears_secure_floor);
