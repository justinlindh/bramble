/* See boot_trace_scan.h. */
#include "boot_trace_scan.h"

#include "boot_trace.h"

/* Free slots a boot needs before the page is reused as-is. A full healthy
 * boot stamps under twenty pairs; a diagnostic build bracketing a subsystem
 * stamps more. Erasing at this bound rather than at exhaustion is what keeps
 * the record of the boot that matters whole instead of cutting it off
 * mid-run, which is the exact failure this trace exists to avoid. */
#define BOOT_TRACE_RESERVE_WORDS 128u

void boot_trace_scan(const volatile uint32_t* page, boot_trace_scan_t* out) {
    out->next = 1;
    out->failed_boots = 0;
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
        } else if (tag == BT_BOOT_BEGIN) {
            out->failed_boots++;
        } else if (tag == BT_BOOT_DONE || tag == BT_FAIL_BOOTLOOP) {
            /* A boot that reached the end of app_init clears the count, and
             * so does a rescue: without the second case the device would
             * bounce straight back into DFU on the very next boot and a
             * freshly flashed, working image would never get to run. */
            out->failed_boots = 0;
        }
        i += 2;
    }
    out->next = i;
}

int boot_trace_page_exhausted(uint32_t next) {
    return next + BOOT_TRACE_RESERVE_WORDS >= BOOT_TRACE_PAGE_WORDS;
}
