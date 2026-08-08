/*
 * Bounded NVMC primitives, because nrfx's are not.
 *
 * Every blocking wrapper this port would otherwise call spins on the NVMC's
 * READY flag with no bound of its own (nrfx v3.9.0,
 * drivers/src/nrfx_nvmc.c): nvmc_word_write() waits for ready BEFORE each
 * word, so nrfx_nvmc_word_write() and nrfx_nvmc_words_write() inherit it;
 * nrfx_nvmc_page_erase() waits after starting a full page erase; and
 * nrfx_nvmc_page_partial_erase_continue() waits after starting each slice, so
 * slicing bounds the NUMBER of waits but not the length of any one of them.
 *
 * A wait that cannot expire is not a slow write, it is a dead node. These run
 * under the global recursive NVS lock that every other lock in the firmware
 * sits above (nrf/shim/nvs_lfs.c) on one path, and from fault handlers on the
 * other, and the port's task watchdog is a no-op stub, so nothing would ever
 * time them out.
 *
 * The bound is a poll count, not a clock, because the callers that need it
 * most have no clock: boot_trace runs before the scheduler starts and from
 * fault handlers with interrupts disabled, where tick counts do not advance
 * and vTaskDelay is not callable. The budgets below are therefore sized in
 * polls and converted to time conservatively, and every one of them clears
 * its datasheet figure by more than an order of magnitude even at the fast
 * end of that conversion.
 *
 * Honest limit on what any of this can do: while the NVMC is busy the flash
 * is unreadable, so a CPU fetching instructions from flash is stalled by the
 * controller rather than executing. If the controller wedges in a way that
 * never releases the fetch, no software bound gets to run and only a hardware
 * watchdog recovers the node. These bounds cover the case where the CPU
 * survives (loop resident in the instruction cache, or an NVMC reporting
 * not-ready while the flash bus is fine), turning a permanent silent freeze
 * into a reported failure.
 *
 * Every function returns false (or NVMC_SLICE_TIMEOUT) if its bound expired,
 * in which case the operation did NOT complete and the target words are of
 * unknown content.
 */
#ifndef BRAMBLE_NVMC_BOUNDED_H
#define BRAMBLE_NVMC_BOUNDED_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Datasheet figures, all from the nRF52840 Product Specification v1.11, NVMC
 * electrical specification, and all already cited by the callers:
 *   tWRITE      41us   write one 32-bit word
 *   tERASEPAGE  85ms   erase one page
 *
 * Poll-to-time conversion used to size the budgets below: the poll body is a
 * volatile read of NVMC->READY plus a compare, branch and increment. Taking a
 * conservative 3 cycles per poll as the floor (an APB peripheral read costs
 * more than that in practice, so real elapsed time is longer) at the 64MHz
 * core clock, one million polls is at least ~47ms.
 */
#define NVMC_PAGE_ERASE_MS 85

/* One word program. 200k polls is at least ~9ms, over 200x tWRITE. */
#define NVMC_WRITE_POLLS 200000u

/* One partial-erase slice, which callers configure at a few ms. 2M polls is
 * at least ~94ms, far above any slice duration this port asks for and above
 * tERASEPAGE itself, so a slice can never be abandoned early. */
#define NVMC_SLICE_POLLS 2000000u

/* One full, unsliced page erase. 40M polls is at least ~1.9s, over 20x
 * tERASEPAGE. Deliberately lavish: aborting a legitimately slow erase would
 * leave a half-erased page, which is far worse than spending a second once on
 * a boot that was already going wrong. */
#define NVMC_PAGE_ERASE_POLLS 40000000u

/*
 * Optional hook, called between polls once the first NVMC_YIELD_AFTER_POLLS
 * have gone by without READY. It exists so a caller that holds the global NVS
 * lock can let the mesh and the BLE host run rather than starve every task
 * below it while it waits out a wedged controller. Pass NULL from any context
 * with no scheduler (boot_trace, fault handlers), where yielding is not just
 * pointless but unsafe.
 */
typedef void (*nvmc_yield_fn)(void);

/* Polls before the first yield, and between yields after that. Sized so the
 * normal case never yields at all: a word program completes in tWRITE, which
 * this covers many times over, and the CPU is usually stalled through the
 * write anyway so READY is already set on the first poll. */
#define NVMC_YIELD_AFTER_POLLS 2000u

/* Polls the READY flag until it asserts. Returns false if max_polls ran out
 * first, which means the controller is still busy or wedged. */
bool nvmc_bounded_ready(uint32_t max_polls, nvmc_yield_fn yield);

/* Programs num_words consecutive words at addr, waiting (bounded) for the
 * controller to go ready before each one, exactly as nrfx does. Returns false
 * as soon as a wait expires, with the remaining words unwritten. Completion
 * of the LAST word is a separate nvmc_bounded_ready() call, again as in nrfx:
 * the write mode is dropped immediately after the store, and is never held
 * across a wait that might yield. */
bool nvmc_bounded_words_write(uint32_t addr, const uint32_t* src, uint32_t num_words,
                              uint32_t max_polls, nvmc_yield_fn yield);

/* Full, unsliced page erase: ~85ms with the CPU stalled throughout. Only for
 * callers that cannot slice, which on this port means boot_trace (no
 * scheduler, and its page has to be writable from a fault handler). */
bool nvmc_bounded_page_erase(uint32_t addr, uint32_t max_polls);

/* Sliced page erase, for callers that must not stall the radios for 85ms.
 * Init once, then call the slice function until it reports done. */
bool nvmc_bounded_partial_erase_init(uint32_t addr, uint32_t duration_ms);

typedef enum {
    NVMC_SLICE_MORE = 0,   /* slice finished, page needs more slices */
    NVMC_SLICE_DONE = 1,   /* page fully erased */
    NVMC_SLICE_TIMEOUT = 2 /* READY never came back within the bound */
} nvmc_slice_result_t;

nvmc_slice_result_t nvmc_bounded_partial_erase_slice(uint32_t max_polls);

#endif /* BRAMBLE_NVMC_BOUNDED_H */
