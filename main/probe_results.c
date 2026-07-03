#include "probe_results.h"

void probe_results_upsert(probe_result_t *results, int *count, int max_results,
                          uint32_t resp_addr, uint8_t hops, int16_t rssi, int8_t snr,
                          uint32_t latency_ms, uint8_t probe_round) {
    int idx = -1;
    for (int i = 0; i < *count; i++) {
        if (results[i].addr == resp_addr) {
            idx = i;
            break;
        }
    }

    if (idx >= 0) {
        probe_result_t *r = &results[idx];
        r->hops = hops;
        r->latency_ms = latency_ms; /* latest latency */
        if (rssi > r->rssi) r->rssi = rssi; /* best RSSI */
        if (snr > r->snr) r->snr = snr;      /* best SNR */
        r->seen_round_mask |= (uint8_t)(1u << (probe_round - 1));
    } else if (*count < max_results) {
        probe_result_t *r = &results[(*count)++];
        r->addr = resp_addr;
        r->hops = hops;
        r->rssi = rssi;
        r->snr = snr;
        r->latency_ms = latency_ms;
        r->seen_round_mask = (uint8_t)(1u << (probe_round - 1));
    }
}
