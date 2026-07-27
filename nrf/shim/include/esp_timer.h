// esp_timer shim for the nRF52840 target. Only the surface the linked
// components call: esp_timer_get_time (msg_store, traffic_debug). The
// timer-object API (create/start/stop) has no caller until mesh_task lands
// in P2 and gets added with it.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int64_t esp_timer_get_time(void);

#ifdef __cplusplus
}
#endif
