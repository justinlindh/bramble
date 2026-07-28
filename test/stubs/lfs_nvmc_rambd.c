/*
 * Host-side stand-in for the nRF NVMC block device: the same
 * lfs_nvmc_config_init entry point the flash-backed NVS shim calls, backed by
 * littlefs's own RAM block device. That lets test_nvs_lfs_shim exercise the
 * real nvs_lfs.c logic (paths, type prefixes, iterators, error codes) without
 * flash hardware; the NVMC driver itself is bench-verified, not host-tested.
 */
#include <string.h>

#include "bd/lfs_rambd.h"
#include "lfs_nvmc.h"

/* LFS_NO_MALLOC is kept on for fidelity with the target build, so the RAM
 * disk gets a static backing store rather than allocating one. */
#define HOST_BD_BYTES (0x2D000)
static uint8_t s_disk[HOST_BD_BYTES];
static lfs_rambd_t s_bd;
static struct lfs_rambd_config s_bdcfg;
static uint8_t s_read_buffer[LFS_NVMC_CACHE_SIZE];
static uint8_t s_prog_buffer[LFS_NVMC_CACHE_SIZE];
static uint32_t s_lookahead[32 / sizeof(uint32_t)];

void lfs_nvmc_config_init(struct lfs_config* cfg, uint32_t base, uint32_t size) {
    (void)base;
    memset(cfg, 0, sizeof(*cfg));
    memset(&s_bd, 0, sizeof(s_bd));
    memset(&s_bdcfg, 0, sizeof(s_bdcfg));

    cfg->context = &s_bd;
    cfg->read = lfs_rambd_read;
    cfg->prog = lfs_rambd_prog;
    cfg->erase = lfs_rambd_erase;
    cfg->sync = lfs_rambd_sync;

    cfg->read_size = 4;
    cfg->prog_size = 4;
    cfg->block_size = 4096;
    cfg->block_count = size / 4096;
    cfg->block_cycles = 200;
    cfg->cache_size = LFS_NVMC_CACHE_SIZE;
    cfg->lookahead_size = 32;
    cfg->read_buffer = s_read_buffer;
    cfg->prog_buffer = s_prog_buffer;
    cfg->lookahead_buffer = s_lookahead;

    s_bdcfg.read_size = cfg->read_size;
    s_bdcfg.prog_size = cfg->prog_size;
    s_bdcfg.erase_size = cfg->block_size;
    s_bdcfg.erase_count = cfg->block_count;
    s_bdcfg.buffer = s_disk;
    /* A fresh format each time: erased flash reads as 0xFF. */
    memset(s_disk, 0xFF, sizeof(s_disk));
    lfs_rambd_create(cfg, &s_bdcfg);
}
