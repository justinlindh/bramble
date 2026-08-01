/*
 * battery_virt: the emulator's virtual battery. It implements the battery.h
 * contract for the host (IDF linux target and the plain-gcc harness) by
 * serving the last VBAT millivolts (and charging state) delivered over
 * emu-link as a `batt` message. Before the first `batt` arrives it reports
 * a sane full-ish value (4000 mV) so a node that reads the battery at
 * boot never sees 0.
 *
 * `batt` message fields: "mv" (required, number), "charging" (optional
 * bool). charging:true maps to BATTERY_CHG_YES, charging:false to
 * BATTERY_CHG_NO, and an absent field to BATTERY_CHG_UNKNOWN (also the
 * default before the first message, and what any scenario script that
 * never sends "charging" gets). The emulated node always reports
 * present=true: emu-link is its battery hardware.
 *
 * Thread-safety: the batt handler runs on emu_link's reader thread while
 * battery_get_status is called from firmware tasks. Plain scalars are
 * enough, so atomics avoid a mutex entirely.
 */

/* CONFIG_IDF_TARGET_LINUX lives in sdkconfig.h; pull it in on IDF builds so
 * the composite host-gate below sees it (same convention as board_config.h /
 * display.h). The plain-gcc test harness has no sdkconfig.h and takes the host
 * branch anyway. */
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#if defined(ESP_PLATFORM) && !defined(CONFIG_IDF_TARGET_LINUX)
/* On-device build: components/battery/battery.c owns the real ADC driver. */
#else

#include "battery.h"
#include "emu_link.h"

#include <stdatomic.h>

#define BATTERY_VIRT_DEFAULT_MV 4000u

static _Atomic uint32_t s_mv = BATTERY_VIRT_DEFAULT_MV;
static _Atomic int s_charging = BATTERY_CHG_UNKNOWN;

static void batt_handler(const cJSON* msg, void* ctx) {
    (void)ctx;
    const cJSON* mv = cJSON_GetObjectItem(msg, "mv");
    if (cJSON_IsNumber(mv) && mv->valueint >= 0)
        atomic_store(&s_mv, (uint32_t)mv->valueint);

    const cJSON* charging = cJSON_GetObjectItem(msg, "charging");
    if (cJSON_IsBool(charging))
        atomic_store(&s_charging, cJSON_IsTrue(charging) ? BATTERY_CHG_YES : BATTERY_CHG_NO);
    else
        atomic_store(&s_charging, BATTERY_CHG_UNKNOWN);
}

void battery_init(void) { emu_link_on("batt", batt_handler, NULL); }

/* battery_mv_to_pct lives in battery_pct.c, shared with the device driver. */

void battery_get_status(battery_status_t* out) {
    out->mv = atomic_load(&s_mv);
    out->pct = battery_mv_to_pct(out->mv);
    out->charging = (battery_charging_t)atomic_load(&s_charging);
    out->present = true;
}

#endif /* host build */
