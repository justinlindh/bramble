/* See boot_trace.h. Word writes go straight through nrfx_nvmc with a
 * busy-wait: the whole point is to work when nothing else does, including
 * from a fault handler with interrupts disabled. */
#include "boot_trace.h"

#include <nrfx.h>
#include <nrfx_nvmc.h>

#include "boot_trace_scan.h"

static uint32_t s_next = 1; /* next free word slot */
static uint32_t s_last_tag;
static volatile bool s_adv_ok;

/* Internal flash is memory-mapped, so reading the page back is a plain
 * load. Volatile because the same words are written through the NVMC. */
static const volatile uint32_t* const s_page = (const volatile uint32_t*)BOOT_TRACE_PAGE;

/* Poll bound for a word write. nRF52840 Product Specification v1.11, NVMC
 * electrical specification: tWRITE (write one 32-bit word) is 41us. This path
 * has no clock to bound against: it runs before the scheduler starts, and
 * from a fault handler with interrupts disabled, so tick counts do not
 * advance and yielding is not an option. A raw poll count is what is left. At
 * a handful of cycles per poll on a 64MHz core, 200000 lands in the tens of
 * milliseconds, hundreds of times tWRITE. Coarse on purpose: the number only
 * has to be far above a working write and far below "forever". */
#define BOOT_TRACE_WRITE_POLLS 200000u

/* Returns when the NVMC reports the write done, or when the bound expires.
 *
 * On expiry the record is ABANDONED and this returns anyway. That is the only
 * safe behaviour: there is no way to report a flash failure by writing to
 * flash, and one lost diagnostic word costs far less than hanging the node.
 * It matters most on the path this whole file exists for, boot_trace_fail(),
 * where spinning here would hang before the reset that was the entire point
 * of taking the stamp. */
static void wait_write_done(void) {
    for (uint32_t i = 0; i < BOOT_TRACE_WRITE_POLLS; i++) {
        if (nrfx_nvmc_write_done_check()) {
            return;
        }
    }
}

static void page_reset(uint32_t carry_failed_boots) {
    nrfx_nvmc_page_erase(BOOT_TRACE_PAGE);
    nrfx_nvmc_word_write(BOOT_TRACE_PAGE, BOOT_TRACE_MAGIC);
    wait_write_done();
    s_next = 1;
    if (carry_failed_boots > 0) {
        boot_trace_mark(BT_BOOT_CARRY, carry_failed_boots);
    }
}

void boot_trace_init(void) {
    /* Read and clear before anything else: this is the only evidence of why
     * the previous boot ended, and on a board with no console it is the
     * difference between "it hung" and "it keeps resetting". */
    uint32_t resetreas = NRF_POWER->RESETREAS;
    NRF_POWER->RESETREAS = resetreas;

    boot_trace_scan_t scan;
    boot_trace_scan(s_page, &scan);

    if (!scan.valid) {
        /* Virgin, corrupt, or not a trace page at all. */
        page_reset(0);
    } else {
        s_next = scan.next;
        if (boot_trace_page_exhausted(s_next)) {
            page_reset(scan.failed_boots);
        }
    }

    if (scan.failed_boots >= BT_BOOT_LOOP_LIMIT) {
        /* Nothing that runs after this point has managed to reach the end of
         * app_init for BT_BOOT_LOOP_LIMIT boots running, and a board that
         * resets faster than the sentinel's 120s never gets rescued by it.
         * Hand the board back to the bootloader while the trace, which now
         * holds every one of those boots and their reset reasons, is
         * readable. Never returns. */
        boot_trace_fail(BT_FAIL_BOOTLOOP, scan.failed_boots);
    }

    boot_trace_mark(BT_BOOT_BEGIN, resetreas);
}

void boot_trace_mark(uint32_t tag, uint32_t aux) {
    if (s_next + 1u >= BOOT_TRACE_PAGE_WORDS) {
        return;
    }
    nrfx_nvmc_word_write(BOOT_TRACE_PAGE + 4u * s_next, BOOT_TRACE_TAG_MARKER | tag);
    nrfx_nvmc_word_write(BOOT_TRACE_PAGE + 4u * (s_next + 1u), aux);
    wait_write_done();
    /* Advanced even when the wait expired: the two words may be partially
     * written, and a slot whose contents are unknown is never reused. */
    s_next += 2;
    s_last_tag = tag;
}

/* Adafruit nRF52 bootloader: DFU_MAGIC_UF2_RESET. The bootloader sees this
 * in GPREGRET after reset and stays resident with the UF2 volume, which is
 * exactly where the host can read this page back. On a board without that
 * bootloader (SWD layout) the value is ignored and the app simply reboots. */
static void reboot_to_dfu(void) __attribute__((noreturn));

static void reboot_to_dfu(void) {
    NRF_POWER->GPREGRET = 0x57;
    __DSB();
    NVIC_SystemReset();
    for (;;) {
    }
}

void boot_trace_fail(uint32_t tag, uint32_t aux) {
    boot_trace_mark(tag, aux);
    reboot_to_dfu();
}

void bramble_nrfx_assert_failed(uint32_t line) {
    /* Interrupts off before touching flash, matching bramble_assert_failed:
     * this can fire from IRQ context, and the NVMC busy-wait in
     * boot_trace_mark() should not be racing anything. Nothing is logged on
     * the way out either. An nrfx assert can fire inside an ISR, the T1000-E
     * has no console at all, and a blocking console write would risk hanging
     * before the stamp lands, which is precisely the failure this handler
     * exists to eliminate. The flash stamp is the channel that survives. */
    __disable_irq();

    /* Reentrancy guard, and it is load-bearing rather than defensive:
     * boot_trace_mark() writes through nrfx_nvmc_word_write(), which carries
     * NRFX_ASSERTs of its own (address validity and word alignment). Without
     * this, an assert raised inside the stamping path would call straight
     * back into here and recurse until the stack gave out, turning a silent
     * lockup into a silent lockup with extra steps. A nested failure skips
     * the flash write and resets anyway, so whatever the trace already holds
     * (the clean boot stages) is still readable from DFU. */
    static volatile bool s_stamping;
    if (!s_stamping) {
        s_stamping = true;
        boot_trace_mark(BT_FAIL_NRFX, line);
    }
    reboot_to_dfu();
}

uint32_t boot_trace_last(void) { return s_last_tag; }

bool boot_trace_adv_ok(void) { return s_adv_ok; }

/* Strong override of the weak no-op probe in ble_server.c: the shared BLE
 * server reports its advertising result here without knowing about flash
 * pages or this platform. */
void bramble_boot_probe(unsigned stage, int rc) {
    if (stage == BT_ADV && rc == 0) {
        s_adv_ok = true;
    }
    boot_trace_mark((uint32_t)stage, (uint32_t)rc);
}
