/*
 * Pure page-walking logic for the flash boot trace (see boot_trace.h).
 *
 * Split out of boot_trace.c so it can be exercised on the host: everything
 * here is arithmetic over a page image, with no NVMC access, no reboot and
 * no side effects. test/test_boot_trace.c covers it. The hardware side
 * (erase, word write, reset-into-DFU) stays in boot_trace.c.
 */
#pragma once

#include <stdint.h>

/* Words in one nRF52840 flash page (4096 bytes). Slot 0 holds the magic;
 * the rest are (tag, aux) pairs, so tag words sit at odd slots. */
#define BOOT_TRACE_PAGE_WORDS 1024u

/* Every tag word carries this marker in its top nibble, which is what makes
 * an erased slot (0xFFFFFFFF) distinguishable from a written one. An aux
 * word has no marker and may legitimately be any value at all, including
 * 0xFFFFFFFF: boot_trace_mark(BT_NVS_INIT, (uint32_t)-1) writes exactly
 * that. Scanning tag words only, never aux words, is what keeps a negative
 * return code from truncating the trace. */
#define BOOT_TRACE_TAG_MARKER 0xB0000000u
#define BOOT_TRACE_TAG_MASK 0xF0000000u

typedef struct {
    /* First free word slot, i.e. where the next pair goes. Equals
     * BOOT_TRACE_PAGE_WORDS-ish when the page is full. */
    uint32_t next;
    /* Boots recorded since the last one that finished, or since the last
     * boot-loop rescue. Zero on a page whose most recent boot completed. */
    uint32_t failed_boots;
    /* Consecutive boots, most recent first, whose BT_BOOT_BEGIN aux
     * (RESETREAS) had the DOG bit set, i.e. were reset by the watchdog. Set
     * per BT_BOOT_BEGIN from that boot's own reset reason, not from whether
     * the boot itself later reached BT_BOOT_DONE (every DOG-reset boot
     * does, or the watchdog would not have helped, so tracking that instead
     * would never accumulate past one); cleared by any BT_BOOT_BEGIN whose
     * aux does NOT have the DOG bit set, and by a rescue
     * (BT_FAIL_BOOTLOOP or BT_FAIL_DOGLOOP). See BT_DOG_LOOP_LIMIT. */
    uint32_t dog_boots;
    /* False when slot 0 does not hold the magic, i.e. the page is virgin,
     * corrupt, or holds something that is not a trace. The caller must
     * erase before writing anything. */
    int valid;
} boot_trace_scan_t;

/* Walks a page image. `page` must point at BOOT_TRACE_PAGE_WORDS words. */
void boot_trace_scan(const volatile uint32_t* page, boot_trace_scan_t* out);

/* True when fewer than a whole boot's worth of slots remain free, so the
 * caller should erase and start a fresh page rather than record a boot it
 * would have to truncate halfway through. */
int boot_trace_page_exhausted(uint32_t next);
