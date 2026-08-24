#include "tx_gate.h"
#include "radio.h" /* bramble_calculate_airtime_us */

/*
 * Listen-Before-Talk policy (moved here from mesh_task so it sits behind
 * the budget check): up to 3 CAD attempts with randomized exponential
 * backoff. What happens when all three find the channel busy is per-kind:
 * most kinds transmit anyway to avoid starvation, the kinds in lbt_defers()
 * give the caller TX_GATE_ERR_CHANNEL_BUSY back instead.
 */
#define TX_GATE_LBT_MAX_ATTEMPTS 3u
#define TX_GATE_LBT_BACKOFF_BASE_MS 50u
#define TX_GATE_LBT_BACKOFF_MAX_MS 300u

/* Fallback ToA parameters used if the radio is not configured yet (boot
 * window before radio_init). Matches the freq_plan defaults: SF9, 125 kHz,
 * CR 4/5. No transmission can actually happen before radio_init; this only
 * keeps the math defined. */
#define TX_GATE_FALLBACK_SF 9u
#define TX_GATE_FALLBACK_BW_HZ 125000u
#define TX_GATE_FALLBACK_CR 1u

void tx_gate_init(tx_gate_t* g, const tx_gate_ops_t* ops, uint8_t max_duty_cycle_pct,
                  bool duty_cycle_enforced) {
    g->ops = *ops;
    airtime_budget_init(&g->budget, g->ops.now_ms());
    airtime_budget_set_duty_cap(&g->budget, max_duty_cycle_pct, duty_cycle_enforced);
    g->beacon_wire_len = 0u;
    g->beacon_budget_exempt = false;
}

/*
 * Kind -> tier mapping. Uses the existing four-lane budget model:
 *
 *   NORMAL     unicast data, retransmissions, relayed data, mailbox
 *              flushes, probe replies. The workhorse lane.
 *   CRITICAL   routing control (RREQ/RREP/RERR) and ACKs. No data path
 *              debits CRITICAL, so this lane is effectively a reserved
 *              sub-budget: route repair and ACK delivery cannot be starved
 *              by data load. CRITICAL may additionally borrow from NORMAL
 *              (existing budget semantics), so control traffic degrades
 *              last under congestion. An unsent ACK is the most expensive
 *              packet in the mesh: the peer retransmits at full data size.
 *   BROADCAST  beacons, broadcast data, probe sweeps: one lane for all
 *              traffic addressed to everyone, per the existing tier model.
 *   RECEIPT    broadcast delivery receipts, originated and relayed alike
 *              (lowest priority, own lane so receipt storms cannot crowd
 *              out anything else).
 */
uint8_t tx_gate_kind_tier(tx_kind_t kind) {
    switch (kind) {
    case TX_KIND_ROUTING:
    case TX_KIND_ACK:
        return AIRTIME_TIER_CRITICAL;
    case TX_KIND_BEACON:
    case TX_KIND_DATA_BROADCAST:
    case TX_KIND_PROBE:
        return AIRTIME_TIER_BROADCAST;
    case TX_KIND_RECEIPT:
    case TX_KIND_RECEIPT_FORWARD:
        return AIRTIME_TIER_RECEIPT;
    case TX_KIND_DATA:
    case TX_KIND_DATA_RETRY:
    case TX_KIND_FORWARD:
    case TX_KIND_MAILBOX:
    case TX_KIND_PROBE_REPLY:
    default:
        return AIRTIME_TIER_NORMAL;
    }
}

uint32_t tx_gate_cost_ms(tx_gate_t* g, uint8_t wire_len) {
    uint8_t sf = 0;
    uint32_t bw_hz = 0;
    uint8_t cr = 0;
    g->ops.get_toa_params(&sf, &bw_hz, &cr);
    if (sf < 5u || sf > 12u || bw_hz == 0u) {
        sf = TX_GATE_FALLBACK_SF;
        bw_hz = TX_GATE_FALLBACK_BW_HZ;
        cr = TX_GATE_FALLBACK_CR;
    }
    if (cr < 1u || cr > 4u)
        cr = TX_GATE_FALLBACK_CR;
    uint32_t us = bramble_calculate_airtime_us(wire_len, sf, bw_hz, cr);
    return (us + 999u) / 1000u; /* ceil: never undercount airtime */
}

/* One beacon's ToA held back from non-beacon BROADCAST spenders, so a
 * broadcast-data burst can never leave the lane too empty for the next
 * beacon. Paired with the interval floor (tx_gate_min_beacon_interval_ms)
 * this guarantees a beacon's tokens are present when its timer fires:
 * the stretched cadence consumes at most TX_GATE_BEACON_LANE_PCT of the
 * lane refill, and data can only drain the lane down to the reserve. */
static uint32_t beacon_reserve_ms(tx_gate_t* g, uint8_t tier, tx_kind_t kind) {
    if (tier != AIRTIME_TIER_BROADCAST || kind == TX_KIND_BEACON || g->beacon_wire_len == 0u)
        return 0u;
    return tx_gate_cost_ms(g, g->beacon_wire_len);
}

/* True when this send is an emulator-exempt beacon: skips the budget check
 * AND the debit (see tx_gate_set_beacon_budget_exempt_core), so data lanes
 * in the same run stay faithfully budgeted. */
static bool beacon_exempt(const tx_gate_t* g, tx_kind_t kind) {
    return g->beacon_budget_exempt && kind == TX_KIND_BEACON;
}

/*
 * Which kinds refuse to blind-fire when LBT never finds a quiet channel.
 *
 * TX_KIND_RECEIPT only. An originated broadcast delivery receipt is the one
 * kind that can genuinely afford to wait: the app layer already gives it
 * three jittered attempts and its own 12s/hour airtime lane, and nothing
 * downstream blocks on it. It is also the kind that suffers most from
 * blind-firing, because receipts arrive as a storm: every node that stored
 * a broadcast answers the same origin at once, so "channel busy" here means
 * "another receipt is on the air right now", and transmitting into it loses
 * the capture battle at the origin. That is the measured defect: bench
 * traces put broadcast receipt loss at 20-25%, and the app-layer retry
 * jitter could not fix it because all three attempts blind-fired the same
 * way. Deferring turns a near-certain collision into a later, quiet send.
 *
 * Nothing else defers, deliberately:
 *   - DATA / DATA_RETRY / FORWARD / MAILBOX / DATA_BROADCAST: the workhorse
 *     lanes. A busy channel is the normal state under load, and a deferring
 *     data path starves exactly when the mesh is busiest.
 *   - ACK / ROUTING: an undelivered ACK costs a full-size retransmission and
 *     an undelivered RREP costs a route; these must never yield the channel.
 *   - BEACON / PROBE / PROBE_REPLY: cadence-driven, no retry structure to
 *     defer into, and beacons are the liveness backbone.
 *   - RECEIPT_FORWARD: a relayed receipt carries the originator's bytes and
 *     seq, so it cannot be re-originated here, and the originator's own
 *     retries are replay-rejected downstream once this hop has passed one
 *     copy on. There is no retry structure to defer into (sharing the 8-slot
 *     receipt queue would let relay traffic starve this node's own receipts
 *     in precisely the storm this change targets), so relays keep the
 *     existing blind-fire behavior.
 *
 * The defer set being receipt-only is also what keeps TX_GATE_ERR_CHANNEL_BUSY
 * safe for every existing caller: the only call sites that pass
 * TX_KIND_RECEIPT are in main/mesh_reliability.c and both handle it. Every
 * other call site can only ever see the return codes it sees today.
 */
static bool lbt_defers(tx_kind_t kind) { return kind == TX_KIND_RECEIPT; }

int tx_gate_transmit(tx_gate_t* g, const uint8_t* buf, uint8_t len, tx_kind_t kind) {
    uint8_t tier = tx_gate_kind_tier(kind);
    uint32_t cost_ms = tx_gate_cost_ms(g, len);
    bool exempt = beacon_exempt(g, kind);

    airtime_budget_refill(&g->budget, g->ops.now_ms());
    if (!exempt && !airtime_budget_can_transmit(&g->budget, tier,
                                                cost_ms + beacon_reserve_ms(g, tier, kind))) {
        return TX_GATE_ERR_BUDGET;
    }

    /* Listen-Before-Talk: budget approved, now wait for a quiet channel.
     * `quiet` records whether a CAD actually found the channel free, which
     * is what separates "the channel went quiet" from "the attempts ran
     * out while it was still busy". Tracking it costs nothing: the quiet
     * path spends exactly one CAD either way. */
    bool quiet = false;
    for (uint8_t attempt = 0; attempt < TX_GATE_LBT_MAX_ATTEMPTS; attempt++) {
        if (g->ops.wdt_feed)
            g->ops.wdt_feed();
        if (!g->ops.channel_busy()) {
            quiet = true;
            break;
        }
        uint32_t backoff_ms = TX_GATE_LBT_BACKOFF_BASE_MS * (1u << attempt);
        if (backoff_ms > TX_GATE_LBT_BACKOFF_MAX_MS)
            backoff_ms = TX_GATE_LBT_BACKOFF_MAX_MS;
        backoff_ms += g->ops.random_u32() % backoff_ms;
        g->ops.delay_ms(backoff_ms);
    }
    /* Attempts exhausted on a busy channel: deferring kinds hand the
     * decision back to the caller, every other kind transmits anyway to
     * avoid starvation. */
    if (!quiet && lbt_defers(kind))
        return TX_GATE_ERR_CHANNEL_BUSY;

    if (g->ops.wdt_feed)
        g->ops.wdt_feed();
    int ret = g->ops.transmit(buf, len);
    if (ret != 0)
        return TX_GATE_ERR_RADIO;

    if (!exempt)
        airtime_budget_debit(&g->budget, tier, cost_ms);
    return TX_GATE_OK;
}

bool tx_gate_can_transmit(tx_gate_t* g, uint8_t wire_len, tx_kind_t kind) {
    if (beacon_exempt(g, kind))
        return true;
    uint8_t tier = tx_gate_kind_tier(kind);
    uint32_t cost_ms = tx_gate_cost_ms(g, wire_len);
    airtime_budget_refill(&g->budget, g->ops.now_ms());
    return airtime_budget_can_transmit(&g->budget, tier,
                                       cost_ms + beacon_reserve_ms(g, tier, kind));
}

void tx_gate_set_beacon_budget_exempt_core(tx_gate_t* g, bool exempt) {
    g->beacon_budget_exempt = exempt;
}

void tx_gate_set_mesh_size(tx_gate_t* g, uint8_t peer_count) {
    airtime_budget_set_mesh_size(&g->budget, peer_count);
}

void tx_gate_set_beacon_profile(tx_gate_t* g, uint8_t beacon_wire_len) {
    g->beacon_wire_len = beacon_wire_len;
}

uint32_t tx_gate_min_beacon_interval_ms(tx_gate_t* g) {
    if (g->beacon_budget_exempt)
        return 0u; /* emulator-exempt beacons have no budget floor */
    if (g->beacon_wire_len == 0u)
        return 0u;
    uint32_t cost_ms = tx_gate_cost_ms(g, g->beacon_wire_len);
    /* Hourly refill of the BROADCAST lane equals its capacity. */
    uint64_t lane_per_hr = g->budget.max_ms[AIRTIME_IDX_BROADCAST];
    uint64_t beacon_budget_per_hr = (lane_per_hr * TX_GATE_BEACON_LANE_PCT) / 100u;
    if (beacon_budget_per_hr == 0u)
        return AIRTIME_REFILL_INTERVAL_MS;
    /* interval >= cost * hour / budget  (ceil), so that
     * (hour / interval) beacons cost at most beacon_budget_per_hr. */
    uint64_t interval =
        ((uint64_t)cost_ms * AIRTIME_REFILL_INTERVAL_MS + beacon_budget_per_hr - 1u) /
        beacon_budget_per_hr;

    /* Liveness ceiling: rather than stretch past (and purge out of)
     * neighbor tables, let beacons consume up to the full lane. */
    if (interval > TX_GATE_BEACON_LIVENESS_CEILING_MS && lane_per_hr > 0u) {
        uint64_t full_lane_floor =
            ((uint64_t)cost_ms * AIRTIME_REFILL_INTERVAL_MS + lane_per_hr - 1u) / lane_per_hr;
        interval = (full_lane_floor > TX_GATE_BEACON_LIVENESS_CEILING_MS)
                       ? full_lane_floor
                       : TX_GATE_BEACON_LIVENESS_CEILING_MS;
    }

    if (interval > AIRTIME_REFILL_INTERVAL_MS)
        interval = AIRTIME_REFILL_INTERVAL_MS;
    return (uint32_t)interval;
}
