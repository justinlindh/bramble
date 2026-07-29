/* See boot_trace.h. Word writes go straight through nrfx_nvmc with a
 * busy-wait: the whole point is to work when nothing else does, including
 * from a fault handler with interrupts disabled. */
#include "boot_trace.h"

#include <nrfx.h>
#include <nrfx_nvmc.h>

/* Word slots within the page: [0]=magic, then (tag, aux) pairs. */
#define BT_MAX_WORDS 1022u

static uint32_t s_next = 1; /* next free word slot */
static uint32_t s_last_tag;
static volatile bool s_adv_ok;

void boot_trace_init(void) {
    nrfx_nvmc_page_erase(BOOT_TRACE_PAGE);
    nrfx_nvmc_word_write(BOOT_TRACE_PAGE, BOOT_TRACE_MAGIC);
    while (!nrfx_nvmc_write_done_check()) {
    }
    s_next = 1;
}

void boot_trace_mark(uint32_t tag, uint32_t aux) {
    if (s_next + 1 >= BT_MAX_WORDS) {
        return;
    }
    nrfx_nvmc_word_write(BOOT_TRACE_PAGE + 4u * s_next, 0xB0000000u | tag);
    nrfx_nvmc_word_write(BOOT_TRACE_PAGE + 4u * (s_next + 1u), aux);
    while (!nrfx_nvmc_write_done_check()) {
    }
    s_next += 2;
    s_last_tag = tag;
}

void boot_trace_fail(uint32_t tag, uint32_t aux) {
    boot_trace_mark(tag, aux);
    /* Adafruit nRF52 bootloader: DFU_MAGIC_UF2_RESET. The bootloader sees
     * this in GPREGRET after reset and stays resident with the UF2 volume,
     * which is exactly where the host can read this page back. On a board
     * without that bootloader (SWD layout) the value is ignored and the app
     * simply reboots. */
    NRF_POWER->GPREGRET = 0x57;
    __DSB();
    NVIC_SystemReset();
    for (;;) {
    }
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
