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
 *
 * Every wait in here is bounded, which is why none of the nrfx_nvmc_* blocking
 * wrappers are used: each of them spins on the READY flag with no bound of its
 * own, including inside the sliced partial-erase call. nvmc_bounded.h explains
 * that in full and provides the replacements. This block device runs under the
 * NVS shim's global recursive lock, the one lock every other lock in the
 * firmware sits above (nrf/shim/nvs_lfs.c), so an unbounded spin blocks every
 * NVS consumer at once, and an unbounded spin that also never yields starves
 * every task below the one holding it.
 */
#include "lfs_nvmc.h"

#include <string.h>

#include <FreeRTOS.h>
#include <task.h>

#include "esp_log.h"
#include "lfs.h"
#include "nvmc_bounded.h"

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

/* Yield hook for the bounded waits. It only fires once a wait has already run
 * long past the datasheet time for the operation, so the normal case never
 * reaches it; when it does fire, this task is holding the global NVS lock and
 * everything below it would otherwise be starved outright. */
static void prog_yield(void) { taskYIELD(); }

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
    if (!nvmc_bounded_words_write(addr, (const uint32_t*)buffer, size / sizeof(uint32_t),
                                  NVMC_WRITE_POLLS, prog_yield)) {
        ESP_LOGE(TAG, "word program did not start at 0x%08lx", (unsigned long)addr);
        return LFS_ERR_IO;
    }
    /* The last word is still in flight: nvmc_bounded_words_write drops write
     * mode immediately after the store, exactly as nrfx does, so completion is
     * this wait.
     *
     * LFS_ERR_IO and not LFS_ERR_CORRUPT, deliberately: CORRUPT tells littlefs
     * the block is bad and asks it to relocate, which is another page erase
     * plus a whole block of programs. If the controller is not completing
     * writes at all, that turns one expired wait into hundreds, every one of
     * them still holding the global NVS lock. IO fails the operation
     * immediately and hands the decision to the caller. */
    if (!nvmc_bounded_ready(NVMC_WRITE_POLLS, prog_yield)) {
        ESP_LOGE(TAG, "word program did not complete at 0x%08lx", (unsigned long)addr);
        return LFS_ERR_IO;
    }
    return LFS_ERR_OK;
}

int lfs_nvmc_erase(const struct lfs_config* c, lfs_block_t block) {
    uint32_t addr = s_base + block * c->block_size;
    if (!nvmc_bounded_partial_erase_init(addr, LFS_ERASE_SLICE_MS)) {
        ESP_LOGE(TAG, "partial erase init failed at 0x%08lx", (unsigned long)addr);
        return LFS_ERR_IO;
    }
    for (int i = 0; i < LFS_ERASE_MAX_SLICES; i++) {
        nvmc_slice_result_t r = nvmc_bounded_partial_erase_slice(NVMC_SLICE_POLLS);
        if (r == NVMC_SLICE_DONE) {
            return LFS_ERR_OK; /* page fully erased */
        }
        if (r == NVMC_SLICE_TIMEOUT) {
            ESP_LOGE(TAG, "erase slice %d timed out at 0x%08lx", i, (unsigned long)addr);
            return LFS_ERR_IO;
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
     * shim's lock: the nvs_* API takes it internally, and direct consumers
     * of nvs_lfs_handle() (the message store) take it via nvs_lfs_lock().
     * Any new direct consumer inherits that obligation. */
    static uint8_t read_buffer[LFS_NVMC_CACHE_SIZE];
    static uint8_t prog_buffer[LFS_NVMC_CACHE_SIZE];
    static uint32_t lookahead_buffer[32 / sizeof(uint32_t)];
    cfg->read_buffer = read_buffer;
    cfg->prog_buffer = prog_buffer;
    cfg->lookahead_buffer = lookahead_buffer;
}
