#ifndef SIM_ANOMALY_H
#define SIM_ANOMALY_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#define MAX_ROUTE_FLAP_TRACK 64
#define ROUTE_FLAP_WINDOW_US 2000000  /* 2 seconds */
#define ROUTE_FLAP_THRESHOLD 5

typedef struct {
    uint32_t dest_addr;
    uint32_t next_hop;
    uint64_t timestamp_us;
} route_change_t;

typedef struct {
    route_change_t changes[MAX_ROUTE_FLAP_TRACK];
    int count;
} route_flap_tracker_t;

void anomaly_init(route_flap_tracker_t *tracker);
bool anomaly_check_route_flap(route_flap_tracker_t *tracker, uint32_t dest_addr, uint32_t next_hop, uint64_t now_us, FILE *emit_out, const char *node_id);

#endif /* SIM_ANOMALY_H */
