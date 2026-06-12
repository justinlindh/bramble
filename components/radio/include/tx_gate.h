#ifndef BRAMBLE_TX_GATE_H
#define BRAMBLE_TX_GATE_H

#include <stdint.h>
#include <stdbool.h>
#include "airtime_budget.h"

/*
 * tx_gate: the single chokepoint for every LoRa transmission.
 *
 * Sequence enforced for every packet, no exceptions:
 *   1. airtime budget check (real Semtech ToA at the live radio config)
 *   2. Listen-Before-Talk (CAD with bounded randomized backoff)
 *   3. raw radio transmit
 *   4. budget debit of the same ToA that was checked
 *
 * The raw radio TX primitive is private to the radio component; the only
 * way to put bytes on the air is through this gate.
 */

/* What is being transmitted. The gate maps each kind onto one of the four
 * existing airtime budget tiers; callers pick the kind, never the tier. */
typedef enum {
    TX_KIND_DATA = 0,       /* unicast/channel data, original send */
    TX_KIND_DATA_BROADCAST, /* broadcast data (public channel, fragments) */
    TX_KIND_DATA_RETRY,     /* ACK-driven retransmission of pending data */
    TX_KIND_FORWARD,        /* relaying another node's data packet */
    TX_KIND_MAILBOX,        /* mailbox flush of stored packets */
    TX_KIND_BEACON,         /* periodic presence beacon */
    TX_KIND_ROUTING,        /* RREQ / RREP / RERR control */
    TX_KIND_ACK,            /* ACK send and ACK forward */
    TX_KIND_RECEIPT,        /* broadcast delivery receipts (send + forward) */
    TX_KIND_PROBE,          /* probe sweep rounds and probe forwards */
    TX_KIND_PROBE_REPLY,    /* probe ACK send and forward */
    TX_KIND_COUNT
} tx_kind_t;

/* Return codes for tx_gate_transmit / tx_gate_send. */
#define TX_GATE_OK 0
#define TX_GATE_ERR_RADIO (-1)  /* radio driver rejected the transmit */
#define TX_GATE_ERR_BUDGET (-2) /* airtime budget denied; nothing transmitted */

/* Dependency injection surface. Host tests provide fakes; firmware glue
 * (tx_gate_esp.c) binds the real radio, timer, and RTOS primitives. */
typedef struct {
    /* CAD check: true = channel busy. */
    bool (*channel_busy)(void);
    /* Raw radio transmit. Returns 0 on success. */
    int (*transmit)(const uint8_t* data, uint8_t len);
    /* Live LoRa parameters for ToA math (SF, bandwidth Hz, coding rate 1..4). */
    void (*get_toa_params)(uint8_t* sf, uint32_t* bw_hz, uint8_t* cr);
    uint32_t (*now_ms)(void);
    uint32_t (*random_u32)(void);
    void (*delay_ms)(uint32_t ms);
    /* Optional (may be NULL): feed the task watchdog around blocking waits. */
    void (*wdt_feed)(void);
} tx_gate_ops_t;

typedef struct {
    tx_gate_ops_t ops;
    airtime_budget_t budget;
    /* Wire size of this node's beacon (registered at each beacon TX).
     * Funds the beacon reserve and the budget-derived minimum beacon
     * interval; 0 = unknown (no reserve, no interval floor). */
    uint8_t beacon_wire_len;
} tx_gate_t;

/* Share of the BROADCAST lane's hourly refill that beacon cadence may
 * consume; the remainder stays available to broadcast data and probes.
 * Beacons are the liveness backbone (neighbor tables live on them), so
 * they get the larger share and broadcast data is the elastic party. */
#define TX_GATE_BEACON_LANE_PCT 60u

/* Liveness ceiling for the budget-derived beacon interval: 80% of
 * routing.h NEIGHBOR_EXPIRY_MS (600000). When the 60% share would stretch
 * the cadence past this, beacons may consume up to the FULL lane instead
 * (broadcast data yields entirely): losing broadcast data beats vanishing
 * from neighbor tables. Even the full lane cannot always fund a beacon per
 * ceiling: the over-expiry regime includes EU868 at SF11/12, EU868 SF10
 * above 40 peers (~697s), and US915 SF12 above 40 peers (~702s). In that
 * regime peers purge this node between beacons (NEIGHBOR_EXPIRY_MS) and
 * rediscover it on the next beacon; unicast still works via RREQ discovery
 * with added latency. The bound comes from the BROADCAST lane allocation
 * (16-32% of the duty-halved window target), not from the regulatory duty
 * cycle itself: a 600s SF12 beacon cadence costs ~0.35% duty, so a beacon-
 * first lane allocation could relax this without touching the regulatory
 * cap. The stretch is deterministic, never a random denial. */
#define TX_GATE_BEACON_LIVENESS_CEILING_MS 480000u

/* ── Core API (host-testable, no RTOS deps) ─────────────────────────── */

void tx_gate_init(tx_gate_t* g, const tx_gate_ops_t* ops, uint8_t max_duty_cycle_pct,
                  bool duty_cycle_enforced);

/* Budget tier (AIRTIME_TIER_*) a given kind debits. */
uint8_t tx_gate_kind_tier(tx_kind_t kind);

/* Real time-on-air cost in ms (ceil) for a wire_len-byte packet at the
 * live radio configuration. */
uint32_t tx_gate_cost_ms(tx_gate_t* g, uint8_t wire_len);

/* Budget check -> LBT -> transmit -> debit. Returns TX_GATE_OK,
 * TX_GATE_ERR_BUDGET (denied, radio untouched) or TX_GATE_ERR_RADIO. */
int tx_gate_transmit(tx_gate_t* g, const uint8_t* buf, uint8_t len, tx_kind_t kind);

/* Non-mutating pre-check: would a packet of wire_len pass the budget now?
 * (Refills the bucket; does not debit or touch the radio.) */
bool tx_gate_can_transmit(tx_gate_t* g, uint8_t wire_len, tx_kind_t kind);

void tx_gate_set_mesh_size(tx_gate_t* g, uint8_t peer_count);

/* Register the node's beacon wire size (called at each beacon TX, so the
 * reserve and the interval floor always track the live beacon and SF). */
void tx_gate_set_beacon_profile(tx_gate_t* g, uint8_t beacon_wire_len);

/* Minimum beacon interval such that beacon cadence ToA fits inside
 * TX_GATE_BEACON_LANE_PCT of the BROADCAST lane's hourly refill at the
 * live radio config. The beacon scheduler stretches its interval to at
 * least this, so beacons fit the budget BY DESIGN instead of being denied
 * at transmit time. Returns 0 while the beacon size is unknown. */
uint32_t tx_gate_min_beacon_interval_ms(tx_gate_t* g);

/* ── Firmware singleton (tx_gate_esp.c); thread-safe wrappers ───────── */

void tx_gate_global_init(uint8_t max_duty_cycle_pct, bool duty_cycle_enforced);
int tx_gate_send(const uint8_t* buf, uint8_t len, tx_kind_t kind);
bool tx_gate_check(uint8_t wire_len, tx_kind_t kind);
void tx_gate_set_peer_count(uint8_t peer_count);
void tx_gate_set_beacon_size(uint8_t beacon_wire_len);
uint32_t tx_gate_beacon_min_interval(void);
uint32_t tx_gate_remaining(uint8_t tier);
void tx_gate_snapshot(airtime_budget_t* out);

#endif
