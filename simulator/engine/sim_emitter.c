#include "sim_emitter.h"
#include "../../components/packet/include/packet.h"

static bool g_emitter_quiet = false;

void sim_emitter_set_quiet(bool quiet) { g_emitter_quiet = quiet; }

#define EMIT_QUIET_GUARD()                                                                         \
    do {                                                                                           \
        if (g_emitter_quiet)                                                                       \
            return;                                                                                \
    } while (0)

/* Map PKT_TYPE_* constants to human-readable strings */
static const char* pkt_type_name(uint8_t t) {
    switch (t) {
    case PKT_TYPE_RREQ:
        return "RREQ";
    case PKT_TYPE_RREP:
        return "RREP";
    case PKT_TYPE_RERR:
        return "RERR";
    case PKT_TYPE_BEACON:
        return "BEACON";
    case PKT_TYPE_DATA:
        return "DATA";
    case PKT_TYPE_ACK:
        /* Phase 2 Task 0: also the flood-comparison baseline's flooded ACK
         * (gosim/flood.go), not just a firmware ACK use; kept as one label
         * since both are genuinely PKT_TYPE_ACK on the wire. */
        return "ACK";
    case PKT_TYPE_DELIVERY_RECEIPT:
        /* Flooding F1 Task 2: the confirmation receipt is now a first-class
         * flooded packet (broadcast under flood transport, unicast-routed
         * under reactive), so label it instead of leaving it "UNKNOWN" and let
         * scenarios distinguish a flooded receipt (dest 0xFFFFFFFF) from a
         * routed one. */
        return "DELIVERY_RECEIPT";
    default:
        return "UNKNOWN";
    }
}

void emit_packet_dropped(FILE* out, uint64_t timestamp_us, const char* node_id,
                         const char* reason) {
    EMIT_QUIET_GUARD();
    fprintf(
        out,
        "{\"type\":\"packet_dropped\",\"timestamp_us\":%llu,\"node\":\"%s\",\"reason\":\"%s\"}\n",
        (unsigned long long)timestamp_us, node_id, reason);
    fflush(out);
}

void emit_route_added(FILE* out, uint64_t timestamp_us, const char* node_id, uint32_t dest_addr,
                      uint32_t next_hop, uint8_t hop_count) {
    EMIT_QUIET_GUARD();
    fprintf(out,
            "{\"type\":\"route_added\",\"timestamp_us\":%llu,\"node\":\"%s\",\"dest_addr\":\"0x%"
            "08X\",\"next_hop\":\"0x%08X\",\"hop_count\":%u}\n",
            (unsigned long long)timestamp_us, node_id, dest_addr, next_hop, hop_count);
    fflush(out);
}

void emit_link_broken(FILE* out, uint64_t timestamp_us, const char* node_id, uint32_t peer_addr) {
    EMIT_QUIET_GUARD();
    fprintf(out,
            "{\"type\":\"link_broken\",\"timestamp_us\":%llu,\"node\":\"%s\",\"peer_addr\":\"0x%"
            "08X\"}\n",
            (unsigned long long)timestamp_us, node_id, peer_addr);
    fflush(out);
}

void emit_anomaly(FILE* out, uint64_t timestamp_us, const char* type, const char* node_id,
                  uint32_t dest_addr, const char* details) {
    EMIT_QUIET_GUARD();
    fprintf(out,
            "{\"type\":\"anomaly\",\"timestamp_us\":%llu,\"anomaly_type\":\"%s\",\"node\":\"%s\","
            "\"dest_addr\":\"0x%08X\",\"details\":\"%s\"}\n",
            (unsigned long long)timestamp_us, type, node_id, dest_addr, details);
    fflush(out);
}

void emit_packet_sent_typed(FILE* out, uint64_t timestamp_us, const char* node_id,
                            uint32_t src_addr, uint32_t dest_addr, uint16_t size,
                            uint8_t pkt_type) {
    EMIT_QUIET_GUARD();
    fprintf(out,
            "{\"type\":\"packet_sent\",\"timestamp_us\":%llu"
            ",\"node\":\"%s\",\"src\":\"0x%08X\",\"dest\":\"0x%08X\""
            ",\"pkt_type\":\"%s\",\"size\":%u}\n",
            (unsigned long long)timestamp_us, node_id, src_addr, dest_addr, pkt_type_name(pkt_type),
            size);
    fflush(out);
}

void emit_packet_received_typed(FILE* out, uint64_t timestamp_us, const char* node_id,
                                uint32_t src_addr, int8_t rssi, int8_t snr, uint16_t size,
                                uint8_t pkt_type) {
    EMIT_QUIET_GUARD();
    fprintf(out,
            "{\"type\":\"packet_received\",\"timestamp_us\":%llu"
            ",\"node\":\"%s\",\"src\":\"0x%08X\""
            ",\"pkt_type\":\"%s\",\"rssi\":%d,\"snr\":%d,\"size\":%u}\n",
            (unsigned long long)timestamp_us, node_id, src_addr, pkt_type_name(pkt_type), (int)rssi,
            (int)snr, size);
    fflush(out);
}
