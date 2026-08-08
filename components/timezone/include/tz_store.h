#ifndef BRAMBLE_TZ_STORE_H
#define BRAMBLE_TZ_STORE_H

#include <stdbool.h>
#include <stddef.h>

#include "bramble_tz.h"

/*
 * Persistence for the node's configured zone.
 *
 * The stored value is a POSIX TZ specification (see bramble_tz.h). A node with
 * nothing stored, or with a stored value that no longer parses, reports
 * BRAMBLE_TZ_DEFAULT_SPEC, so the clock shows real UTC rather than a guess.
 */

/* Copy the effective zone spec into out, always NUL-terminating. out_len
 * should be at least BRAMBLE_TZ_SPEC_MAX. */
void tz_store_get(char* out, size_t out_len);

/* Validate and persist a zone spec.
 * Returns 0 on success, -1 when spec is not a valid POSIX TZ specification,
 * -2 on an NVS failure. */
int tz_store_set(const char* spec);

/* True when a zone is stored in NVS, as opposed to falling back to the
 * default. */
bool tz_store_is_configured(void);

#endif /* BRAMBLE_TZ_STORE_H */
