/* components/ota/ota_progress.c */
#include "ota_progress.h"

#include <stddef.h>

static ota_progress_snapshot_t s_snap;
static ota_progress_cb_t s_cb;
static int s_last_fired_percent = -1;

int ota_progress_percent(const ota_progress_snapshot_t* snap) {
    if (snap->total <= 0) {
        return 0;
    }
    return (int)((long long)snap->bytes * 100 / snap->total);
}

void ota_progress_report(ota_prog_state_t state, int bytes, int total) {
    int state_changed = (state != s_snap.state);
    s_snap.state = state;
    s_snap.bytes = bytes;
    s_snap.total = total;

    if (!s_cb) {
        return;
    }
    int pct = ota_progress_percent(&s_snap);
    if (state_changed || (state == OTA_PROG_DOWNLOADING && pct - s_last_fired_percent >= 5)) {
        s_last_fired_percent = pct;
        s_cb(&s_snap);
    }
}

void ota_progress_set_state(ota_prog_state_t state) {
    ota_progress_report(state, s_snap.bytes, s_snap.total);
}

void ota_progress_get(ota_progress_snapshot_t* out) {
    if (out) {
        *out = s_snap;
    }
}

void ota_progress_set_callback(ota_progress_cb_t cb) {
    s_cb = cb;
    s_last_fired_percent = -1;
}

const char* ota_progress_state_str(ota_prog_state_t state) {
    switch (state) {
    case OTA_PROG_DOWNLOADING:
        return "downloading";
    case OTA_PROG_VERIFYING:
        return "verifying";
    case OTA_PROG_REBOOTING:
        return "rebooting";
    case OTA_PROG_FAILED:
        return "failed";
    case OTA_PROG_IDLE:
    default:
        return "idle";
    }
}
