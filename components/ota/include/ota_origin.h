/**
 * @file ota_origin.h
 * @brief NVS-backed OTA origin allowlist and artifact path resolution.
 *
 * The device only ever downloads firmware from its configured OTA origin.
 * The origin defaults to the official update server and can be changed only
 * through an authenticated RPC. otaUpdate callers supply a relative artifact
 * path; the raw-URL parameter is gone.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

/** Official OTA origin; used whenever no override is stored in NVS. */
#define OTA_DEFAULT_ORIGIN "https://bramblemesh.org/ota/"

/**
 * Copy the effective OTA origin into out. Falls back to OTA_DEFAULT_ORIGIN
 * when no valid override is stored. Always NUL-terminates.
 */
void ota_origin_get(char* out, size_t out_len);

/**
 * Validate and persist an origin override. Returns 0 on success, -1 if the
 * origin fails policy validation (see ota_url_origin_valid; http:// is only
 * accepted when CONFIG_BRAMBLE_OTA_ALLOW_HTTP), -2 on NVS failure.
 */
int ota_origin_set(const char* origin);

/** Remove the override and return to OTA_DEFAULT_ORIGIN. 0 on success. */
int ota_origin_reset(void);

/** True when an override is stored in NVS. */
bool ota_origin_is_overridden(void);

/**
 * Resolve a relative artifact path against the effective origin.
 * Returns OTA_URL_OK or a negative OTA_URL_ERR_* code (see ota_url.h).
 */
int ota_resolve_artifact(const char* rel_path, char* out, size_t out_len);
