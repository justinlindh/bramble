#include "include/discovery.h"
#include "network_key.h"
#include <string.h>

void discovery_init(pending_discovery_table_t* table) { memset(table, 0, sizeof(*table)); }

int discovery_start(pending_discovery_table_t* table, uint32_t dest_addr, uint32_t query_id,
                    uint32_t now_ms) {
    if (table->count >= MAX_PENDING_DISCOVERIES)
        return -1;
    pending_discovery_t* e = &table->entries[table->count++];
    memset(e, 0, sizeof(*e));
    e->dest_addr = dest_addr;
    e->query_ids[0] = query_id;
    e->timestamp = now_ms;
    e->attempts = 1;
    return 0;
}

pending_discovery_t* discovery_lookup(pending_discovery_table_t* table, uint32_t dest_addr) {
    for (int i = 0; i < table->count; i++)
        if (table->entries[i].dest_addr == dest_addr)
            return &table->entries[i];
    return NULL;
}

pending_discovery_t* discovery_lookup_by_query(pending_discovery_table_t* table,
                                               uint32_t query_id) {
    for (int i = 0; i < table->count; i++) {
        pending_discovery_t* e = &table->entries[i];
        uint8_t n = (e->attempts < MAX_RREQ_ATTEMPTS) ? e->attempts : MAX_RREQ_ATTEMPTS;
        for (uint8_t a = 0; a < n; a++)
            if (e->query_ids[a] == query_id)
                return e;
    }
    return NULL;
}

void discovery_remove(pending_discovery_table_t* table, uint32_t dest_addr) {
    for (int i = 0; i < table->count; i++) {
        if (table->entries[i].dest_addr == dest_addr) {
            table->entries[i] = table->entries[--table->count];
            return;
        }
    }
}

bool discovery_should_retry(const pending_discovery_t* d, uint32_t now_ms) {
    if (d->attempts >= MAX_RREQ_ATTEMPTS)
        return false;
    uint32_t interval = (d->attempts <= 1) ? RREQ_RETRY_INTERVAL_1_MS : RREQ_RETRY_INTERVAL_2_MS;
    return (now_ms - d->timestamp) >= interval;
}

void discovery_record_attempt(pending_discovery_t* d, uint32_t query_id, uint32_t now_ms) {
    if (d->attempts < MAX_RREQ_ATTEMPTS)
        d->query_ids[d->attempts] = query_id;
    d->attempts++;
    d->timestamp = now_ms;
}

uint32_t discovery_current_query_id(const pending_discovery_t* d) {
    uint8_t idx = (d->attempts > 0) ? (uint8_t)(d->attempts - 1) : 0;
    if (idx >= MAX_RREQ_ATTEMPTS)
        idx = MAX_RREQ_ATTEMPTS - 1;
    return d->query_ids[idx];
}

uint8_t discovery_hop_limit_for_attempt(uint8_t attempt) {
    return (attempt <= 1) ? RREQ_HOP_LIMIT_INITIAL : RREQ_HOP_LIMIT_EXPANDED;
}

uint32_t discovery_forward_jitter_ms(uint32_t random_value) {
    return RREQ_FWD_JITTER_MIN_MS +
           (random_value % (RREQ_FWD_JITTER_MAX_MS - RREQ_FWD_JITTER_MIN_MS + 1u));
}

bramble_rreq_t rreq_build_originator(uint32_t my_addr, uint32_t dest_addr, uint32_t query_id,
                                     uint32_t encrypted_source, uint8_t hop_limit) {
    bramble_rreq_t r;
    memset(&r, 0, sizeof(r));
    r.header.version = BRAMBLE_VERSION;
    r.header.type = PKT_TYPE_RREQ;
    r.header.flags = 0;
    r.header.hop_limit = hop_limit;
    r.header.dest_addr = dest_addr;
    r.header.packet_id = query_id;
    r.query_id = query_id;
    r.encrypted_source = encrypted_source;
    r.hop_count = 0;
    r.metric = 255;
    r.prev_hop = my_addr;
    return r;
}

bramble_rreq_t rreq_forward(const bramble_rreq_t* incoming, uint32_t my_addr, int8_t rx_rssi,
                            int8_t rx_snr) {
    bramble_rreq_t r = *incoming;
    r.hop_count++;
    r.metric = metric_apply_link_penalty(r.metric, rx_rssi, rx_snr);
    r.header.hop_limit--;
    r.prev_hop = my_addr;
    return r;
}

/*
 * Constant-time equality (tag comparison), same OR-accumulate pattern used
 * elsewhere in this codebase for MAC/tag checks (e.g. dm_session.c's
 * ct_eq): no early exit, so comparing a computed MAC against an
 * attacker-supplied one never leaks how many leading bytes matched.
 */
static int ct_eq(const uint8_t* a, const uint8_t* b, size_t n) {
    uint8_t acc = 0;
    for (size_t i = 0; i < n; i++)
        acc |= a[i] ^ b[i];
    return acc == 0;
}

/* query_id(4) || src_addr(4) || hop_count(1) || route_metric(1) || seq(6),
 * big-endian for the multi-byte fields: the origin-stable fields, exactly
 * excluding next_hop and header.dest_addr (the only two fields rrep_forward
 * mutates). seq (ws 1.3b) is origin-stable like the rest: rrep_forward
 * carries it through unchanged, so it belongs in the same coverage set. */
static void rrep_build_auth_buf(const bramble_rrep_t* r, uint8_t buf[16]) {
    buf[0] = (uint8_t)(r->query_id >> 24);
    buf[1] = (uint8_t)(r->query_id >> 16);
    buf[2] = (uint8_t)(r->query_id >> 8);
    buf[3] = (uint8_t)r->query_id;
    buf[4] = (uint8_t)(r->src_addr >> 24);
    buf[5] = (uint8_t)(r->src_addr >> 16);
    buf[6] = (uint8_t)(r->src_addr >> 8);
    buf[7] = (uint8_t)r->src_addr;
    buf[8] = r->hop_count;
    buf[9] = r->route_metric;
    memcpy(buf + 10, r->seq, 6);
}

void rrep_sign(bramble_rrep_t* r) {
    uint8_t buf[16];
    rrep_build_auth_buf(r, buf);
    network_key_mac("bramble-rrep-v2", buf, sizeof(buf), r->auth_hmac);
}

int rrep_verify(const bramble_rrep_t* r) {
    uint8_t buf[16];
    rrep_build_auth_buf(r, buf);
    uint8_t expect[8];
    network_key_mac("bramble-rrep-v2", buf, sizeof(buf), expect);
    return ct_eq(expect, r->auth_hmac, sizeof(expect));
}

bramble_rrep_t rrep_build_destination(const bramble_rreq_t* rreq, uint32_t my_addr) {
    bramble_rrep_t r;
    memset(&r, 0, sizeof(r));
    r.header.version = BRAMBLE_VERSION;
    r.header.type = PKT_TYPE_RREP;
    r.header.flags = 0;
    r.header.hop_limit = ROUTE_HOP_LIMIT_MAX;
    r.header.dest_addr = rreq->prev_hop; /* unicast back toward originator */
    r.header.packet_id = rreq->query_id;
    r.query_id = rreq->query_id;
    r.src_addr = my_addr;
    r.next_hop = rreq->prev_hop;
    r.hop_count = rreq->hop_count + 1;
    r.route_metric = rreq->metric;
    rrep_sign(&r);
    return r;
}

bramble_rrep_t rrep_forward(const bramble_rrep_t* incoming, uint32_t next_hop_back) {
    /* Copies the whole struct, including auth_hmac, unchanged: only
     * next_hop and header.dest_addr are relay-mutable, exactly the two
     * fields rrep_sign/rrep_verify exclude from the MAC. */
    bramble_rrep_t r = *incoming;
    r.next_hop = next_hop_back;
    r.header.dest_addr = next_hop_back;
    return r;
}

rrep_rx_decision_t rrep_rx_decide(const bramble_rrep_t* rrep, uint32_t self_addr,
                                  uint8_t link_metric, pending_discovery_table_t* pd,
                                  reverse_route_table_t* rev) {
    rrep_rx_decision_t d;
    memset(&d, 0, sizeof(d));

    /* Behavior-preserving extraction of the current handle_rrep logic:
     * install runs unconditionally, and next_hop keeps the pre-fix ternary
     * (correct only when self_addr is a direct neighbor of the source). */
    d.install_route = true;
    d.route_dest = rrep->src_addr;
    d.route_next_hop = (rrep->header.dest_addr == self_addr) ? rrep->src_addr : rrep->next_hop;
    d.route_hops = rrep->hop_count;
    d.route_metric = link_metric;

    pending_discovery_t* pd_entry = discovery_lookup_by_query(pd, rrep->query_id);
    if (pd_entry) {
        d.action = RREP_RX_DELIVER;
        d.deliver_dest = pd_entry->dest_addr;
        return d;
    }

    reverse_route_t* rev_entry = reverse_route_lookup(rev, rrep->query_id);
    if (rev_entry) {
        d.action = RREP_RX_FORWARD;
        d.forward_to = rev_entry->prev_hop;
        return d;
    }

    d.action = RREP_RX_DROP;
    return d;
}
