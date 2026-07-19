#include "ota_rollback_policy.h"

#include "ota_version.h"

ota_gate_decision_t ota_rollback_decide(const ota_gate_input_t* in) {
    if (!in) {
        return OTA_GATE_REJECT_UNPARSEABLE;
    }

    /* Hardware floor first. It is absolute: an image whose secure_version is
     * below the burned eFuse value would be refused by the bootloader at the
     * next boot and strand the device in a boot loop, so it must be rejected
     * even when the caller passed allow_downgrade. allow_downgrade lowers only
     * the soft floor; it can never push an image past what the device's own
     * bootloader will run. This check is independent of the semver string, so
     * it also blocks an unparseable-versioned image that fails the hw floor. */
    if (in->secure_enforced && !in->candidate_clears_secure_floor) {
        return OTA_GATE_REJECT_BELOW_SECURE_FLOOR;
    }

    ota_semver_t candidate;
    if (!in->candidate_version || !ota_version_parse(in->candidate_version, &candidate)) {
        /* Fail closed on an unparseable version unless a downgrade was
         * explicitly authorized (preserves the historical soft-floor behavior;
         * the hw floor above has already been cleared at this point). */
        return in->allow_downgrade ? OTA_GATE_ACCEPT : OTA_GATE_REJECT_UNPARSEABLE;
    }

    /* Soft floor: advisory, and only meaningful within a single secure epoch.
     * A malformed stored floor is treated as "no floor" (accept), matching the
     * NVS wrapper which ignores a floor it cannot parse. */
    if (in->has_soft_floor) {
        ota_semver_t floor;
        if (ota_version_parse(in->soft_floor_version, &floor) &&
            ota_version_cmp(&candidate, &floor) < 0) {
            return in->allow_downgrade ? OTA_GATE_ACCEPT_DOWNGRADE
                                       : OTA_GATE_REJECT_BELOW_SOFT_FLOOR;
        }
    }

    return OTA_GATE_ACCEPT;
}
