#include "sim_emitter.h"
#include "../../components/packet/include/packet.h"

/* Map PKT_TYPE_* constants to human-readable strings */
static const char *pkt_type_name(uint8_t t) {
    switch (t) {
        case PKT_TYPE_RREQ:   return "RREQ";
        case PKT_TYPE_RREP:   return "RREP";
        case PKT_TYPE_RERR:   return "RERR";
        case PKT_TYPE_BEACON: return "BEACON";
        case PKT_TYPE_DATA:   return "DATA";
        default:              return "UNKNOWN";
    }
}

void emit_packet_sent(FILE *out, uint64_t timestamp_us, const char *node_id, uint32_t dest_addr, uint16_t size) {
    fprintf(out, "{\"type\":\"packet_sent\",\"timestamp_us\":%llu,\"node\":\"%s\",\"dest_addr\":\"0x%08X\",\"size\":%u}\n",
            (unsigned long long)timestamp_us, node_id, dest_addr, size);
    fflush(out);
}

void emit_packet_received(FILE *out, uint64_t timestamp_us, const char *node_id, uint32_t src_addr, uint16_t size) {
    fprintf(out, "{\"type\":\"packet_received\",\"timestamp_us\":%llu,\"node\":\"%s\",\"src_addr\":\"0x%08X\",\"size\":%u}\n",
            (unsigned long long)timestamp_us, node_id, src_addr, size);
    fflush(out);
}

void emit_packet_dropped(FILE *out, uint64_t timestamp_us, const char *node_id, const char *reason) {
    fprintf(out, "{\"type\":\"packet_dropped\",\"timestamp_us\":%llu,\"node\":\"%s\",\"reason\":\"%s\"}\n",
            (unsigned long long)timestamp_us, node_id, reason);
    fflush(out);
}

void emit_route_added(FILE *out, uint64_t timestamp_us, const char *node_id, uint32_t dest_addr, uint32_t next_hop, uint8_t hop_count) {
    fprintf(out, "{\"type\":\"route_added\",\"timestamp_us\":%llu,\"node\":\"%s\",\"dest_addr\":\"0x%08X\",\"next_hop\":\"0x%08X\",\"hop_count\":%u}\n",
            (unsigned long long)timestamp_us, node_id, dest_addr, next_hop, hop_count);
    fflush(out);
}

void emit_route_removed(FILE *out, uint64_t timestamp_us, const char *node_id, uint32_t dest_addr) {
    fprintf(out, "{\"type\":\"route_removed\",\"timestamp_us\":%llu,\"node\":\"%s\",\"dest_addr\":\"0x%08X\"}\n",
            (unsigned long long)timestamp_us, node_id, dest_addr);
    fflush(out);
}

void emit_node_moved(FILE *out, uint64_t timestamp_us, const char *node_id, float x, float y) {
    fprintf(out, "{\"type\":\"node_moved\",\"timestamp_us\":%llu,\"node\":\"%s\",\"x\":%.2f,\"y\":%.2f}\n",
            (unsigned long long)timestamp_us, node_id, x, y);
    fflush(out);
}

void emit_node_joined(FILE *out, uint64_t timestamp_us, const char *node_id, uint32_t addr, float x, float y) {
    fprintf(out, "{\"type\":\"node_joined\",\"timestamp_us\":%llu,\"node\":\"%s\",\"addr\":\"0x%08X\",\"x\":%.2f,\"y\":%.2f}\n",
            (unsigned long long)timestamp_us, node_id, addr, x, y);
    fflush(out);
}

void emit_node_left(FILE *out, uint64_t timestamp_us, const char *node_id) {
    fprintf(out, "{\"type\":\"node_left\",\"timestamp_us\":%llu,\"node\":\"%s\"}\n",
            (unsigned long long)timestamp_us, node_id);
    fflush(out);
}

void emit_link_broken(FILE *out, uint64_t timestamp_us, const char *node_id, uint32_t peer_addr) {
    fprintf(out, "{\"type\":\"link_broken\",\"timestamp_us\":%llu,\"node\":\"%s\",\"peer_addr\":\"0x%08X\"}\n",
            (unsigned long long)timestamp_us, node_id, peer_addr);
    fflush(out);
}

void emit_metrics(FILE *out, uint64_t timestamp_us, int active_nodes, uint64_t total_packets, uint64_t delivered, uint64_t dropped, double avg_latency_ms) {
    fprintf(out, "{\"type\":\"metrics\",\"timestamp_us\":%llu,\"active_nodes\":%d,\"total_packets\":%llu,\"delivered\":%llu,\"dropped\":%llu,\"avg_latency_ms\":%.3f}\n",
            (unsigned long long)timestamp_us, active_nodes, (unsigned long long)total_packets, 
            (unsigned long long)delivered, (unsigned long long)dropped, avg_latency_ms);
    fflush(out);
}

void emit_anomaly(FILE *out, uint64_t timestamp_us, const char *type, const char *node_id, uint32_t dest_addr, const char *details) {
    fprintf(out, "{\"type\":\"anomaly\",\"timestamp_us\":%llu,\"anomaly_type\":\"%s\",\"node\":\"%s\",\"dest_addr\":\"0x%08X\",\"details\":\"%s\"}\n",
            (unsigned long long)timestamp_us, type, node_id, dest_addr, details);
    fflush(out);
}

void emit_packet_sent_typed(FILE *out, uint64_t timestamp_us,
    const char *node_id, uint32_t src_addr, uint32_t dest_addr,
    uint16_t size, uint8_t pkt_type)
{
    fprintf(out,
        "{\"type\":\"packet_sent\",\"timestamp_us\":%llu"
        ",\"node\":\"%s\",\"src\":\"0x%08X\",\"dest\":\"0x%08X\""
        ",\"pkt_type\":\"%s\",\"size\":%u}\n",
        (unsigned long long)timestamp_us, node_id,
        src_addr, dest_addr, pkt_type_name(pkt_type), size);
    fflush(out);
}

void emit_packet_received_typed(FILE *out, uint64_t timestamp_us,
    const char *node_id, uint32_t src_addr, int8_t rssi,
    uint16_t size, uint8_t pkt_type)
{
    fprintf(out,
        "{\"type\":\"packet_received\",\"timestamp_us\":%llu"
        ",\"node\":\"%s\",\"src\":\"0x%08X\""
        ",\"pkt_type\":\"%s\",\"rssi\":%d,\"size\":%u}\n",
        (unsigned long long)timestamp_us, node_id,
        src_addr, pkt_type_name(pkt_type), (int)rssi, size);
    fflush(out);
}
