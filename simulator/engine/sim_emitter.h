#ifndef SIM_EMITTER_H
#define SIM_EMITTER_H

#include <stdio.h>
#include <stdint.h>

/* JSON event emitters - each writes a single JSON line and flushes */

void emit_packet_sent(FILE *out, uint64_t timestamp_us, const char *node_id, uint32_t dest_addr, uint16_t size);
void emit_packet_received(FILE *out, uint64_t timestamp_us, const char *node_id, uint32_t src_addr, uint16_t size);
void emit_packet_dropped(FILE *out, uint64_t timestamp_us, const char *node_id, const char *reason);
void emit_route_added(FILE *out, uint64_t timestamp_us, const char *node_id, uint32_t dest_addr, uint32_t next_hop, uint8_t hop_count);
void emit_route_removed(FILE *out, uint64_t timestamp_us, const char *node_id, uint32_t dest_addr);
void emit_node_moved(FILE *out, uint64_t timestamp_us, const char *node_id, float x, float y);
void emit_node_joined(FILE *out, uint64_t timestamp_us, const char *node_id, uint32_t addr);
void emit_node_left(FILE *out, uint64_t timestamp_us, const char *node_id);
void emit_link_broken(FILE *out, uint64_t timestamp_us, const char *node_id, uint32_t peer_addr);
void emit_metrics(FILE *out, uint64_t timestamp_us, int active_nodes, uint64_t total_packets, uint64_t delivered, uint64_t dropped, double avg_latency_ms);
void emit_anomaly(FILE *out, uint64_t timestamp_us, const char *type, const char *node_id, uint32_t dest_addr, const char *details);

#endif /* SIM_EMITTER_H */
