#include "unity.h"
#include "../components/airtime/tx_queue.c"

void setUp(void) {}
void tearDown(void) {}

void test_queue_enqueue_dequeue_fifo(void) {
    tx_queue_t q;
    tx_queue_init(&q);
    uint8_t d1[] = {1, 2, 3};
    uint8_t d2[] = {4, 5, 6};
    tx_queue_enqueue(&q, d1, 3, 1, 100);
    tx_queue_enqueue(&q, d2, 3, 1, 200);
    uint8_t out[256]; uint16_t len;
    TEST_ASSERT_TRUE(tx_queue_dequeue(&q, out, &len));
    TEST_ASSERT_EQUAL(3, len);
    TEST_ASSERT_EQUAL(1, out[0]); // first in, first out
    TEST_ASSERT_TRUE(tx_queue_dequeue(&q, out, &len));
    TEST_ASSERT_EQUAL(4, out[0]);
}

void test_queue_priority_ordering(void) {
    tx_queue_t q;
    tx_queue_init(&q);
    uint8_t b = 0, n = 1, c = 2;
    tx_queue_enqueue(&q, &b, 1, 0, 100); // broadcast
    tx_queue_enqueue(&q, &n, 1, 1, 200); // normal
    tx_queue_enqueue(&q, &c, 1, 2, 300); // critical
    uint8_t out[256]; uint16_t len;
    tx_queue_dequeue(&q, out, &len);
    TEST_ASSERT_EQUAL(2, out[0]); // critical first
    tx_queue_dequeue(&q, out, &len);
    TEST_ASSERT_EQUAL(1, out[0]); // normal
    tx_queue_dequeue(&q, out, &len);
    TEST_ASSERT_EQUAL(0, out[0]); // broadcast
}

void test_queue_full_drops_lowest(void) {
    tx_queue_t q;
    tx_queue_init(&q);
    // Fill with broadcast priority
    for (int i = 0; i < TX_QUEUE_SIZE; i++) {
        uint8_t d = (uint8_t)i;
        tx_queue_enqueue(&q, &d, 1, 0, i * 10);
    }
    TEST_ASSERT_TRUE(tx_queue_is_full(&q));
    // Enqueue critical - should drop oldest broadcast
    uint8_t crit = 0xFF;
    TEST_ASSERT_EQUAL(0, tx_queue_enqueue(&q, &crit, 1, 2, 1000));
    TEST_ASSERT_EQUAL(TX_QUEUE_SIZE, tx_queue_count(&q));
    // Dequeue should give critical first
    uint8_t out[256]; uint16_t len;
    tx_queue_dequeue(&q, out, &len);
    TEST_ASSERT_EQUAL(0xFF, out[0]);
}

void test_queue_empty_dequeue_returns_false(void) {
    tx_queue_t q;
    tx_queue_init(&q);
    uint8_t out[256]; uint16_t len;
    TEST_ASSERT_FALSE(tx_queue_dequeue(&q, out, &len));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_queue_enqueue_dequeue_fifo);
    RUN_TEST(test_queue_priority_ordering);
    RUN_TEST(test_queue_full_drops_lowest);
    RUN_TEST(test_queue_empty_dequeue_returns_false);
    return UNITY_END();
}
