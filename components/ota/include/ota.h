#ifndef BRAMBLE_OTA_H
#define BRAMBLE_OTA_H

#include <stdbool.h>

/**
 * Start an OTA update from an already-resolved, policy-checked URL
 * (see ota_origin.h / ota_url.h; RPC callers never pass raw URLs here).
 *
 * The image must carry a valid RSA-3072 signature block trusted by the
 * running app (CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT) and must pass
 * the soft anti-rollback gate unless allow_downgrade is set.
 *
 * Returns 0 on success. On failure, ota_get_last_error() describes why.
 */
int ota_wifi_start(const char* url, bool allow_downgrade);

/** Human-readable reason for the most recent OTA failure, or NULL. */
const char* ota_get_last_error(void);

/** Get the label of the running OTA partition (e.g. "app0"). */
const char* ota_get_running_partition(void);

/** Get the firmware version string. */
const char* ota_get_app_version(void);

#endif
