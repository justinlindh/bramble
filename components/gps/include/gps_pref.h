#ifndef BRAMBLE_GPS_PREF_H
#define BRAMBLE_GPS_PREF_H
#include <stdbool.h>
/* Persisted GPS power preference (NVS bramble/gps_en, default ON). */
bool gps_pref_get(void);
void gps_pref_set(bool enabled);
#endif
