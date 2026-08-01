/*
 * battery_wrappers: the legacy battery_read_mv/battery_read_pct API,
 * reimplemented as thin wrappers over battery_get_status() (wave 2) so
 * every target implements the read path exactly once (in
 * battery_get_status) and every existing call site keeps working
 * unchanged.
 */
#include "battery.h"

uint32_t battery_read_mv(void) {
    battery_status_t st;
    battery_get_status(&st);
    return st.mv;
}

uint8_t battery_read_pct(void) {
    battery_status_t st;
    battery_get_status(&st);
    return st.pct;
}
