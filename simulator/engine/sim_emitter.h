#ifndef SIM_EMITTER_H
#define SIM_EMITTER_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

/* JSON event emitters - each writes a single JSON line and flushes */

/* Suppress all emit_* output (used by unit tests driving the radio model) */
void sim_emitter_set_quiet(bool quiet);

void emit_packet_dropped(FILE* out, uint64_t timestamp_us, const char* node_id, const char* reason);
void emit_route_added(FILE* out, uint64_t timestamp_us, const char* node_id, uint32_t dest_addr,
                      uint32_t next_hop, uint8_t hop_count);
void emit_link_broken(FILE* out, uint64_t timestamp_us, const char* node_id, uint32_t peer_addr);
void emit_anomaly(FILE* out, uint64_t timestamp_us, const char* type, const char* node_id,
                  uint32_t dest_addr, const char* details);

/*
 * Typed variants: include pkt_type string ("BEACON","RREQ","RREP","RERR","DATA").
 */
void emit_packet_sent_typed(FILE* out, uint64_t timestamp_us, const char* node_id,
                            uint32_t src_addr, uint32_t dest_addr, uint16_t size, uint8_t pkt_type);

void emit_packet_received_typed(FILE* out, uint64_t timestamp_us, const char* node_id,
                                uint32_t src_addr, int8_t rssi, int8_t snr, uint16_t size,
                                uint8_t pkt_type);

#endif /* SIM_EMITTER_H */
