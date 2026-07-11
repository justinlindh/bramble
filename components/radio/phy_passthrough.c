#include "phy_passthrough.h"
#include "esp_timer.h"

#include <stddef.h>

/*
 * PHY passthrough gate state. All module-global, never persisted (DESIGN.md
 * section 10: passthrough must never survive a reboot, so there is deliberately
 * no NVS read or write anywhere in this file).
 *
 * Concurrency: enable/disable run on the RPC task; is_active / forward_rx run
 * on the radio RX task. No mutex (host tests have no RTOS, and the RX path must
 * stay cheap). Safety comes from ordering, not locking:
 *   - enable writes s_expiry_us and s_forced, THEN sets s_enabled = true, so a
 *     reader that observes s_enabled == true also observes a valid expiry;
 *   - disable clears s_enabled first.
 * s_enabled is checked first in every read, so a stale/torn s_expiry_us is
 * never consulted while disabled. The worst race (a config change racing a
 * single RX) forwards or drops exactly one frame either way, which is benign.
 */
static volatile bool s_enabled = false;
static volatile bool s_forced = false;
static volatile int64_t s_expiry_us = 0;
static volatile uint32_t s_ttl_s = 0;

static phy_passthrough_emit_fn s_emit = NULL;

void phy_passthrough_set_emit(phy_passthrough_emit_fn fn) { s_emit = fn; }

int phy_passthrough_enable(uint32_t ttl_s, bool force, bool has_live_identity) {
    if (has_live_identity && !force) {
        return PHY_PT_ERR_IDENTITY;
    }
    if (ttl_s == 0u) {
        ttl_s = PHY_PT_DEFAULT_TTL_S;
    }
    if (ttl_s > PHY_PT_MAX_TTL_S) {
        ttl_s = PHY_PT_MAX_TTL_S;
    }

    s_expiry_us = esp_timer_get_time() + (int64_t)ttl_s * 1000000;
    s_ttl_s = ttl_s;
    s_forced = force;
    s_enabled = true; /* publish last: see the ordering note above */
    return PHY_PT_OK;
}

void phy_passthrough_disable(void) {
    s_enabled = false; /* clear first: readers gate on this before expiry */
    s_forced = false;
    s_expiry_us = 0;
    s_ttl_s = 0;
}

bool phy_passthrough_is_active(void) {
    if (!s_enabled) {
        return false;
    }
    if (esp_timer_get_time() >= s_expiry_us) {
        /* TTL elapsed: fold the auto-expire into a real disable so subsequent
         * status reads report the window as closed rather than merely stale. */
        phy_passthrough_disable();
        return false;
    }
    return true;
}

void phy_passthrough_get_status(phy_passthrough_status_t* out) {
    if (!out) {
        return;
    }
    bool active = phy_passthrough_is_active();
    out->enabled = s_enabled;
    out->active = active;
    out->forced = s_forced;
    out->ttl_s = s_ttl_s;
    if (active) {
        int64_t left_us = s_expiry_us - esp_timer_get_time();
        out->remaining_s = left_us > 0 ? (uint32_t)(left_us / 1000000) : 0u;
    } else {
        out->remaining_s = 0u;
    }
}

void phy_passthrough_forward_rx(const uint8_t* data, uint8_t len, const radio_rx_info_t* info,
                                uint32_t freq_hz) {
    if (!phy_passthrough_is_active() || s_emit == NULL || data == NULL || info == NULL) {
        return;
    }
    s_emit(data, len, info, freq_hz);
}
