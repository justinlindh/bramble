// nRF52840 watchdog lifecycle. nRF-only: ESP-IDF's task watchdog has no
// equivalent two-step init-then-arm API, so none of this is part of the
// esp_task_wdt.h portability surface and no shared code includes it.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Initializes the nrfx WDT driver (reload value, behaviour) without
// starting it. Safe to call once, synchronously, at the very top of
// main() before any task is created: initializing does not start the
// countdown, so it carries none of bramble_wdt_arm's risk, and every
// task's esp_task_wdt_add() needs the driver already initialized
// (nrfx_wdt_channel_alloc requires it).
void bramble_wdt_init(void);

// Starts the watchdog countdown. Call exactly once, after boot reaches
// steady state: see the call site in nrf/src/app_init.c (right after the
// BT_BOOT_DONE trace mark) for why arming any earlier risks the watchdog
// surviving into a DFU session, and for the argument that every task this
// build gives a channel to has, by construction, already registered by
// that point. A no-op if already armed or if bramble_wdt_init() never
// succeeded.
void bramble_wdt_arm(void);

// Feeds every channel, including opted-out ones, once. No-op before the
// watchdog is armed. Defense in depth for reboot_to_dfu(): this build
// treats "the nRF52840 WDT survives NVIC_SystemReset()" as the design
// assumption (see nrf/shim/wdt_nrf.c's "DFU survival" section for why,
// and why the 60s period is sized around a whole DFU session fitting
// inside it), not a bench-confirmed fact, so every path into DFU refreshes
// every channel immediately beforehand to give the bootloader as full a
// window as this watchdog can offer.
void bramble_wdt_feed_all(void);

#ifdef __cplusplus
}
#endif
