/*
 * LittleFS block device on the nRF52840's internal flash (NVMC).
 *
 * The one non-obvious thing here is erase. A full 4KB page erase takes ~85ms
 * during which the flash is unreadable and the CPU stalls, which would wreck
 * both radios (the LoRa IRQ path and, from P2 on, the BLE link layer's
 * anchor points). The NVMC's ERASEPAGEPARTIAL feature exists exactly for
 * this: erase is issued in short slices with the CPU free in between, so we
 * slice at LFS_ERASE_SLICE_MS and yield to the scheduler between slices.
 * Nothing in Mynewt/NimBLE coordinates flash with the link layer for us
 * (verified: their hal_flash is a plain blocking nrfx wrapper), so the
 * slicing IS the coordination.
 *
 * Reads are plain memcpy: internal flash is memory-mapped.
 */
#include "lfs_nvmc.h"

#include <string.h>

#include <FreeRTOS.h>
#include <task.h>

#include <nrfx_nvmc.h>

#include "esp_log.h"
#include "lfs.h"

static const char* TAG = "lfs_nvmc";

/* Slice length for a partial page erase. The NVMC's granularity is ~1.05ms
 * per configured ms; 2ms keeps each stall comfortably shorter than a BLE
 * connection interval and far shorter than a LoRa symbol at SF9. */
#define LFS_ERASE_SLICE_MS 2

/* One page erase is ~85ms of erase time total, so bound the slice loop well
 * above that before declaring the page dead. */
#define LFS_ERASE_MAX_SLICES 200

static uint32_t s_base;
static uint32_t s_size;

int lfs_nvmc_read(const struct lfs_config* c, lfs_block_t block, lfs_off_t off, void* buffer,
                  lfs_size_t size) {
    (void)c;
    const void* src = (const void*)(uintptr_t)(s_base + block * c->block_size + off);
    memcpy(buffer, src, size);
    return LFS_ERR_OK;
}

int lfs_nvmc_prog(const struct lfs_config* c, lfs_block_t block, lfs_off_t off, const void* buffer,
                  lfs_size_t size) {
    uint32_t addr = s_base + block * c->block_size + off;
    /* prog_size is 4 and littlefs honors it, so this is always word-aligned
     * and word-sized; a word write is ~41us and needs no slicing. */
    nrfx_nvmc_words_write(addr, buffer, size / sizeof(uint32_t));
    while (!nrfx_nvmc_write_done_check()) {
    }
    return LFS_ERR_OK;
}

int lfs_nvmc_erase(const struct lfs_config* c, lfs_block_t block) {
    uint32_t addr = s_base + block * c->block_size;
    if (nrfx_nvmc_page_partial_erase_init(addr, LFS_ERASE_SLICE_MS) != NRFX_SUCCESS) {
        ESP_LOGE(TAG, "partial erase init failed at 0x%08lx", (unsigned long)addr);
        return LFS_ERR_IO;
    }
    for (int i = 0; i < LFS_ERASE_MAX_SLICES; i++) {
        if (nrfx_nvmc_page_partial_erase_continue()) {
            return LFS_ERR_OK; /* page fully erased */
        }
        /* Let the radios and the mesh run between slices. */
        taskYIELD();
    }
    ESP_LOGE(TAG, "page erase did not complete at 0x%08lx", (unsigned long)addr);
    return LFS_ERR_IO;
}

int lfs_nvmc_sync(const struct lfs_config* c) {
    (void)c;
    /* Writes complete synchronously above; nothing is buffered in hardware. */
    return LFS_ERR_OK;
}

void lfs_nvmc_config_init(struct lfs_config* cfg, uint32_t base, uint32_t size) {
    s_base = base;
    s_size = size;

    memset(cfg, 0, sizeof(*cfg));
    cfg->read = lfs_nvmc_read;
    cfg->prog = lfs_nvmc_prog;
    cfg->erase = lfs_nvmc_erase;
    cfg->sync = lfs_nvmc_sync;

    cfg->read_size = 4;
    cfg->prog_size = 4;
    cfg->block_size = 4096; /* nRF52840 flash page */
    cfg->block_count = size / 4096;
    /* Metadata blocks are rewritten this many times before littlefs evicts
     * them to a fresh block; 200 balances wear against churn on a partition
     * this small. Flash endurance is 10k cycles/page. */
    cfg->block_cycles = 200;
    cfg->cache_size = LFS_NVMC_CACHE_SIZE;
    cfg->lookahead_size = 32;

    /* LFS_NO_MALLOC: littlefs never allocates, so the caches are ours. One
     * set is enough because every filesystem call is serialized by the NVS
     * shim's lock. */
    static uint8_t read_buffer[LFS_NVMC_CACHE_SIZE];
    static uint8_t prog_buffer[LFS_NVMC_CACHE_SIZE];
    static uint32_t lookahead_buffer[32 / sizeof(uint32_t)];
    cfg->read_buffer = read_buffer;
    cfg->prog_buffer = prog_buffer;
    cfg->lookahead_buffer = lookahead_buffer;
}
