/**
 * @file ota_rollback.h
 * @brief Anti-rollback floors for OTA updates: soft NVS floor plus an optional
 *        hardware eFuse floor.
 *
 * The soft floor is an NVS-stored version that only ever rises automatically
 * (on boot of a higher version) and can be lowered solely through an
 * authenticated, explicit downgrade request. A physical-flash attacker can
 * erase NVS; that residual is closed by the hardware floor.
 *
 * The hardware floor (compiled in only with CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK,
 * an opt-in overlay, see docs/design/ota-antirollback.md) is the ESP32-S3
 * eFuse secure-version field the bootloader enforces at boot. It survives a
 * flash rewrite and an NVS wipe. Enabling it is a deliberate, bench-gated
 * hardware step and is off in every shipping build.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Record the running firmware version as the new floor if it is higher than
 * the stored one (or if no floor is stored). Call once at boot after NVS init.
 * In an anti-rollback build this also confirms the running image valid, which
 * cancels the pending-verify rollback and lets the bootloader ratchet the
 * eFuse secure-version floor up to the running app.
 */
void ota_rollback_note_boot(void);

/**
 * Gate an incoming OTA image against both anti-rollback floors.
 *
 * Returns 0 to accept, -1 to reject. The soft NVS floor is fail-closed: an
 * unparseable candidate version is rejected unless allow_downgrade is set, and
 * an accepted below-floor downgrade lowers the floor so the device is not
 * stranded afterwards. The hardware eFuse floor (active only in a build
 * compiled with CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK) is checked first using
 * candidate_secure_version (the image descriptor's secure_version): a
 * below-floor image is rejected regardless of allow_downgrade, because the
 * bootloader would otherwise refuse to boot it and brick the device.
 */
int ota_rollback_gate(const char* new_version, uint32_t candidate_secure_version,
                      bool allow_downgrade);

/**
 * Copy the stored floor version into out. Returns true if a floor is stored.
 */
bool ota_rollback_get_floor(char* out, size_t out_len);
