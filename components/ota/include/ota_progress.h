/* components/ota/include/ota_progress.h */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OTA_PROG_IDLE = 0,
    OTA_PROG_DOWNLOADING,
    OTA_PROG_VERIFYING,
    OTA_PROG_REBOOTING,
    OTA_PROG_FAILED,
} ota_prog_state_t;

typedef struct {
    ota_prog_state_t state;
    int bytes;
    int total;
} ota_progress_snapshot_t;

typedef void (*ota_progress_cb_t)(const ota_progress_snapshot_t* snap);

/* Record progress. Invokes the callback on state change, or when the
 * download percent advanced >= 5 points since the last callback. Single
 * writer (the OTA task); readers get a coherent-enough snapshot for UI. */
void ota_progress_report(ota_prog_state_t state, int bytes, int total);
void ota_progress_get(ota_progress_snapshot_t* out);
void ota_progress_set_callback(ota_progress_cb_t cb);
const char* ota_progress_state_str(ota_prog_state_t state);

/* Percent complete (0 when total<=0), from an arbitrary snapshot. */
int ota_progress_percent(const ota_progress_snapshot_t* snap);

/* Re-report the CURRENT bytes/total with a new state (get+report in one). */
void ota_progress_set_state(ota_prog_state_t state);

#ifdef __cplusplus
}
#endif
