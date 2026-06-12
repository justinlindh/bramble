#include "unity.h"
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "traffic_debug.h"

/* Module API from traffic_debug.h */
/* Packet types from packet.h */
#define PKT_TYPE_ACK              0x01
#define PKT_TYPE_RREQ             0x02
#define PKT_TYPE_RREP             0x03
#define PKT_TYPE_RERR             0x04
#define PKT_TYPE_BEACON           0x05
#define PKT_TYPE_KEY_EXCHANGE     0x06
#define PKT_TYPE_DELIVERY_RECEIPT 0x07
#define PKT_TYPE_DATA             0x0A
#define PKT_TYPE_STORE_REQUEST    0x0B
#define PKT_TYPE_STORE_ACK        0x0C
#define PKT_TYPE_MAILBOX_DELIVERY 0x0D
#define PKT_TYPE_MAILBOX_QUERY    0x0E
#define PKT_TYPE_EMERGENCY        0x0F
#define PKT_TYPE_EMERGENCY_CANCEL 0x10
#define PKT_TYPE_CODED            0x11
#define PKT_TYPE_PROBE            0x12
#define PKT_TYPE_PROBE_ACK        0x13
#define PKT_TYPE_LOCATION         0x14

/* Airtime tiers from airtime_budget.h */
#define AIRTIME_TIER_NORMAL    0x01
#define AIRTIME_TIER_CRITICAL  0x02
#define AIRTIME_TIER_BROADCAST 0x03

static traffic_debug_t td;
static traffic_event_t events[32];

void setUp(void) {
    memset(&td, 0, sizeof(td));
    memset(events, 0, sizeof(events));
}

void tearDown(void) {}

/* ── Packet Type → Category Mapping ────────────────────────────────── */

void test_classify_beacon(void) {
    TEST_ASSERT_EQUAL(TRAFFIC_CAT_BEACON, 
                     traffic_debug_classify_packet(PKT_TYPE_BEACON));
}

void test_classify_routing_packets(void) {
    TEST_ASSERT_EQUAL(TRAFFIC_CAT_ROUTING,
                     traffic_debug_classify_packet(PKT_TYPE_RREQ));
    TEST_ASSERT_EQUAL(TRAFFIC_CAT_ROUTING,
                     traffic_debug_classify_packet(PKT_TYPE_RREP));
    TEST_ASSERT_EQUAL(TRAFFIC_CAT_ROUTING,
                     traffic_debug_classify_packet(PKT_TYPE_RERR));
    TEST_ASSERT_EQUAL(TRAFFIC_CAT_ROUTING,
                     traffic_debug_classify_packet(PKT_TYPE_PROBE));
    TEST_ASSERT_EQUAL(TRAFFIC_CAT_ROUTING,
                     traffic_debug_classify_packet(PKT_TYPE_PROBE_ACK));
}

void test_classify_ack_packets(void) {
    TEST_ASSERT_EQUAL(TRAFFIC_CAT_ACK,
                     traffic_debug_classify_packet(PKT_TYPE_ACK));
    TEST_ASSERT_EQUAL(TRAFFIC_CAT_ACK,
                     traffic_debug_classify_packet(PKT_TYPE_DELIVERY_RECEIPT));
    TEST_ASSERT_EQUAL(TRAFFIC_CAT_ACK,
                     traffic_debug_classify_packet(PKT_TYPE_STORE_ACK));
}

void test_classify_chat_packets(void) {
    TEST_ASSERT_EQUAL(TRAFFIC_CAT_CHAT,
                     traffic_debug_classify_packet(PKT_TYPE_DATA));
}

void test_classify_maintenance_packets(void) {
    TEST_ASSERT_EQUAL(TRAFFIC_CAT_MAINTENANCE,
                     traffic_debug_classify_packet(PKT_TYPE_KEY_EXCHANGE));
    TEST_ASSERT_EQUAL(TRAFFIC_CAT_MAINTENANCE,
                     traffic_debug_classify_packet(PKT_TYPE_STORE_REQUEST));
    TEST_ASSERT_EQUAL(TRAFFIC_CAT_MAINTENANCE,
                     traffic_debug_classify_packet(PKT_TYPE_MAILBOX_DELIVERY));
    TEST_ASSERT_EQUAL(TRAFFIC_CAT_MAINTENANCE,
                     traffic_debug_classify_packet(PKT_TYPE_MAILBOX_QUERY));
    TEST_ASSERT_EQUAL(TRAFFIC_CAT_MAINTENANCE,
                     traffic_debug_classify_packet(PKT_TYPE_CODED));
    TEST_ASSERT_EQUAL(TRAFFIC_CAT_MAINTENANCE,
                     traffic_debug_classify_packet(PKT_TYPE_LOCATION));
}

void test_classify_other_packets(void) {
    /* Emergency packets are "other" - they bypass normal classification */
    TEST_ASSERT_EQUAL(TRAFFIC_CAT_OTHER,
                     traffic_debug_classify_packet(PKT_TYPE_EMERGENCY));
    TEST_ASSERT_EQUAL(TRAFFIC_CAT_OTHER,
                     traffic_debug_classify_packet(PKT_TYPE_EMERGENCY_CANCEL));
    /* Unknown packet type */
    TEST_ASSERT_EQUAL(TRAFFIC_CAT_OTHER,
                     traffic_debug_classify_packet(0xFF));
}

/* ── Category + Tier → Airtime Bucket Mapping ─────────────────────── */

void test_beacon_maps_to_broadcast(void) {
    TEST_ASSERT_EQUAL(AIRTIME_TIER_BROADCAST,
                     traffic_debug_get_airtime_tier(TRAFFIC_CAT_BEACON, AIRTIME_TIER_NORMAL));
}

void test_timesync_maps_to_broadcast(void) {
    TEST_ASSERT_EQUAL(AIRTIME_TIER_BROADCAST,
                     traffic_debug_get_airtime_tier(TRAFFIC_CAT_TIMESYNC, AIRTIME_TIER_NORMAL));
}

void test_routing_maps_to_normal(void) {
    TEST_ASSERT_EQUAL(AIRTIME_TIER_NORMAL,
                     traffic_debug_get_airtime_tier(TRAFFIC_CAT_ROUTING, AIRTIME_TIER_NORMAL));
}

void test_ack_maps_to_normal(void) {
    TEST_ASSERT_EQUAL(AIRTIME_TIER_NORMAL,
                     traffic_debug_get_airtime_tier(TRAFFIC_CAT_ACK, AIRTIME_TIER_NORMAL));
}

void test_chat_respects_tier_hint(void) {
    TEST_ASSERT_EQUAL(AIRTIME_TIER_NORMAL,
                     traffic_debug_get_airtime_tier(TRAFFIC_CAT_CHAT, AIRTIME_TIER_NORMAL));
    TEST_ASSERT_EQUAL(AIRTIME_TIER_CRITICAL,
                     traffic_debug_get_airtime_tier(TRAFFIC_CAT_CHAT, AIRTIME_TIER_CRITICAL));
}

void test_maintenance_maps_to_normal(void) {
    TEST_ASSERT_EQUAL(AIRTIME_TIER_NORMAL,
                     traffic_debug_get_airtime_tier(TRAFFIC_CAT_MAINTENANCE, AIRTIME_TIER_NORMAL));
}

void test_other_respects_tier_hint(void) {
    /* Emergency packets should use their original tier */
    TEST_ASSERT_EQUAL(AIRTIME_TIER_CRITICAL,
                     traffic_debug_get_airtime_tier(TRAFFIC_CAT_OTHER, AIRTIME_TIER_CRITICAL));
    TEST_ASSERT_EQUAL(AIRTIME_TIER_BROADCAST,
                     traffic_debug_get_airtime_tier(TRAFFIC_CAT_OTHER, AIRTIME_TIER_BROADCAST));
}

/* ── Monotonic Sequence Increments ───────────────────────────────── */

void test_seq_starts_at_zero(void) {
    traffic_debug_init(&td, events, 32);
    traffic_debug_enable(&td, true);
    
    traffic_debug_record_tx(&td, PKT_TYPE_BEACON, 100, AIRTIME_TIER_BROADCAST);
    const traffic_event_t *evt = traffic_debug_get_event(&td, 0);
    TEST_ASSERT_NOT_NULL(evt);
    TEST_ASSERT_EQUAL_UINT32(0, evt->seq);
}

void test_seq_increments_monotonically(void) {
    traffic_debug_init(&td, events, 32);
    traffic_debug_enable(&td, true);
    
    traffic_debug_record_tx(&td, PKT_TYPE_BEACON, 100, AIRTIME_TIER_BROADCAST);
    traffic_debug_record_rx(&td, PKT_TYPE_ACK, 50, -60);
    traffic_debug_record_tx(&td, PKT_TYPE_DATA, 200, AIRTIME_TIER_NORMAL);
    
    const traffic_event_t *evt0 = traffic_debug_get_event(&td, 0);
    const traffic_event_t *evt1 = traffic_debug_get_event(&td, 1);
    const traffic_event_t *evt2 = traffic_debug_get_event(&td, 2);
    
    TEST_ASSERT_EQUAL_UINT32(0, evt0->seq);
    TEST_ASSERT_EQUAL_UINT32(1, evt1->seq);
    TEST_ASSERT_EQUAL_UINT32(2, evt2->seq);
}

void test_seq_never_decreases(void) {
    traffic_debug_init(&td, events, 8);
    traffic_debug_enable(&td, true);
    
    /* Fill buffer and wrap around */
    for (int i = 0; i < 20; i++) {
        traffic_debug_record_tx(&td, PKT_TYPE_BEACON, 100, AIRTIME_TIER_BROADCAST);
    }
    
    /* All visible events should have monotonically increasing seq */
    uint16_t count = traffic_debug_get_count(&td);
    TEST_ASSERT_EQUAL(8, count);
    
    uint32_t prev_seq = 0;
    for (uint16_t i = 0; i < count; i++) {
        const traffic_event_t *evt = traffic_debug_get_event(&td, i);
        if (i > 0) {
            TEST_ASSERT_GREATER_THAN(prev_seq, evt->seq);
        }
        prev_seq = evt->seq;
    }
}

/* ── Ring Buffer Wrap (Newest N + Dropped Count) ────────────────── */

void test_init_sets_capacity(void) {
    traffic_debug_init(&td, events, 16);
    TEST_ASSERT_EQUAL(16, td.capacity);
    TEST_ASSERT_EQUAL(0, td.count);
    TEST_ASSERT_EQUAL(0, td.dropped_count);
}

void test_buffer_stores_up_to_capacity(void) {
    traffic_debug_init(&td, events, 8);
    traffic_debug_enable(&td, true);
    
    for (int i = 0; i < 8; i++) {
        traffic_debug_record_tx(&td, PKT_TYPE_BEACON, 100, AIRTIME_TIER_BROADCAST);
    }
    
    TEST_ASSERT_EQUAL(8, traffic_debug_get_count(&td));
    TEST_ASSERT_EQUAL(0, traffic_debug_get_dropped(&td));
}

void test_buffer_wrap_drops_oldest(void) {
    traffic_debug_init(&td, events, 8);
    traffic_debug_enable(&td, true);
    
    /* Add 12 events to 8-slot buffer */
    for (int i = 0; i < 12; i++) {
        traffic_debug_record_tx(&td, PKT_TYPE_BEACON, 100 + i, AIRTIME_TIER_BROADCAST);
    }
    
    /* Should have newest 8 events */
    TEST_ASSERT_EQUAL(8, traffic_debug_get_count(&td));
    TEST_ASSERT_EQUAL(4, traffic_debug_get_dropped(&td));
    
    /* First visible event should be seq=4 (events 0-3 were dropped) */
    const traffic_event_t *evt = traffic_debug_get_event(&td, 0);
    TEST_ASSERT_EQUAL_UINT32(4, evt->seq);
    TEST_ASSERT_EQUAL(104, evt->packet_len);
}

void test_buffer_retains_newest_after_many_wraps(void) {
    traffic_debug_init(&td, events, 4);
    traffic_debug_enable(&td, true);
    
    /* Add 100 events to 4-slot buffer */
    for (int i = 0; i < 100; i++) {
        traffic_debug_record_tx(&td, PKT_TYPE_BEACON, 100 + i, AIRTIME_TIER_BROADCAST);
    }
    
    TEST_ASSERT_EQUAL(4, traffic_debug_get_count(&td));
    TEST_ASSERT_EQUAL(96, traffic_debug_get_dropped(&td));
    
    /* Should have events 96, 97, 98, 99 */
    for (uint16_t i = 0; i < 4; i++) {
        const traffic_event_t *evt = traffic_debug_get_event(&td, i);
        TEST_ASSERT_EQUAL_UINT32(96 + i, evt->seq);
        TEST_ASSERT_EQUAL(196 + i, evt->packet_len);
    }
}

void test_dropped_count_tracks_total_dropped(void) {
    traffic_debug_init(&td, events, 5);
    traffic_debug_enable(&td, true);
    
    /* Add events in batches to verify cumulative tracking */
    for (int i = 0; i < 10; i++) {
        traffic_debug_record_tx(&td, PKT_TYPE_BEACON, 100, AIRTIME_TIER_BROADCAST);
    }
    TEST_ASSERT_EQUAL(5, traffic_debug_get_dropped(&td));
    
    for (int i = 0; i < 7; i++) {
        traffic_debug_record_tx(&td, PKT_TYPE_BEACON, 100, AIRTIME_TIER_BROADCAST);
    }
    TEST_ASSERT_EQUAL(12, traffic_debug_get_dropped(&td));
}

/* ── Debug Disabled => No Event Emission ──────────────────────────── */

void test_disabled_by_default(void) {
    traffic_debug_init(&td, events, 32);
    TEST_ASSERT_FALSE(traffic_debug_is_enabled(&td));
}

void test_enable_flag_works(void) {
    traffic_debug_init(&td, events, 32);
    traffic_debug_enable(&td, true);
    TEST_ASSERT_TRUE(traffic_debug_is_enabled(&td));
    
    traffic_debug_enable(&td, false);
    TEST_ASSERT_FALSE(traffic_debug_is_enabled(&td));
}

void test_disabled_drops_all_events(void) {
    traffic_debug_init(&td, events, 32);
    /* Explicitly disabled */
    traffic_debug_enable(&td, false);
    
    traffic_debug_record_tx(&td, PKT_TYPE_BEACON, 100, AIRTIME_TIER_BROADCAST);
    traffic_debug_record_rx(&td, PKT_TYPE_ACK, 50, -60);
    
    TEST_ASSERT_EQUAL(0, traffic_debug_get_count(&td));
    TEST_ASSERT_EQUAL(0, traffic_debug_get_dropped(&td));
}

void test_disabled_does_not_increment_seq(void) {
    traffic_debug_init(&td, events, 32);
    traffic_debug_enable(&td, false);
    
    traffic_debug_record_tx(&td, PKT_TYPE_BEACON, 100, AIRTIME_TIER_BROADCAST);
    traffic_debug_record_tx(&td, PKT_TYPE_BEACON, 100, AIRTIME_TIER_BROADCAST);
    
    /* Enable and record - should still start at seq 0 */
    traffic_debug_enable(&td, true);
    traffic_debug_record_tx(&td, PKT_TYPE_BEACON, 100, AIRTIME_TIER_BROADCAST);
    
    const traffic_event_t *evt = traffic_debug_get_event(&td, 0);
    TEST_ASSERT_EQUAL_UINT32(0, evt->seq);
}

void test_enable_disable_toggle(void) {
    traffic_debug_init(&td, events, 32);
    
    traffic_debug_enable(&td, true);
    traffic_debug_record_tx(&td, PKT_TYPE_BEACON, 100, AIRTIME_TIER_BROADCAST);
    TEST_ASSERT_EQUAL(1, traffic_debug_get_count(&td));
    
    traffic_debug_enable(&td, false);
    traffic_debug_record_tx(&td, PKT_TYPE_BEACON, 100, AIRTIME_TIER_BROADCAST);
    TEST_ASSERT_EQUAL(1, traffic_debug_get_count(&td));  /* No new event */
    
    traffic_debug_enable(&td, true);
    traffic_debug_record_tx(&td, PKT_TYPE_BEACON, 100, AIRTIME_TIER_BROADCAST);
    TEST_ASSERT_EQUAL(2, traffic_debug_get_count(&td));
}

/* ── Record TX/RX Event Details ──────────────────────────────────── */

void test_record_tx_populates_fields(void) {
    traffic_debug_init(&td, events, 32);
    traffic_debug_enable(&td, true);
    
    traffic_debug_record_tx(&td, PKT_TYPE_DATA, 250, AIRTIME_TIER_NORMAL);
    
    const traffic_event_t *evt = traffic_debug_get_event(&td, 0);
    TEST_ASSERT_NOT_NULL(evt);
    TEST_ASSERT_EQUAL(PKT_TYPE_DATA, evt->pkt_type);
    TEST_ASSERT_EQUAL(TRAFFIC_CAT_CHAT, evt->category);
    TEST_ASSERT_EQUAL(AIRTIME_TIER_NORMAL, evt->airtime_tier);
    TEST_ASSERT_EQUAL(250, evt->packet_len);
    TEST_ASSERT_TRUE(evt->is_tx);
}

void test_record_rx_populates_fields(void) {
    traffic_debug_init(&td, events, 32);
    traffic_debug_enable(&td, true);
    
    traffic_debug_record_rx(&td, PKT_TYPE_BEACON, 120, -75);
    
    const traffic_event_t *evt = traffic_debug_get_event(&td, 0);
    TEST_ASSERT_NOT_NULL(evt);
    TEST_ASSERT_EQUAL(PKT_TYPE_BEACON, evt->pkt_type);
    TEST_ASSERT_EQUAL(TRAFFIC_CAT_BEACON, evt->category);
    TEST_ASSERT_EQUAL(120, evt->packet_len);
    TEST_ASSERT_EQUAL(-75, evt->rssi);
    TEST_ASSERT_FALSE(evt->is_tx);
}

/* ── Unity Runner ─────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    
    /* Packet type → category mapping */
    RUN_TEST(test_classify_beacon);
    RUN_TEST(test_classify_routing_packets);
    RUN_TEST(test_classify_ack_packets);
    RUN_TEST(test_classify_chat_packets);
    RUN_TEST(test_classify_maintenance_packets);
    RUN_TEST(test_classify_other_packets);
    
    /* Category + tier → airtime bucket mapping */
    RUN_TEST(test_beacon_maps_to_broadcast);
    RUN_TEST(test_timesync_maps_to_broadcast);
    RUN_TEST(test_routing_maps_to_normal);
    RUN_TEST(test_ack_maps_to_normal);
    RUN_TEST(test_chat_respects_tier_hint);
    RUN_TEST(test_maintenance_maps_to_normal);
    RUN_TEST(test_other_respects_tier_hint);
    
    /* Monotonic sequence increments */
    RUN_TEST(test_seq_starts_at_zero);
    RUN_TEST(test_seq_increments_monotonically);
    RUN_TEST(test_seq_never_decreases);
    
    /* Ring buffer wrap retains newest N + dropped count */
    RUN_TEST(test_init_sets_capacity);
    RUN_TEST(test_buffer_stores_up_to_capacity);
    RUN_TEST(test_buffer_wrap_drops_oldest);
    RUN_TEST(test_buffer_retains_newest_after_many_wraps);
    RUN_TEST(test_dropped_count_tracks_total_dropped);
    
    /* Debug disabled => no event emission */
    RUN_TEST(test_disabled_by_default);
    RUN_TEST(test_enable_flag_works);
    RUN_TEST(test_disabled_drops_all_events);
    RUN_TEST(test_disabled_does_not_increment_seq);
    RUN_TEST(test_enable_disable_toggle);
    
    /* Event details */
    RUN_TEST(test_record_tx_populates_fields);
    RUN_TEST(test_record_rx_populates_fields);
    
    return UNITY_END();
}
