/**
 * @file ota_version.h
 * @brief Pure semantic-version parsing and comparison for OTA anti-rollback.
 *
 * No ESP-IDF dependencies; host-testable. Used by the soft anti-rollback
 * floor: an incoming OTA image whose version compares lower than the
 * NVS-stored floor is rejected unless the caller explicitly allows a
 * downgrade.
 */

#pragma once

#include <stdbool.h>

#define OTA_VERSION_PRERELEASE_MAX 48

typedef struct {
    int major;
    int minor;
    int patch;
    /** Empty string for a release version. */
    char prerelease[OTA_VERSION_PRERELEASE_MAX];
} ota_semver_t;

/**
 * Parse "1.4.0", "v1.4.0" or "1.4.6-dev.3.gabc123" into out.
 * Build metadata ("+...") is ignored. Returns false on malformed input.
 */
bool ota_version_parse(const char* s, ota_semver_t* out);

/**
 * Semver precedence comparison: returns <0, 0 or >0.
 * A release version outranks any prerelease of the same core version.
 */
int ota_version_cmp(const ota_semver_t* a, const ota_semver_t* b);

/**
 * Convenience string comparison. Returns true and sets *cmp on success;
 * returns false if either string fails to parse.
 */
bool ota_version_cmp_str(const char* a, const char* b, int* cmp);
