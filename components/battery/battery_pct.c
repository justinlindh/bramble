#include "battery.h"

/*
 * LiPo discharge-curve approximation, shared by the on-device ADC driver
 * (battery.c) and the emulator's virtual battery (battery_virt.c) so the two
 * builds can never drift apart. Piecewise-linear between the breakpoints
 * below; clamped to [0, 100] outside the 3300-4200 mV span.
 *
 *   4200 mV = 100%, 4060 mV = 90%, 3900 mV = 70%,
 *   3800 mV =  50%, 3700 mV = 30%, 3600 mV = 15%,
 *   3300 mV =   0% (cutoff)
 */
uint8_t battery_mv_to_pct(uint32_t mv) {
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
