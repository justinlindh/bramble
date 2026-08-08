/* See boot_trace_scan.h. */
#include "boot_trace_scan.h"

#include "boot_trace.h"

/* Free slots a boot needs before the page is reused as-is. A full healthy
 * boot stamps under twenty pairs; a diagnostic build bracketing a subsystem
 * stamps more. Erasing at this bound rather than at exhaustion is what keeps
 * the record of the boot that matters whole instead of cutting it off
 * mid-run, which is the exact failure this trace exists to avoid. */
#define BOOT_TRACE_RESERVE_WORDS 128u

/* nRF52840 POWER->RESETREAS bit 1 (nrf52840_bitfields.h:
 * POWER_RESETREAS_DOG_Msk). Restated here, not included, so this file stays
 * free of nrfx headers and host-testable (see the file comment in
 * boot_trace_scan.h); nrf/scripts/read_boot_trace.py's RESETREAS_BITS dict
 * hardcodes the same bit for the same reason and must stay in sync. */
#define BOOT_TRACE_RESETREAS_DOG_MSK 0x2u

void boot_trace_scan(const volatile uint32_t* page, boot_trace_scan_t* out) {
    out->next = 1;
    out->failed_boots = 0;
    out->dog_boots = 0;
    out->valid = 0;

    if (page[0] != BOOT_TRACE_MAGIC)
        return;
    out->valid = 1;

    uint32_t i = 1;
    while (i + 1u < BOOT_TRACE_PAGE_WORDS &&
           (page[i] & BOOT_TRACE_TAG_MASK) == BOOT_TRACE_TAG_MARKER) {
        uint32_t tag = page[i] & 0xFFu;
        uint32_t aux = page[i + 1u];
        if (tag == BT_BOOT_CARRY) {
            /* Written immediately after an erase so the count survives a
             * page the trace outgrew. It replaces the running total rather
             * than adding to it: everything it was counting is gone. */
            out->failed_boots = aux;
        } else if (tag == BT_BOOT_CARRY_DOG) {
            /* Same idea as BT_BOOT_CARRY, for the DOG-reset streak. */
            out->dog_boots = aux;
        } else if (tag == BT_BOOT_BEGIN) {
            out->failed_boots++;
            /* Whether THIS boot's own DOG-reset streak continues or breaks
             * is decided entirely by why THIS boot started (aux, its
             * RESETREAS), independent of the failed_boots bookkeeping
             * above and of whether this boot later reaches BT_BOOT_DONE:
             * see boot_trace_scan_t.dog_boots for why tying it to
             * BT_BOOT_DONE instead would defeat the whole counter. A DOG
             * reset can only happen after this build's watchdog is armed,
             * which is strictly after BT_BOOT_DONE (nrf/src/app_init.c), so
             * in practice a boot that starts here with the DOG bit set
             * always means the PREVIOUS boot reached BT_BOOT_DONE and then
             * hung; nothing here depends on that invariant holding, since
             * this branch reads only the current boot's own RESETREAS. */
            if (aux & BOOT_TRACE_RESETREAS_DOG_MSK) {
                out->dog_boots++;
            } else {
                out->dog_boots = 0;
            }
        } else if (tag == BT_BOOT_DONE) {
            /* A boot that reached the end of app_init clears failed_boots:
             * without this the device would bounce straight back into DFU
             * on the very next boot and a freshly flashed, working image
             * would never get to run. dog_boots is deliberately NOT touched
             * here; see boot_trace_scan_t.dog_boots. */
            out->failed_boots = 0;
        } else if (tag == BT_FAIL_BOOTLOOP || tag == BT_FAIL_DOGLOOP) {
            /* A rescue clears both counts, giving the freshly-recovered
             * board a clean slate for whichever kind of loop comes next. */
            out->failed_boots = 0;
            out->dog_boots = 0;
        }
        i += 2;
    }
    out->next = i;
}

int boot_trace_page_exhausted(uint32_t next) {
    return next + BOOT_TRACE_RESERVE_WORDS >= BOOT_TRACE_PAGE_WORDS;
}
