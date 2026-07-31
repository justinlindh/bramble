#ifndef BRAMBLE_GPS_DUTY_H
#define BRAMBLE_GPS_DUTY_H
#include <stdbool.h>
#include <stdint.h>

/* Wake this long before the next scheduled location share so the AG3335
 * completes a warm reacquisition (VRTC held). Deliberately generous until
 * the bench measures warm TTFF (Task 11 Step 7 updates it with the measured
 * value; if the bench cannot produce a fix, this default stands, documented
 * as awaiting measurement). */
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
