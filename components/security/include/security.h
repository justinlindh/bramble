#ifndef BRAMBLE_SECURITY_H
#define BRAMBLE_SECURITY_H
#include <stdint.h>
#include <stdbool.h>

#define RREQ_RATE_LIMIT_MS 30000
#define RREQ_RATE_ENTRIES 64
#define SYBIL_RSSI_CLUSTER_THRESHOLD 3
#define SYBIL_MIN_SUSPECTS 3
#define RREQ_FWD_BURST 16
#define RREQ_FWD_REFILL_MS 2000

typedef struct {
    uint32_t neighbor_addr;
    uint32_t dest_addr;
    uint32_t last_rreq_ms;
} rreq_rate_entry_t;

typedef struct {
    rreq_rate_entry_t entries[RREQ_RATE_ENTRIES];
    int count;
} rreq_rate_limiter_t;

void rreq_rate_init(rreq_rate_limiter_t* rl);
bool rreq_rate_allow(rreq_rate_limiter_t* rl, uint32_t neighbor, uint32_t dest, uint32_t now_ms);
bool sybil_check_rssi_cluster(const int8_t* rssi_values, int count);

// Global forwarded-RREQ token bucket. Bounds this node's aggregate forwarded-RREQ
// rate regardless of the unauthenticated, spoofable rreq.prev_hop field (SEC-M4).
// Not keyed per-neighbor: see design doc for why per-neighbor keying is evadable
// and introduces a targeted framing DoS.
typedef struct {
    uint32_t tokens;
    uint32_t last_refill_ms;
} rreq_fwd_limiter_t;

void rreq_fwd_init(rreq_fwd_limiter_t* rl, uint32_t now_ms);
bool rreq_fwd_allow(rreq_fwd_limiter_t* rl, uint32_t now_ms);

/*
 * PROBE ingress backpressure (issue #75).
 *
 * PROBE is UNAUTHENTICATED BY DESIGN and stays that way: an unprovisioned
 * node asking "who can hear me" is the entire feature, so there is no MAC
 * check and no provisioning gate on the receive path. What was broken is the
 * AMPLIFICATION. `handle_probe` did one length check and nothing else, and
 * each accepted probe bought a three-send PROBE_ACK burst plus a
 * rebroadcast. Dedup keys on a `packet_id` an attacker varies freely, so one
 * injected 16-byte frame cost every node in earshot four transmissions, with
 * no ceiling and no cost to the attacker. These buckets put a ceiling on it.
 *
 * Two GLOBAL token buckets, deliberately not keyed per-sender:
 *
 *   reply   - gates answering a probe at all. The node-wide ceiling on
 *             probe-induced transmission.
 *   forward - gates rebroadcasting it, and is tighter. Answering is a
 *             bounded local cost; FORWARDING is the term that multiplies
 *             across the mesh. Running the forward bucket dry first means a
 *             probe flood stops propagating while the node keeps answering
 *             its own neighbors, so PROBE stays useful as a reachability
 *             tool under exactly the pressure that used to weaponize it.
 *
 * WHY NOT PER-SENDER: this follows SEC-M4's forwarded-RREQ decision
 * (`rreq_fwd_allow` above) for the same reasons, and the reasoning is worth
 * repeating because "per-sender token bucket" is the intuitive fix and it is
 * the wrong one here. The only sender signal available at PROBE RX is the
 * `src_addr` in the payload, an unauthenticated wire field. Keying a bucket
 * on it buys nothing and costs something:
 *   - It is EVADABLE. An attacker rotates `src_addr` across fabricated
 *     values and spreads the flood across as many per-sender buckets as the
 *     table has slots, so the aggregate rate is unchanged and a global cap
 *     is still needed to bound it. The per-sender bucket adds no ceiling the
 *     global one did not already provide.
 *   - It CREATES A TARGETED DoS THAT DOES NOT OTHERWISE EXIST. An attacker
 *     sets `src_addr` to a victim's address, drains the victim's bucket, and
 *     the victim's real probes are then dropped by every honest node in
 *     earshot. That is a new, cheap, targeted denial of service introduced
 *     by the mitigation itself, and it frames the victim as the flooder.
 * A global cap has neither property. Under a flood it refuses probes without
 * regard to who claims to have sent them, which is uniform rather than
 * targetable, and it self-heals on refill with no operator action. The cost
 * is that a flood also delays legitimate probes; that is an accepted
 * airtime-vs-reach tradeoff, identical to the one SEC-M4 already documents,
 * and probing is a manual, retryable diagnostic. Genuine per-sender fairness
 * requires authenticating PROBE, which would delete the
 * unprovisioned-reachability use case PROBE exists for.
 *
 * Budgets: PROBE_REPLY_BURST (8) tokens refilling one per
 * PROBE_REPLY_REFILL_MS (5000ms), so roughly 12 answered probes a minute
 * plus a burst of 8, which at the worst-case 3-send reply burst is about 36
 * frames a minute before `tx_gate`'s TX_KIND_PROBE lane gets its own,
 * final say. PROBE_FWD_BURST (4) tokens refilling one per
 * PROBE_FWD_REFILL_MS (10000ms) for the amplifying path.
 */
#define PROBE_REPLY_BURST 8
#define PROBE_REPLY_REFILL_MS 5000
#define PROBE_FWD_BURST 4
#define PROBE_FWD_REFILL_MS 10000

typedef struct {
    uint32_t tokens;
    uint32_t last_refill_ms;
} probe_bucket_t;

typedef struct {
    probe_bucket_t reply;
    probe_bucket_t forward;

    /* Observability. Backpressure that absorbs a flood silently is
     * indistinguishable in the field from a broken radio, so the refusals
     * are counted and surfaced through bramble.getDiagnostics. */
    uint32_t accepted;
    uint32_t dropped_reply;
    uint32_t dropped_forward;
} probe_ingress_limiter_t;

typedef struct {
    bool reply;   /* answer this probe (queue the PROBE_ACK burst) */
    bool forward; /* rebroadcast it onward; never true unless reply is */
} probe_ingress_decision_t;

void probe_ingress_init(probe_ingress_limiter_t* rl, uint32_t now_ms);

/*
 * Charge one received PROBE and decide what this node may do with it.
 * Mutates rl: this is the token spend, not a peek. A probe that is not
 * answered is never forwarded, because forwarding a probe this node has
 * already decided is beyond its budget only spends the budget elsewhere.
 *
 * forward_eligible is the caller's answer to "could this probe be forwarded
 * at all, budget aside", which in practice is `hop_limit > 1`. It is an
 * explicit input, mirroring how channel_flood_decide takes budget_permits,
 * so the limiter never has to infer context it cannot see. When it is false
 * the forward bucket is not touched AT ALL: not consumed, and not counted as
 * a forward drop. Charging hop-exhausted probes would quietly spend the
 * scarcer budget on frames that could never propagate, and since probes
 * originate at a hop limit of 8, every legitimate sweep ends with such
 * arrivals at the edge of range. That is ordinary traffic. Letting it drain
 * the forward bucket would suppress forwarding for genuinely eligible
 * multi-hop probes sooner than intended, which is a version of the
 * "ordinary traffic degrades service for others" failure the global-bucket
 * choice exists to avoid, and it would make dropped_forward rise from
 * harmless last-hop receptions rather than from real congestion.
 *
 * Takes no source address ON PURPOSE. The parameter would be an
 * unauthenticated wire field and accepting it would invite exactly the
 * per-sender keying rejected above.
 */
probe_ingress_decision_t probe_ingress_allow(probe_ingress_limiter_t* rl, bool forward_eligible,
                                             uint32_t now_ms);
#endif
