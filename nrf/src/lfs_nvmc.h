// LittleFS block device over the nRF52840 NVMC; see lfs_nvmc.c for the
// partial-erase rationale.
#pragma once

#include <stdint.h>

#include "lfs.h"

/* Cache size shared by the block device config and the per-file buffers the
 * NVS shim supplies (LFS_NO_MALLOC). */
#define LFS_NVMC_CACHE_SIZE 256

// Fills cfg with the NVMC block-device callbacks and the geometry for a
// partition of `size` bytes at flash address `base` (both page-aligned).
void lfs_nvmc_config_init(struct lfs_config* cfg, uint32_t base, uint32_t size);

int lfs_nvmc_read(const struct lfs_config* c, lfs_block_t block, lfs_off_t off, void* buffer,
                  lfs_size_t size);
int lfs_nvmc_prog(const struct lfs_config* c, lfs_block_t block, lfs_off_t off, const void* buffer,
                  lfs_size_t size);
int lfs_nvmc_erase(const struct lfs_config* c, lfs_block_t block);
int lfs_nvmc_sync(const struct lfs_config* c);
