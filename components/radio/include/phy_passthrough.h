#ifndef BRAMBLE_PHY_PASSTHROUGH_H
#define BRAMBLE_PHY_PASSTHROUGH_H

#include <stdbool.h>
#include <stdint.h>
#include "radio.h"

/*
 * PHY passthrough: the hardware-bridge mode (DESIGN.md section 10).
 *
 * When active, every raw frame the radio receives is forwarded up the
 * RPC/serial link with its radio metadata (rssi/snr/freq) BEFORE mesh
 * processing, and frames handed down over RPC (phy.tx) are transmitted raw.
 * A real node in this mode becomes one PHY member of the emulator's ether
 * (the gosim gateway client bridges it in).
 *
 * Because this puts arbitrary bytes on the real channel, it is gated hard
 * (all mandatory, section 10):
 *   - disabled by default;
 *   - enabled only via an authenticated RPC call (the phy.* methods are not
 *     on the unauthenticated allowlist, and the serial CLI is authenticated
 *     by physical access);
 *   - auto-expires after a configurable TTL (default 30 min);
 *   - never persists across reboot (module state only, no NVS anywhere);
 *   - refuses to enable while the node holds a live channel identity unless
 *     the caller forces it.
 *
 * This module owns only the gate state and the RX forward tap. It has no
 * json/rpc dependency: the actual notification is emitted through a hook the
 * RPC layer registers (phy_passthrough_set_emit), which keeps the gating
 * logic pure and host-testable and keeps the radio component free of a
 * dependency on the rpc layer.
 */

/* Default and ceiling for the enable TTL (seconds). */
#define PHY_PT_DEFAULT_TTL_S (30u * 60u)   /* 30 minutes */
#define PHY_PT_MAX_TTL_S (24u * 60u * 60u) /* 24 hours */

/* phy_passthrough_enable return codes. */
#define PHY_PT_OK 0
#define PHY_PT_ERR_IDENTITY 1 /* live channel identity held and force==false */

typedef struct {
    bool enabled;         /* enable flag (independent of TTL) */
    bool active;          /* enabled AND not yet TTL-expired */
    bool forced;          /* enabled with force==true (identity override) */
    uint32_t ttl_s;       /* configured TTL for the current window */
    uint32_t remaining_s; /* seconds until auto-expire (0 when inactive) */
} phy_passthrough_status_t;

/*
 * Frame-forward hook. Registered by the RPC layer; invoked (when active) for
 * every received frame with the raw bytes and radio metadata. freq_hz is the
 * live channel carrier in Hz. The hook must copy anything it needs; data is
 * only valid for the call.
 */
typedef void (*phy_passthrough_emit_fn)(const uint8_t* data, uint8_t len,
                                        const radio_rx_info_t* info, uint32_t freq_hz);

void phy_passthrough_set_emit(phy_passthrough_emit_fn fn);

/*
 * Enable passthrough. ttl_s==0 selects PHY_PT_DEFAULT_TTL_S; values above
 * PHY_PT_MAX_TTL_S are clamped. has_live_identity is computed by the caller
 * (whether the node is a live mesh participant). Returns PHY_PT_OK, or
 * PHY_PT_ERR_IDENTITY when has_live_identity && !force (nothing is enabled).
 */
int phy_passthrough_enable(uint32_t ttl_s, bool force, bool has_live_identity);

/* Disable passthrough immediately. Idempotent. */
void phy_passthrough_disable(void);

/* True iff enabled and the TTL has not elapsed. Cheap; called per RX frame. */
bool phy_passthrough_is_active(void);

/*
 * Drain the one-shot auto-expiry latch. Returns true exactly once after the TTL
 * elapses (the live->off transition folded into is_active), then false until the
 * next auto-expiry. Lets a logging-capable caller emit that transition once
 * without giving this module a logging dependency.
 */
bool phy_passthrough_consume_auto_expired(void);

/* Fill *out with the current gate state (TTL remaining computed live). */
void phy_passthrough_get_status(phy_passthrough_status_t* out);

/*
 * RX tap: forward one received frame up the emit hook when active. No-op when
 * inactive or no hook is registered. Called from the radio RX callback before
 * the frame is queued for mesh processing.
 */
void phy_passthrough_forward_rx(const uint8_t* data, uint8_t len, const radio_rx_info_t* info,
                                uint32_t freq_hz);

#endif /* BRAMBLE_PHY_PASSTHROUGH_H */
