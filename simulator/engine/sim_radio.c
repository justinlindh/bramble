#include "sim_radio.h"
#include "freq_plan.h"
#include "sim_emitter.h"
#include <math.h>
#include <string.h>

/*
 * Radio model
 * ===========
 *
 * Deliverability is disk-range gated (distance > range never delivers), with
 * optional uniform residual loss (loss_pct) and interference zones on top.
 * Within range, the model adds:
 *
 *  - Real time-on-air: every frame occupies the shared medium for the LoRa
 *    ToA of its byte length at the configured SF/BW/CR, computed by the
 *    firmware's own bramble_calculate_airtime_us (Semtech AN1200.13).
 *  - Half-duplex: a node cannot receive while transmitting, and its own
 *    transmissions are serialized (a new TX waits for the current one).
 *  - Collisions: two packets overlapping in time at a receiver, both audible
 *    there, destroy each other unless the capture effect applies.
 *  - Capture effect: the packet that is at least capture_db (default 6 dB)
 *    stronger survives the overlap, provided it started first or within the
 *    interferer's preamble window (the receiver can still re-sync during the
 *    preamble). The 6 dB co-SF capture threshold follows Bor, Roedig, Voigt,
 *    Alonso, "Do LoRa Low-Power Wide-Area Networks Scale?" (MSWiM 2016) and
 *    the SX127x/SX126x co-channel rejection figures.
 *
 * Default PHY: taken from the frequency plan, not from the radio profile
 * table, because that is what the firmware transmits at. mesh_task.c's
 * mesh_init_radio_config loads RADIO_PROFILE_LONG_RANGE and then overwrites
 * its sf/bw_hz with freq_plan_get_default()->default_sf/default_bw_hz, so a
 * real node's boot log reads "Radio config: 915.0 MHz SF9 BW125000", not the
 * profile's SF10. Reading the plan here (see radio_config_init) keeps the two
 * from drifting apart: change a plan's default_sf and the simulated medium
 * follows the firmware automatically.
 *
 * RSSI gradient: log-distance path loss,
 *     RSSI(d) = tx_power_dbm - PL(d0) - 10 n log10(d / d0)
 * with d0 = 1 grid unit (= 10 m, the simulator's position scale), PL(d0) =
 * 52 dB (free-space loss at 10 m, 915 MHz) and n = 2.9 (typical suburban
 * outdoor value; log-distance exponents of 2.7-3.5 are standard for built-up
 * terrain). With the firmware default 22 dBm TX power this lands at about
 * -30 dBm at 10 m and -93 dBm at the default ~150-unit (1.5 km) range edge.
 *
 * Range derives from SF/BW, not a fixed disk: config->range is the distance
 * at which the RSSI gradient above crosses radio_sensitivity_dbm(sf, bw_hz)
 * (see radio_derive_range). Higher SF has a lower (more negative) sensitivity
 * threshold and therefore more link budget and longer range; wider bandwidth
 * raises the noise floor and shortens it. A scenario that sets "range"
 * explicitly overrides the derivation (sim_scenario.c load_radio); otherwise
 * range is recomputed from whatever sf/bw_hz/tx_power_dbm/path_loss_* the
 * scenario configured. See radio_sensitivity_dbm for the SF/BW sensitivity
 * model and radio_noise_margin_db for its calibration anchor.
 *
 * Not modeled, on purpose: real RF terrain/fading (range disk stands in for
 * it), external interference from other networks, inter-SF interference
 * (single shared channel and SF assumed), and propagation-delay offsets of
 * the occupancy window (delivery timing includes propagation delay, overlap
 * is computed on TX windows; at LoRa scales propagation is microseconds
 * against ToA of hundreds of milliseconds).
 */

/* Firmware ToA function (components/radio/radio_airtime.c); declared here
 * instead of including radio.h because the firmware header defines its own,
 * unrelated radio_config_t. */
extern uint32_t bramble_calculate_airtime_us(uint16_t payload_bytes, uint8_t sf, uint32_t bw_hz,
                                             uint8_t cr);

/* Default link-budget params, named so the radio_noise_margin_db derivation
 * below provably uses the same numbers radio_config_init installs. */
#define SIM_DEFAULT_TX_POWER_DBM 22
#define SIM_DEFAULT_PATH_LOSS_D0_DB 52.0f
#define SIM_DEFAULT_PATH_LOSS_EXP 2.9f

/* The simulator's reception-range baseline, in grid units
 * (1 unit = 10 m, so 1.5 km). The scenario corpus is laid out around this
 * disk (120-unit grid spacing so orthogonal neighbors, and only they, are
 * audible), so it is a fixed point of the model: the derived range at the
 * firmware's default PHY must reproduce it. radio_noise_margin_db is the
 * constant that holds that anchor. */
#define SIM_BASELINE_RANGE_UNITS 150.0f

uint8_t radio_default_sf(void) {
    uint8_t sf = freq_plan_get_default()->default_sf;
    /* Guard the sensitivity table's index: every shipped plan is inside 7..12,
     * so this only fires if a future plan is edited out of range. */
    return (sf >= 7 && sf <= 12) ? sf : 10;
}

uint32_t radio_default_bw_hz(void) {
    uint32_t bw = freq_plan_get_default()->default_bw_hz;
    return bw ? bw : 125000;
}

void radio_config_init(radio_config_t* config) {
    memset(config, 0, sizeof(*config));
    config->loss_pct = 0.0f;
    config->propagation_speed_ms_per_unit = 0.1f;

    /* The PHY the firmware actually transmits at: the frequency plan's
     * defaults, which mesh_init_radio_config writes over the radio profile's
     * sf/bw_hz (SF9/125 kHz on every shipped plan). CR and TX power still come
     * from RADIO_PROFILE_LONG_RANGE, which the plan does not override (it only
     * clamps power to the regulatory max, and 22 dBm is inside US915's 30). */
    config->sf = radio_default_sf();
    config->bw_hz = radio_default_bw_hz();
    config->cr = 1; /* 4/5 */
    config->tx_power_dbm = SIM_DEFAULT_TX_POWER_DBM;

    config->collisions_enabled = true;
    config->lbt_enabled = true;
    config->capture_db = 6.0f;
    config->path_loss_exp = SIM_DEFAULT_PATH_LOSS_EXP;
    config->path_loss_d0_db = SIM_DEFAULT_PATH_LOSS_D0_DB;

    config->duty_cycle_set = false; /* unlimited: today's behavior */
    config->duty_cycle_pct = 0;

    /* Range derives from the PHY params just set, landing on
     * SIM_BASELINE_RANGE_UNITS by construction; see radio_derive_range and
     * radio_noise_margin_db. Must run last: it reads sf/bw_hz/tx_power_dbm/
     * path_loss_* above. */
    config->range = radio_derive_range(config);
}

/* SX127x/SX126x datasheet receiver sensitivity at 125 kHz bandwidth, dBm,
 * indexed by spreading factor 7..12 (kSfSensitivity125kDbm[sf - 7]). */
static const float kSfSensitivity125kDbm[6] = {
    -123.0f, /* SF7 */
    -126.0f, /* SF8 */
    -129.0f, /* SF9 */
    -132.0f, /* SF10 */
    -134.5f, /* SF11 */
    -137.0f, /* SF12 */
};

/* Bandwidth term of the sensitivity model: wider bandwidth admits more noise,
 * +10*log10(bw/125000) dB relative to the 125 kHz datasheet column. */
static float bw_noise_adj_db(uint32_t bw_hz) {
    if (bw_hz == 0)
        bw_hz = 125000;
    return 10.0f * log10f((float)bw_hz / 125000.0f);
}

/*
 * radio_noise_margin_db: single additive calibration constant folded into
 * radio_sensitivity_dbm. This simulator's path-loss model (path_loss_d0_db =
 * 52 dB, path_loss_exp = 2.9) is far lossier at short grid distances than a
 * real link budget with datasheet sensitivity would imply (the raw budget
 * would put the default PHY's range past 2000 grid units); the simulator's own
 * ~150-unit range is set for test scale, not physical realism.
 * This margin folds that difference into a single "noise figure +
 * implementation margin" offset so the derived range at the FIRMWARE'S DEFAULT
 * PHY reproduces SIM_BASELINE_RANGE_UNITS exactly (the scenario corpus is
 * laid out around that disk, and the ones that omit "range" derive it), while
 * SF/BW deltas relative to that anchor follow the real datasheet deltas.
 *
 * The anchor is computed from the frequency plan rather than hardcoded, so it
 * tracks the default PHY instead of silently moving every unpinned scenario's
 * topology when a plan's default_sf changes. At the shipped default
 * (SF9/125 kHz, base sensitivity -129 dBm):
 *   margin = tx_power - path_loss_d0_db - base_sens(SF9)
 *            - 10 * path_loss_exp * log10(150)
 *          = 22 - 52 - (-129) - 29 * log10(150)
 *          = 99 - 63.107
 *          = 35.89 dB.
 * A plan defaulting to SF10 would put it at 38.9 dB instead: the 3 dB gap is
 * exactly the SF9-to-SF10 datasheet sensitivity step, so moving the default
 * shifts every non-default SF/BW range by that one step while the default PHY
 * keeps the 150-unit baseline.
 */
static float radio_noise_margin_db(void) {
    float base = kSfSensitivity125kDbm[radio_default_sf() - 7];
    base += bw_noise_adj_db(radio_default_bw_hz());
    return (float)SIM_DEFAULT_TX_POWER_DBM - SIM_DEFAULT_PATH_LOSS_D0_DB - base -
           10.0f * SIM_DEFAULT_PATH_LOSS_EXP * log10f(SIM_BASELINE_RANGE_UNITS);
}

float radio_sensitivity_dbm(uint8_t sf, uint32_t bw_hz) {
    if (sf < 7 || sf > 12)
        sf = radio_default_sf();
    if (bw_hz == 0)
        bw_hz = radio_default_bw_hz();
    float base = kSfSensitivity125kDbm[sf - 7];
    return base + bw_noise_adj_db(bw_hz) + radio_noise_margin_db();
}

float radio_derive_range(const radio_config_t* config) {
    uint8_t sf = config->sf ? config->sf : radio_default_sf();
    uint32_t bw = config->bw_hz ? config->bw_hz : radio_default_bw_hz();
    float path_loss_exp =
        config->path_loss_exp > 0.0f ? config->path_loss_exp : SIM_DEFAULT_PATH_LOSS_EXP;
    float sensitivity = radio_sensitivity_dbm(sf, bw);
    float budget_db = (float)config->tx_power_dbm - config->path_loss_d0_db - sensitivity;
    if (budget_db <= 0.0f)
        return 1.0f; /* degenerate config: no usable link past d0 */
    return powf(10.0f, budget_db / (10.0f * path_loss_exp));
}

float radio_distance(const sim_node_t* a, const sim_node_t* b) {
    float dx = a->x - b->x;
    float dy = a->y - b->y;
    return sqrtf(dx * dx + dy * dy);
}

/* Log-distance path loss; float dB for capture comparisons. */
static float path_rssi_dbm(const radio_config_t* config, float distance) {
    if (distance < 1.0f)
        distance = 1.0f; /* inside d0: clamp to the reference distance */
    float rssi = (float)config->tx_power_dbm - config->path_loss_d0_db -
                 10.0f * config->path_loss_exp * log10f(distance);
    if (rssi < -120.0f)
        rssi = -120.0f;
    if (rssi > -30.0f)
        rssi = -30.0f;
    return rssi;
}

int8_t radio_compute_rssi(const radio_config_t* config, float distance) {
    return (int8_t)path_rssi_dbm(config, distance);
}

/* ── Link-graph mode ──────────────────────────────────────────────────── */

/* Raw table read, bounds-checked. Zero means "no link" (see radio_config_t). */
static int8_t link_rssi_at(const radio_config_t* config, int from, int to) {
    if (from < 0 || to < 0 || from >= MAX_NODES || to >= MAX_NODES)
        return 0;
    return config->link_rssi[from][to];
}

bool radio_link_set(radio_config_t* config, int from, int to, int8_t rssi, int8_t snr) {
    if (from < 0 || to < 0 || from >= MAX_NODES || to >= MAX_NODES || from == to)
        return false;
    if (rssi == 0)
        return false; /* zero is the absent marker; an observed link cannot use it */
    config->link_mode = true;
    config->link_rssi[from][to] = rssi;
    config->link_snr[from][to] = snr;
    return true;
}

/* Audibility of one logged transmission at `node`. The channel log carries the
 * transmitter's index alongside its position so both modes can answer from the
 * same entry. */
static bool audible_from_log(const radio_config_t* config, const channel_tx_t* e,
                             const sim_node_t* node) {
    if (config->link_mode)
        return link_rssi_at(config, e->tx_index, node->index) != 0;
    float dx = node->x - e->tx_x;
    float dy = node->y - e->tx_y;
    return sqrtf(dx * dx + dy * dy) <= config->range;
}

bool radio_audible(const radio_config_t* config, const sim_node_t* tx, const sim_node_t* rx) {
    if (config->link_mode)
        return link_rssi_at(config, tx->index, rx->index) != 0;
    return radio_distance(tx, rx) <= config->range;
}

bool radio_nodes_connected(const radio_config_t* config, const sim_node_t* a, const sim_node_t* b) {
    return radio_audible(config, a, b) && radio_audible(config, b, a);
}

bool radio_can_receive(const radio_config_t* config, const sim_node_t* tx, const sim_node_t* rx,
                       pcg32_state_t* rng) {
    /* Both must be active */
    if (!tx->active || !rx->active)
        return false;

    /* Reachable at all: range disk, or the imported link table */
    if (!radio_audible(config, tx, rx))
        return false;

    /* Check interference */
    if (radio_in_interference(config, rx))
        return false;

    /* Random packet loss */
    if (config->loss_pct > 0.0f) {
        float roll = pcg32_float(rng) * 100.0f;
        if (roll < config->loss_pct)
            return false;
    }

    return true;
}

uint64_t radio_propagation_delay_us(const radio_config_t* config, float distance) {
    float delay_ms = distance * config->propagation_speed_ms_per_unit;
    return (uint64_t)(delay_ms * 1000.0f);
}

int radio_add_interference_zone(radio_config_t* config, float center_x, float center_y,
                                float radius) {
    if (config->zone_count >= MAX_INTERFERENCE_ZONES)
        return -1;

    int idx = config->zone_count++;
    config->zones[idx].center_x = center_x;
    config->zones[idx].center_y = center_y;
    config->zones[idx].radius = radius;
    config->zones[idx].active = true;
    return idx;
}

void radio_clear_interference_zone(radio_config_t* config, int index) {
    if (index >= 0 && index < config->zone_count)
        config->zones[index].active = false;
}

bool radio_in_interference(const radio_config_t* config, const sim_node_t* node) {
    for (int i = 0; i < config->zone_count; i++) {
        if (!config->zones[i].active)
            continue;

        float dx = node->x - config->zones[i].center_x;
        float dy = node->y - config->zones[i].center_y;
        float dist = sqrtf(dx * dx + dy * dy);

        if (dist <= config->zones[i].radius)
            return true;
    }
    return false;
}

uint32_t radio_frame_airtime_us(const radio_config_t* config, uint16_t frame_bytes) {
    uint8_t sf = config->sf ? config->sf : radio_default_sf();
    uint32_t bw = config->bw_hz ? config->bw_hz : radio_default_bw_hz();
    uint8_t cr = config->cr ? config->cr : 1;
    return bramble_calculate_airtime_us(frame_bytes, sf, bw, cr);
}

uint32_t radio_frame_airtime_ms(const radio_config_t* config, uint16_t frame_bytes) {
    uint32_t us = radio_frame_airtime_us(config, frame_bytes);
    uint32_t ms = (us + 999u) / 1000u;
    return ms ? ms : 1u;
}

uint64_t radio_preamble_us(const radio_config_t* config) {
    uint8_t sf = config->sf ? config->sf : radio_default_sf();
    uint32_t bw = config->bw_hz ? config->bw_hz : radio_default_bw_hz();
    double t_sym_us = (double)(1u << sf) * 1e6 / (double)bw;
    /* Mirrors bramble_calculate_airtime_us: 12 programmed symbols for SF>=9,
     * 8 below, plus 4.25 sync symbols. */
    double n_preamble = (sf >= 9) ? 12.0 : 8.0;
    return (uint64_t)((n_preamble + 4.25) * t_sym_us + 0.5);
}

/* ── Channel occupancy log ────────────────────────────────────────────── */

static void channel_log_prune(channel_log_t* log, uint64_t now_us) {
    uint64_t cutoff = (now_us > CHANNEL_LOG_RETENTION_US) ? now_us - CHANNEL_LOG_RETENTION_US : 0;
    int w = 0;
    for (int i = 0; i < log->count; i++) {
        if (log->entries[i].end_us >= cutoff)
            log->entries[w++] = log->entries[i];
    }
    log->count = w;
}

static void channel_log_add(channel_log_t* log, uint32_t tx_addr, int tx_index, float x, float y,
                            uint64_t start_us, uint64_t end_us, uint64_t now_us) {
    channel_log_prune(log, now_us);
    if (log->count >= MAX_CHANNEL_TX) {
        /* Evict the oldest in-retention entry; count it so saturation shows */
        memmove(&log->entries[0], &log->entries[1],
                sizeof(channel_tx_t) * (size_t)(MAX_CHANNEL_TX - 1));
        log->count = MAX_CHANNEL_TX - 1;
        log->overflow_drops++;
    }
    channel_tx_t* e = &log->entries[log->count++];
    e->tx_addr = tx_addr;
    e->tx_index = tx_index;
    e->tx_x = x;
    e->tx_y = y;
    e->start_us = start_us;
    e->end_us = end_us;
}

/* Carrier sense: is any other audible transmission on the air at time t?
 * Models the SX126x CAD check as deterministic energy detection over whatever
 * defines audibility (the range disk, or the imported link table in link mode;
 * documented simplification: real CAD is preamble-biased and probabilistic). */
static bool channel_busy_at(const radio_config_t* config, const sim_node_t* node, uint64_t t) {
    const channel_log_t* log = &config->channel;
    for (int i = 0; i < log->count; i++) {
        const channel_tx_t* e = &log->entries[i];
        if (e->tx_addr == node->addr)
            continue;
        if (t < e->start_us || t >= e->end_us)
            continue;
        if (!audible_from_log(config, e, node))
            continue;
        return true;
    }
    return false;
}

radio_rx_outcome_t radio_check_reception(const radio_config_t* config, const sim_node_t* rx,
                                         const packet_event_data_t* pkt) {
    if (!config->collisions_enabled)
        return RADIO_RX_OK;

    const channel_log_t* log = &config->channel;
    uint64_t p_start = pkt->air_start_us;
    uint64_t p_end = pkt->air_end_us;
    uint64_t preamble = radio_preamble_us(config);
    bool overlapped = false;

    /* Wanted-signal strength at this receiver. In link mode the packet already
     * carries the observed link's RSSI (sim_radio_broadcast read it out of the
     * link table); in position mode it is the path-loss gradient. */
    float our_rssi;
    if (config->link_mode) {
        our_rssi = (float)pkt->rssi;
    } else {
        float dxp = rx->x - pkt->tx_x;
        float dyp = rx->y - pkt->tx_y;
        our_rssi = path_rssi_dbm(config, sqrtf(dxp * dxp + dyp * dyp));
    }

    for (int i = 0; i < log->count; i++) {
        const channel_tx_t* e = &log->entries[i];
        if (e->start_us >= p_end || e->end_us <= p_start)
            continue; /* no time overlap */
        if (e->tx_addr == pkt->src_addr)
            continue; /* our own transmission (per-node TX is serialized) */
        if (e->tx_addr == rx->addr)
            return RADIO_RX_HALF_DUPLEX; /* receiver was transmitting */

        /* Interferer audible at the receiver? */
        if (!audible_from_log(config, e, rx))
            continue;

        overlapped = true;
        float their_rssi;
        if (config->link_mode) {
            their_rssi = (float)link_rssi_at(config, e->tx_index, rx->index);
        } else {
            float dx = rx->x - e->tx_x;
            float dy = rx->y - e->tx_y;
            their_rssi = path_rssi_dbm(config, sqrtf(dx * dx + dy * dy));
        }

        /* Capture effect: we survive an overlap only if we are at least
         * capture_db stronger AND we started first or within the interferer's
         * preamble (the receiver can still re-sync to the stronger packet). */
        if (our_rssi < their_rssi + config->capture_db)
            return RADIO_RX_COLLISION;
        if (p_start > e->start_us + preamble)
            return RADIO_RX_COLLISION;
    }

    return overlapped ? RADIO_RX_CAPTURED : RADIO_RX_OK;
}

/*
 * sim_radio_broadcast: transmit pkt from tx_node.
 *
 * Computes real time-on-air, serializes per-node transmissions (half-duplex),
 * records the occupancy window, and schedules an EVT_RECEIVE_PACKET event at
 * end-of-packet + propagation delay for each other active node in range.
 * The collision outcome is evaluated at delivery time (see
 * radio_check_reception), when the channel log is complete for the window.
 */
void sim_radio_broadcast(sim_node_t* tx_node, const outbound_packet_t* pkt, node_array_t* nodes,
                         radio_config_t* radio, pcg32_state_t* rng, event_queue_t* events,
                         metrics_state_t* metrics, uint64_t now_us) {
    sim_radio_broadcast_lbt(tx_node, pkt, nodes, radio, rng, events, metrics, now_us, false, NULL);
}

sim_tx_outcome_t sim_radio_broadcast_lbt(sim_node_t* tx_node, const outbound_packet_t* pkt,
                                         node_array_t* nodes, radio_config_t* radio,
                                         pcg32_state_t* rng, event_queue_t* events,
                                         metrics_state_t* metrics, uint64_t now_us,
                                         bool defer_on_busy, uint64_t* out_lbt_end_us) {
    uint32_t toa_us = radio_frame_airtime_us(radio, pkt->len);
    uint64_t air_start = now_us;
    if (radio->collisions_enabled && tx_node->tx_busy_until_us > air_start) {
        /* Half-duplex radio: a new TX waits for the in-progress one */
        air_start = tx_node->tx_busy_until_us;
    }

    /* Listen-before-talk, mirroring components/radio/tx_gate.c's LBT loop: up
     * to 3 CAD checks with randomized exponential backoff (50..300 ms base
     * plus an equal random component). `quiet` records whether a CAD actually
     * found the channel free, exactly like the firmware loop's variable of
     * the same name (tx_gate.c:158): a caller that defers needs to tell "the
     * channel went quiet" apart from "the attempts ran out while it was still
     * busy". */
    bool quiet = true;
    if (radio->collisions_enabled && radio->lbt_enabled) {
        quiet = false;
        for (int attempt = 0; attempt < SIM_LBT_MAX_ATTEMPTS; attempt++) {
            if (!channel_busy_at(radio, tx_node, air_start)) {
                quiet = true;
                break;
            }
            uint32_t backoff_ms = SIM_LBT_BACKOFF_BASE_MS << attempt;
            if (backoff_ms > SIM_LBT_BACKOFF_MAX_MS)
                backoff_ms = SIM_LBT_BACKOFF_MAX_MS;
            backoff_ms += pcg32_random(rng) % backoff_ms;
            air_start += (uint64_t)backoff_ms * 1000ULL;
            metrics->lbt_backoffs++;
        }
    }

    /* Attempts exhausted on a busy channel: deferring callers hand the
     * decision back up with nothing transmitted and no airtime charged
     * (tx_gate.c's TX_GATE_ERR_CHANNEL_BUSY return), every other caller
     * transmits anyway to avoid starvation, as this function always did. */
    if (!quiet && defer_on_busy) {
        if (out_lbt_end_us)
            *out_lbt_end_us = air_start;
        return SIM_TX_CHANNEL_BUSY;
    }

    uint64_t air_end = air_start + toa_us;
    tx_node->tx_busy_until_us = air_end;
    tx_node->airtime_tx_us += toa_us;
    metrics->airtime_total_us += toa_us;
    /* Per-type ToA: same toa_us the channel/collision model above
     * just used, charged once per actual (post-budget-gate) transmission,
     * since sim_radio_broadcast is the single chokepoint every TX site
     * (beacon, RREQ/RREP/RERR, DATA, receipts) converges through. */
    metrics_record_tx_airtime(metrics, pkt->pkt_type, toa_us);

    if (radio->collisions_enabled) {
        channel_log_add(&radio->channel, tx_node->addr, tx_node->index, tx_node->x, tx_node->y,
                        air_start, air_end, now_us);
    }

    /* Emit packet_sent for visualization */
    emit_packet_sent_typed(stdout, air_start, tx_node->id, tx_node->addr, pkt->dest_addr, pkt->len,
                           pkt->pkt_type);
    metrics_record_packet_sent(metrics);
    tx_node->packets_sent++;

    /* Record control packet metrics */
    if (pkt->pkt_type == PKT_TYPE_BEACON) {
        metrics_record_beacon_sent(metrics);
    } else if (pkt->pkt_type == PKT_TYPE_RREQ) {
        metrics_record_rreq_sent(metrics);
    } else if (pkt->pkt_type == PKT_TYPE_RREP) {
        metrics_record_rrep_sent(metrics);
    }

    /* Deliver to all nodes in range */
    for (int i = 0; i < nodes->count; i++) {
        sim_node_t* rx = &nodes->nodes[i];
        if (rx == tx_node || !rx->active)
            continue;

        /* For unicast, skip nodes that are not the target */
        if (!pkt->is_broadcast && pkt->dest_addr != rx->addr)
            continue;

        if (!radio_can_receive(radio, tx_node, rx, rng)) {
            /* Only emit drop for unicast targets and audible nodes
             * (for broadcast we silently skip nodes that cannot hear us) */
            if (!pkt->is_broadcast && pkt->dest_addr == rx->addr) {
                emit_packet_dropped(stdout, now_us, rx->id, "radio_loss");
                metrics_record_packet_dropped(metrics);
            } else if (pkt->is_broadcast && radio_audible(radio, tx_node, rx)) {
                /* Audible but dropped due to loss_pct or interference */
                metrics_record_packet_dropped(metrics);
            }
            continue;
        }

        /* Received signal quality, and how long the frame takes to arrive.
         * Link mode reads both out of the imported table and charges no
         * propagation delay: there is no distance to derive one from, and at
         * LoRa scales it is microseconds against hundreds of milliseconds of
         * time-on-air (the same reason the collision model ignores it). */
        int8_t rssi;
        int8_t snr;
        uint64_t delay;
        if (radio->link_mode) {
            rssi = link_rssi_at(radio, tx_node->index, rx->index);
            snr = radio->link_snr[tx_node->index][rx->index];
            delay = 0;
        } else {
            float dist = radio_distance(tx_node, rx);
            delay = radio_propagation_delay_us(radio, dist);
            rssi = radio_compute_rssi(radio, dist);

/* SNR = RSSI - noise_floor; noise_floor = -120 dBm (typical LoRa) */
#define NOISE_FLOOR_DBM (-120)
            int snr_raw = (int)rssi - NOISE_FLOOR_DBM;
            /* Add ±2 dB random jitter for realism */
            float jitter = (pcg32_float(rng) - 0.5f) * 4.0f; /* -2.0 to +2.0 */
            int snr_jittered = snr_raw + (int)jitter;
            if (snr_jittered < 0)
                snr_jittered = 0;
            if (snr_jittered > 127)
                snr_jittered = 127;
            snr = (int8_t)snr_jittered;
        }

        /* Schedule EVT_RECEIVE_PACKET at end-of-packet + propagation delay */
        sim_event_t recv_evt;
        memset(&recv_evt, 0, sizeof(recv_evt));
        recv_evt.type = EVT_RECEIVE_PACKET;
        recv_evt.timestamp_us = air_end + delay;
        recv_evt.data.packet.src_addr = tx_node->addr;
        recv_evt.data.packet.dest_addr = rx->addr;
        recv_evt.data.packet.rssi = rssi;
        recv_evt.data.packet.snr = snr;
        recv_evt.data.packet.len = pkt->len;
        recv_evt.data.packet.air_start_us = air_start;
        recv_evt.data.packet.air_end_us = air_end;
        recv_evt.data.packet.tx_x = tx_node->x;
        recv_evt.data.packet.tx_y = tx_node->y;
        memcpy(recv_evt.data.packet.data, pkt->data, pkt->len);

        event_queue_push(events, &recv_evt);
    }

    if (out_lbt_end_us)
        *out_lbt_end_us = air_start;
    return SIM_TX_SENT;
}
