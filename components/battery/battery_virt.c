/*
 * battery_virt: the emulator's virtual battery. It implements the battery.h
 * contract for the host (IDF linux target and the plain-gcc harness) by
 * serving the last VBAT millivolts delivered over emu-link as a `batt`
 * message. Before the first `batt` arrives it reports a sane full-ish value
 * (4000 mV) so a node that reads the battery at boot never sees 0.
 *
 * Thread-safety: the batt handler runs on emu_link's reader thread while
 * battery_read_mv is called from firmware tasks. A single scalar is enough,
 * so an atomic avoids a mutex entirely.
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

static void batt_handler(const cJSON *msg, void *ctx) {
    (void)ctx;
    const cJSON *mv = cJSON_GetObjectItem(msg, "mv");
    if (cJSON_IsNumber(mv) && mv->valueint >= 0)
        atomic_store(&s_mv, (uint32_t)mv->valueint);
}

void battery_init(void) { emu_link_on("batt", batt_handler, NULL); }

uint32_t battery_read_mv(void) { return atomic_load(&s_mv); }

uint8_t battery_mv_to_pct(uint32_t mv) {
    /* Same LiPo discharge curve as the device driver (battery.c). */
    if (mv >= 4200)
        return 100;
    if (mv <= 3300)
        return 0;

    static const struct {
        uint32_t mv;
        uint8_t pct;
    } curve[] = {
        {4200, 100}, {4060, 90}, {3900, 70}, {3800, 50}, {3700, 30}, {3600, 15}, {3300, 0},
    };
    for (int i = 0; i < 6; i++) {
        if (mv >= curve[i + 1].mv) {
            uint32_t range_mv = curve[i].mv - curve[i + 1].mv;
            uint8_t range_pct = curve[i].pct - curve[i + 1].pct;
            return curve[i + 1].pct + (uint8_t)((mv - curve[i + 1].mv) * range_pct / range_mv);
        }
    }
    return 0;
}

uint8_t battery_read_pct(void) { return battery_mv_to_pct(battery_read_mv()); }

#endif /* host build */
