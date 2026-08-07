/*
 * nvs_fake: an in-memory NVS that actually stores things.
 *
 * esp_stubs.c's NVS symbols are no-ops, which is right for targets that only
 * need the calls to link. Testing a store means reading back what was
 * written, surviving a "reboot" (the process keeps the flash contents while
 * the module's statics are reset), and iterating a namespace's directory, so
 * those need a real implementation. A target links either this file or
 * esp_stubs.c's NVS_STUBS_ENABLE block, never both.
 */
#ifndef NVS_FAKE_H
#define NVS_FAKE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Wipe every namespace: a factory-fresh device. */
void nvs_fake_reset(void);

/* Make nvs_open fail, standing in for a partition that is not there yet. */
void nvs_fake_set_open_fails(bool fails);

/* Direct access for test setup and assertions, bypassing the handle API. */
void nvs_fake_put_blob(const char* ns, const char* key, const void* value, size_t len);
bool nvs_fake_read_blob(const char* ns, const char* key, void* out, size_t* len);
void nvs_fake_put_u32(const char* ns, const char* key, uint32_t value);
bool nvs_fake_read_u32(const char* ns, const char* key, uint32_t* out);

/* How many entries a namespace holds, for cap and cleanup assertions. */
int nvs_fake_count(const char* ns);

#endif /* NVS_FAKE_H */
