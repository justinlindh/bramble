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
 * Every wait in here is bounded. The NVMC's READY flag is the only completion
 * signal it offers, and a wait on it that cannot expire is not a slow write,
 * it is a dead node: this block device runs under the NVS shim's global
 * recursive lock, the one lock every other lock in the firmware sits above
 * (nrf/shim/nvs_lfs.c), so an unbounded spin blocks every NVS consumer at
 * once, and an unbounded spin that also never yields starves every task below
 * the one holding it.
 *
 * Honest limit on what a software bound can do: while the NVMC is busy the
 * flash is unreadable, so a CPU fetching instructions from flash is stalled
 * by the controller rather than executing. If the controller wedges in a way
 * that never releases the fetch, the bound below never gets to run and only a
 * hardware watchdog can recover the node. The bound covers the case the CPU
 * survives (loop resident in the instruction cache, or an NVMC that reports
 * not-ready while the flash bus is fine), and turns it from a permanent
 * silent freeze into an I/O error the caller can act on.
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

/* Bound on the READY poll after a word program. nRF52840 Product
 * Specification v1.11, NVMC electrical specification: tWRITE (write one
 * 32-bit word) is 41us and tERASEPAGE is 85ms, the same two figures the
 * comments above are sized against. 10ms is ~240x tWRITE, which no
 * slow-but-working write can plausibly need, and it is short enough that a
 * wedged controller costs the radios milliseconds instead of the node. */
#define LFS_PROG_TIMEOUT_MS 10

/* Polls to spin tightly before falling back to the yielding loop. A word
 * write stalls the CPU for its own duration when the CPU fetches from flash,
 * so READY is normally already set on the first poll; this covers the case
 * where it is not without paying a context switch for a 41us wait. Same shape
 * and the same reason as wait_busy_low() in lr11xx_hal_nrf.c. */
#define LFS_PROG_SPIN_POLLS 2000

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

    for (int i = 0; i < LFS_PROG_SPIN_POLLS; i++) {
        if (nrfx_nvmc_write_done_check()) {
            return LFS_ERR_OK;
        }
    }
    /* Past every plausible completion window, so let the radios and the mesh
     * run between polls and give up rather than wait forever.
     *
     * LFS_ERR_IO and not LFS_ERR_CORRUPT, deliberately: CORRUPT tells
     * littlefs the block is bad and asks it to relocate, which is another
     * page erase plus a whole block of programs. If the controller is not
     * completing writes at all, that turns one expired wait into hundreds,
     * every one of them still holding the global NVS lock. IO fails the
     * operation immediately and hands the decision to the caller. */
    TickType_t start = xTaskGetTickCount();
    while (!nrfx_nvmc_write_done_check()) {
        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(LFS_PROG_TIMEOUT_MS)) {
            ESP_LOGE(TAG, "word program did not complete at 0x%08lx", (unsigned long)addr);
            return LFS_ERR_IO;
        }
        taskYIELD();
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
