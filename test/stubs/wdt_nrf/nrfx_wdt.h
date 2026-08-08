/* Host stub for the nrfx_wdt driver, scoped to test_wdt_nrf(_*): nrf/shim/wdt_nrf.c
 * is a hardware peripheral driver and cannot run on the host, so this
 * declares the exact surface it calls (nrfx_wdt_init, nrfx_wdt_channel_alloc,
 * nrfx_wdt_enable, nrfx_wdt_channel_feed, plus the types/macros it names) and
 * the test binary supplies observable, recording bodies. Not a general nrfx
 * shim: kept private to this test target's include path so it can never
 * shadow the real driver in any other build. */
#pragma once

#include <stdint.h>

typedef int nrfx_err_t;
#define NRFX_SUCCESS 0
#define NRFX_ERROR_NO_MEM 1

typedef struct {
    int dummy; /* unused; a real instance carries a register base pointer */
} nrfx_wdt_t;
#define NRFX_WDT_INSTANCE(id) \
    { .dummy = (id) }

typedef int nrfx_wdt_channel_id;

/* wdt_nrf.c only ever reads .behaviour/.reload_value; the fake nrfx_wdt_init
 * below ignores both, matching how nrfx_config.h's NRFX_WDT_CONFIG_NO_IRQ
 * drops the real struct's interrupt_priority field. */
typedef struct {
    uint32_t behaviour;
    uint32_t reload_value;
} nrfx_wdt_config_t;

#define NRF_WDT_BEHAVIOUR_RUN_SLEEP_MASK 1u

nrfx_err_t nrfx_wdt_init(const nrfx_wdt_t* p_instance, const nrfx_wdt_config_t* p_config,
                         void* wdt_event_handler, void* p_context);
nrfx_err_t nrfx_wdt_channel_alloc(const nrfx_wdt_t* p_instance, nrfx_wdt_channel_id* p_channel_id);
void nrfx_wdt_enable(const nrfx_wdt_t* p_instance);
void nrfx_wdt_channel_feed(const nrfx_wdt_t* p_instance, nrfx_wdt_channel_id channel_id);
