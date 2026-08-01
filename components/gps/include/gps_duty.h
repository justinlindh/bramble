#ifndef BRAMBLE_GPS_DUTY_H
#define BRAMBLE_GPS_DUTY_H
#include <stdbool.h>
#include <stdint.h>

/* Wake this long before the next scheduled location share so the AG3335
 * completes a warm reacquisition (VRTC held).
 *
 * Measured on the T1000-E bench: warm TTFF is roughly 1 to 2 seconds with
 * VRTC held (a soak poll caught a valid fix reporting 7 satellites after
 * only 192 NMEA bytes had arrived in that power window). Cold TTFF indoors
 * was about 22 minutes; outdoors cold was not measured.
 *
 * 60s is a judgment on top of that measurement, not the measurement itself.
 * Scaling straight off warm TTFF would give 4 or 5 seconds, which only
 * covers the case the bench actually exercised: ephemeris-fresh warm starts
 * under good sky. After a park of several hours the broadcast ephemeris has
 * expired, so the same "warm" start has to re-download it and takes tens of
 * seconds. 60s covers that case, and the cost of being wrong is asymmetric:
 * an over-long margin wastes some receiver-on time, while an under-long one
 * silently misses the scheduled share. Tune via the knob below if a
 * deployment measures its own reacquisition profile. */
#ifndef GPS_DUTY_WARM_MARGIN_S
#define GPS_DUTY_WARM_MARGIN_S 60
#endif
/* Below this share interval, cycling saves nothing (margin dominates). */
#define GPS_DUTY_MIN_INTERVAL_S 120

typedef struct {
    bool user_enabled;     /* persisted preference (gps_pref) */
    bool sharing_active;   /* policy enabled AND has targets  */
    uint16_t interval_s;   /* policy share interval           */
    uint32_t now_ms;       /* mesh clock (wraps; wrap-safe math inside) */
    uint32_t last_send_ms; /* 0 = never sent                  */
} gps_duty_inputs_t;

/* Pure: should GNSS power be on right now? */
bool gps_duty_should_power(const gps_duty_inputs_t* in);

#endif
