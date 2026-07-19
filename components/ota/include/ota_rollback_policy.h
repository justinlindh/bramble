/**
 * @file ota_rollback_policy.h
 * @brief Pure anti-rollback decision logic, split out of ota_rollback.c.
 *
 * ota_rollback_gate() is device-only: it reads and writes the NVS-stored
 * version floor. The policy question it answers (given a candidate image
 * version, the stored floor and the caller's allow_downgrade flag, accept or
 * reject, and does the floor move?) is pure and host-testable. This header
 * exposes exactly that question so the security boundary can be covered by
 * the host suite instead of a stub.
 *
 * The policy is unchanged from the in-gate logic it replaces: fail closed on
 * an unparseable candidate, accept at or above the floor, reject below it,
 * and lower the floor only on an explicit authenticated downgrade.
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

/** True if the decision accepts the image. */
static inline bool ota_rollback_decision_accepts(ota_rollback_decision_t d) {
    return d == OTA_ROLLBACK_ACCEPT || d == OTA_ROLLBACK_ACCEPT_UNPARSEABLE ||
           d == OTA_ROLLBACK_ACCEPT_LOWER_FLOOR;
}

/**
 * Decide whether booting @p running_version should raise the stored floor.
 * Returns true when the floor should be rewritten to the running version.
 */
bool ota_rollback_should_raise_floor(const char* running_version, const char* floor_version);
