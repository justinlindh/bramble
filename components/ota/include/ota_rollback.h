/**
 * @file ota_rollback.h
 * @brief Soft anti-rollback: NVS-stored version floor for OTA updates.
 *
 * Without burned eFuses there is no hardware anti-rollback; this is the
 * software approximation. The floor only ever rises automatically (on boot of
 * a higher version) and can be lowered solely through an authenticated,
 * explicit downgrade request. A physical-flash attacker can erase NVS; that
 * residual is accepted until hardware Secure Boot lands.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Record the running firmware version as the new floor if it is higher than
 * the stored one (or if no floor is stored). Call once at boot after NVS init.
 */
void ota_rollback_note_boot(void);

/**
 * Gate an incoming OTA image against both anti-rollback floors.
 *
 * Returns 0 to accept, -1 to reject. Two floors are reconciled (see
 * ota_rollback_policy.h):
 *
 *   - The soft NVS semver floor. Fail-closed: an unparseable candidate version
 *     is rejected unless allow_downgrade is set. When allow_downgrade accepts a
 *     version below the soft floor, the floor is lowered to that version so the
 *     device is not stranded under a stale floor after the deliberate
 *     downgrade.
 *
 *   - The hardware eFuse secure-version floor, active only in a build compiled
 *     with CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK. candidate_secure_version is the
 *     image descriptor's secure_version. An image below the burned eFuse value
 *     is rejected regardless of allow_downgrade, because the bootloader would
 *     otherwise refuse to boot it and brick the device.
 */
int ota_rollback_gate(const char* new_version, uint32_t candidate_secure_version,
                      bool allow_downgrade);

/**
 * Copy the stored floor version into out. Returns true if a floor is stored.
 */
bool ota_rollback_get_floor(char* out, size_t out_len);
