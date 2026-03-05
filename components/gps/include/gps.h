#ifndef BRAMBLE_GPS_H
#define BRAMBLE_GPS_H

#include <stdbool.h>
#include "location.h"

/**
 * GPS fix callback - called when a new valid position is available.
 * @param pos: pointer to position structure
 * @param ctx: user context pointer
 */
typedef void (*gps_fix_cb_t)(const bramble_position_t* pos, void* ctx);

/**
 * Initialize GPS UART and start background parsing task.
 * @param cb: callback function to notify on new fix (can be NULL)
 * @param ctx: user context pointer passed to callback
 * @return 0 on success, -1 on failure
 */
int gps_init(gps_fix_cb_t cb, void* ctx);

/**
 * Check if GPS currently has a valid fix.
 * @return true if valid fix available
 */
bool gps_has_fix(void);

/**
 * Get current GPS position.
 * @param out: pointer to position structure to fill
 * @return true if valid position copied, false if no fix
 */
bool gps_get_position(bramble_position_t* out);

/**
 * Shutdown GPS and free resources.
 */
void gps_deinit(void);

#endif /* BRAMBLE_GPS_H */
