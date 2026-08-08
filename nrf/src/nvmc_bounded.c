/* See nvmc_bounded.h. These mirror nrfx v3.9.0's own sequences
 * (drivers/src/nrfx_nvmc.c) step for step, with every "while (!ready) {}"
 * replaced by a counted poll, and the mode restored to read-only on every way
 * out including the timeout paths: leaving write or erase enable latched
 * after a failure would let a later stray store reach flash.
 *
 * nRF52840 has no TrustZone, so the NRF_NVMC_HAS_NON_SECURE_OPERATIONS half
 * of nrfx's mode helpers compiles out there and is not reproduced here. */
#include "nvmc_bounded.h"

#include <hal/nrf_nvmc.h>
#include <nrfx.h>

/* Two words of state, and no more: the t1000e static-RAM gate is measured in
 * bytes. "No erase in flight" is a sentinel address rather than a separate
 * flag (0xFFFFFFFF is not page-aligned, so it can never be a real target),
 * and the slice duration is read back from the peripheral register that
 * already holds it instead of being cached. Both tricks are nrfx's own. */
#define NVMC_NO_PARTIAL_ERASE 0xFFFFFFFFu

static uint32_t s_partial_page_addr = NVMC_NO_PARTIAL_ERASE;
static uint32_t s_partial_elapsed_ms;

bool nvmc_bounded_ready(uint32_t max_polls, nvmc_yield_fn yield) {
    uint32_t since_yield = 0;
    for (uint32_t i = 0; i < max_polls; i++) {
        if (nrf_nvmc_ready_check(NRF_NVMC)) {
            return true;
        }
        if (yield != NULL && ++since_yield >= NVMC_YIELD_AFTER_POLLS) {
            since_yield = 0;
            yield();
        }
    }
    return false;
}

bool nvmc_bounded_words_write(uint32_t addr, const uint32_t* src, uint32_t num_words,
                              uint32_t max_polls, nvmc_yield_fn yield) {
    for (uint32_t i = 0; i < num_words; i++) {
        /* Write enable is latched around the store ONLY, never across the
         * ready wait. nrfx brackets its single-word public entry point the
         * same way, and here it is load-bearing rather than stylistic: the
         * wait is where a yield hook can hand the CPU to another task, and
         * leaving flash write-enabled while somebody else runs would let a
         * stray store reach flash instead of being ignored. Dropping the mode
         * with the write still in flight is exactly what nrfx does too. */
        if (!nvmc_bounded_ready(max_polls, yield)) {
            return false;
        }
        nrf_nvmc_mode_set(NRF_NVMC, NRF_NVMC_MODE_WRITE);
        nrf_nvmc_word_write(addr + (i * sizeof(uint32_t)), src[i]);
        __DMB();
        nrf_nvmc_mode_set(NRF_NVMC, NRF_NVMC_MODE_READONLY);
    }
    return true;
}

bool nvmc_bounded_page_erase(uint32_t addr, uint32_t max_polls) {
    nrf_nvmc_mode_set(NRF_NVMC, NRF_NVMC_MODE_ERASE);
    nrf_nvmc_page_erase_start(NRF_NVMC, addr);
    bool ok = nvmc_bounded_ready(max_polls, NULL);
    nrf_nvmc_mode_set(NRF_NVMC, NRF_NVMC_MODE_READONLY);
    return ok;
}

bool nvmc_bounded_partial_erase_init(uint32_t addr, uint32_t duration_ms) {
    if (duration_ms == 0 || addr == NVMC_NO_PARTIAL_ERASE) {
        return false; /* zero slices would never accumulate to a finished page */
    }
    nrf_nvmc_partial_erase_duration_set(NRF_NVMC, duration_ms);
    s_partial_page_addr = addr;
    s_partial_elapsed_ms = 0;
    return true;
}

nvmc_slice_result_t nvmc_bounded_partial_erase_slice(uint32_t max_polls) {
    if (s_partial_page_addr == NVMC_NO_PARTIAL_ERASE) {
        /* No erase in flight: report a failure rather than "done", so a
         * caller that lost track can never conclude a page is erased. */
        return NVMC_SLICE_TIMEOUT;
    }
#if defined(NVMC_CONFIG_WEN_PEen)
    nrf_nvmc_mode_set(NRF_NVMC, NRF_NVMC_MODE_PARTIAL_ERASE);
#else
    nrf_nvmc_mode_set(NRF_NVMC, NRF_NVMC_MODE_ERASE);
#endif
    nrf_nvmc_page_partial_erase_start(NRF_NVMC, s_partial_page_addr);
    bool ok = nvmc_bounded_ready(max_polls, NULL);
    nrf_nvmc_mode_set(NRF_NVMC, NRF_NVMC_MODE_READONLY);
    if (!ok) {
        s_partial_page_addr = NVMC_NO_PARTIAL_ERASE;
        return NVMC_SLICE_TIMEOUT;
    }
    /* Same accounting rule as nrfx: the page is erased once the accumulated
     * slice time reaches tERASEPAGE. */
    s_partial_elapsed_ms += nrf_nvmc_partial_erase_duration_get(NRF_NVMC);
    if (s_partial_elapsed_ms < NVMC_PAGE_ERASE_MS) {
        return NVMC_SLICE_MORE;
    }
    s_partial_page_addr = NVMC_NO_PARTIAL_ERASE;
    return NVMC_SLICE_DONE;
}
