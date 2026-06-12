/**
 * @file ota_url.h
 * @brief Pure OTA URL policy: origin allowlisting and artifact path resolution.
 *
 * No ESP-IDF dependencies; host-testable. The RPC layer never passes a raw
 * caller-supplied URL to the OTA engine. It resolves a relative artifact path
 * against the device's configured OTA origin and these functions enforce the
 * policy.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

/** Maximum length of a fully resolved OTA URL, including NUL. */
#define OTA_URL_MAX 256

/** ota_url_resolve() error codes. */
#define OTA_URL_OK 0
#define OTA_URL_ERR_ORIGIN (-1)
#define OTA_URL_ERR_PATH (-2)
#define OTA_URL_ERR_TOOLONG (-3)

/**
 * Validate an OTA origin (base URL).
 *
 * Accepts only "https://host[:port][/path...]" (and "http://..." when
 * allow_http is true, for the CONFIG_BRAMBLE_OTA_ALLOW_HTTP dev loop).
 * Rejects: empty/missing host, userinfo ('@'), query ('?'), fragment ('#'),
 * backslashes, whitespace/control characters, and characters outside
 * [A-Za-z0-9._~:/-] after the scheme.
 */
bool ota_url_origin_valid(const char* origin, bool allow_http);

/**
 * Validate a relative artifact path (e.g. "stable/v1.4.0/heltec-v3/bramble.bin").
 *
 * Allowlisted charset [A-Za-z0-9._+-] with '/' separators. Rejects: empty,
 * leading or trailing '/', empty segments ("//"), "." or ".." segments, any
 * ':' (kills scheme smuggling), '%' (percent-encoding tricks), '?', '#', '@',
 * '\\', whitespace and control characters.
 */
bool ota_url_path_valid(const char* path);

/**
 * Resolve origin + relative path into out (inserting '/' when needed).
 * Returns OTA_URL_OK or a negative OTA_URL_ERR_* code. On error, out is set
 * to the empty string.
 */
int ota_url_resolve(const char* origin, const char* path, bool allow_http, char* out,
                    size_t out_len);
