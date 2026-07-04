#ifndef PROBE_RESULTS_H
#define PROBE_RESULTS_H

#include <stdint.h>

#define MAX_PROBE_RESULTS 16

typedef struct {
    uint32_t addr;
    uint8_t hops;
    int16_t rssi;
    int8_t snr;
    uint32_t latency_ms;
    uint8_t seen_round_mask;
} probe_result_t;

/* Upsert one probe-ACK observation keyed by responder addr: latest hops and
 * latency, best (max) rssi/snr, OR the round bit. New responders append while
 * count < max_results; overflow inserts are dropped. Pure; identical to the
 * pre-extraction upsert in mesh_task.c handle_probe_ack. probe_round is
 * assumed already clamped to [1, PROBE_SWEEP_ROUNDS] by the caller (the live
 * clamp at mesh_task.c is `probe_round < 1 || probe_round > PROBE_SWEEP_ROUNDS`).
 * If PROBE_SWEEP_ROUNDS ever exceeds 8, `1u << (probe_round - 1)` into the
 * uint8_t seen_round_mask overflows; that is pre-existing and preserved
 * verbatim. */
void probe_results_upsert(probe_result_t* results, int* count, int max_results, uint32_t resp_addr,
                          uint8_t hops, int16_t rssi, int8_t snr, uint32_t latency_ms,
                          uint8_t probe_round);

#endif /* PROBE_RESULTS_H */
