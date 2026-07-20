#include "ota_rollback_policy.h"

#include "ota_version.h"

ota_rollback_decision_t ota_rollback_decide(const char* new_version, const char* floor_version,
                                            bool allow_downgrade) {
    ota_semver_t candidate;
    if (!new_version || !ota_version_parse(new_version, &candidate)) {
        return allow_downgrade ? OTA_ROLLBACK_ACCEPT_UNPARSEABLE : OTA_ROLLBACK_REJECT_UNPARSEABLE;
    }

    ota_semver_t floor;
    if (!floor_version || !ota_version_parse(floor_version, &floor)) {
        return OTA_ROLLBACK_ACCEPT; /* no floor recorded yet */
    }

    /* Equal to the floor is an accept: reinstalling the running version is a
     * legitimate repair path, and only a strictly lower version is a rollback. */
    if (ota_version_cmp(&candidate, &floor) >= 0) {
        return OTA_ROLLBACK_ACCEPT;
    }

    return allow_downgrade ? OTA_ROLLBACK_ACCEPT_LOWER_FLOOR : OTA_ROLLBACK_REJECT_BELOW_FLOOR;
}

bool ota_rollback_secure_floor_blocks(bool secure_enforced, bool candidate_clears_secure_floor) {
    return secure_enforced && !candidate_clears_secure_floor;
}

bool ota_rollback_should_raise_floor(const char* running_version, const char* floor_version) {
    ota_semver_t running;
    if (!running_version || !ota_version_parse(running_version, &running)) {
        return false;
    }

    ota_semver_t floor;
    if (!floor_version || !ota_version_parse(floor_version, &floor)) {
        return true; /* nothing usable stored: record the running version */
    }

    /* Strictly higher only: an equal floor is already correct. */
    return ota_version_cmp(&running, &floor) > 0;
}
