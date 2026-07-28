/**
 * @file bramble_storage.h
 * @brief Does this build have a persistent NVS store?
 *
 * Components that persist settings fork between a real NVS backend and an
 * in-memory host stand-in. Spelling that fork as a list of platform names
 * (`#ifdef ESP_PLATFORM`) silently gave the nRF target the host branch, so
 * identity and channel state lived in RAM on a device with a filesystem: the
 * node regenerated its identity every boot with no error anywhere. The
 * condition those files actually mean is "an nvs.h implementation exists
 * here", so name that once and let each platform opt in.
 */
#pragma once

#if defined(ESP_PLATFORM) || defined(BRAMBLE_PLATFORM_NRF)
#define BRAMBLE_HAS_NVS 1
#endif
