#include "unity.h"

#include "delivery_event_ring.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static delivery_event_record_t make_event(uint32_t message_id) {
    delivery_event_record_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.message_id = message_id;
    ev.timestamp_s = 1700000000u + message_id;
    ev.recipient_addr = 0x01020300u + message_id;
    ev.source_addr = 0x0A0B0C0Du;
    ev.event_type = (uint8_t)(message_id & 0xFFu);
    ev.tier = 1u;
    ev.route_len = 1u;
    ev.route_hops[0] = ev.source_addr;
    return ev;
}

void test_seq_monotonic_after_wrap(void) {
    delivery_event_ring_t ring;
    delivery_event_record_t out[DELIVERY_EVENT_RING_CAPACITY];
    size_t got;

    delivery_event_ring_init(&ring);

    for (uint32_t i = 1u; i <= (DELIVERY_EVENT_RING_CAPACITY + 3u); i++) {
        delivery_event_record_t ev = make_event(i);
        uint32_t seq = delivery_event_ring_append(&ring, &ev);
        TEST_ASSERT_EQUAL_UINT32(i, seq);
    }

    TEST_ASSERT_EQUAL_UINT32(DELIVERY_EVENT_RING_CAPACITY, delivery_event_ring_count(&ring));
    TEST_ASSERT_EQUAL_UINT32(DELIVERY_EVENT_RING_CAPACITY + 3u,
                             delivery_event_ring_latest_seq(&ring));

    got = delivery_event_ring_list_since(&ring, 0u, out, DELIVERY_EVENT_RING_CAPACITY);
    TEST_ASSERT_EQUAL_UINT32(DELIVERY_EVENT_RING_CAPACITY, got);

    for (size_t i = 0; i < got; i++) {
        TEST_ASSERT_EQUAL_UINT32(4u + i, out[i].event_seq);
    }
}

void test_wrap_preserves_chronological_order(void) {
    delivery_event_ring_t ring;
    delivery_event_record_t out[DELIVERY_EVENT_RING_CAPACITY];

    delivery_event_ring_init(&ring);

    for (uint32_t i = 1u; i <= (DELIVERY_EVENT_RING_CAPACITY + 2u); i++) {
        delivery_event_record_t ev = make_event(i);
        (void)delivery_event_ring_append(&ring, &ev);
    }

    size_t got = delivery_event_ring_list_since(&ring, 0u, out, DELIVERY_EVENT_RING_CAPACITY);
    TEST_ASSERT_EQUAL_UINT32(DELIVERY_EVENT_RING_CAPACITY, got);

    for (size_t i = 0; i < got; i++) {
        TEST_ASSERT_EQUAL_UINT32(3u + i, out[i].message_id);
        TEST_ASSERT_EQUAL_UINT32(3u + i, out[i].event_seq);
    }
}

void test_since_seq_filters_and_serialize_roundtrip(void) {
    delivery_event_ring_t ring;
    delivery_event_ring_t restored;
    delivery_event_record_t out[DELIVERY_EVENT_RING_CAPACITY];
    uint8_t blob[sizeof(delivery_event_ring_t)];

    delivery_event_ring_init(&ring);
    for (uint32_t i = 1u; i <= DELIVERY_EVENT_RING_CAPACITY; i++) {
        delivery_event_record_t ev = make_event(i);
        (void)delivery_event_ring_append(&ring, &ev);
    }

    size_t got = delivery_event_ring_list_since(&ring, 2u, out, DELIVERY_EVENT_RING_CAPACITY);
    TEST_ASSERT_EQUAL_UINT32(2u, got);
    TEST_ASSERT_EQUAL_UINT32(3u, out[0].event_seq);
    TEST_ASSERT_EQUAL_UINT32(4u, out[1].event_seq);

    size_t encoded = delivery_event_ring_serialize(&ring, blob, sizeof(blob));
    TEST_ASSERT_TRUE(encoded > 0u);

    size_t decoded = delivery_event_ring_deserialize(&restored, blob, encoded);
    TEST_ASSERT_EQUAL_UINT32(encoded, decoded);

    delivery_event_record_t ev5 = make_event(99u);
    uint32_t seq = delivery_event_ring_append(&restored, &ev5);
    TEST_ASSERT_EQUAL_UINT32(DELIVERY_EVENT_RING_CAPACITY + 1u, seq);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_seq_monotonic_after_wrap);
    RUN_TEST(test_wrap_preserves_chronological_order);
    RUN_TEST(test_since_seq_filters_and_serialize_roundtrip);
    return UNITY_END();
}
