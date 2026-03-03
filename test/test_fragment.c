#include "unity.h"
#include "../components/fragment/fragment.c"

void setUp(void) {}
void tearDown(void) {}

/* Test 1: Split small message → 1 fragment */
void test_split_small(void) {
    uint8_t data[100];
    memset(data, 0xAB, sizeof(data));
    fragment_t frags[8];
    int n = fragment_split(data, 100, 0x1234, frags, 8);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL(FRAG_HEADER_SIZE + 100, frags[0].len);
    TEST_ASSERT_EQUAL(0, frags[0].data[0]); /* index */
    TEST_ASSERT_EQUAL(1, frags[0].data[1]); /* total */
    TEST_ASSERT_EQUAL(0x34, frags[0].data[2]); /* message_id low */
    TEST_ASSERT_EQUAL(0x12, frags[0].data[3]); /* message_id high */
    TEST_ASSERT_EQUAL(0xAB, frags[0].data[4]);
}

/* Test 2: Split 500B → 4 fragments */
void test_split_500(void) {
    uint8_t data[500];
    for (int i = 0; i < 500; i++) data[i] = (uint8_t)(i & 0xFF);
    fragment_t frags[4];
    int n = fragment_split(data, 500, 42, frags, 4);
    /* ceil(500/154) = 4 */
    TEST_ASSERT_EQUAL(4, n);
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_LESS_OR_EQUAL(FRAG_HEADER_SIZE + FRAG_MAX_PLAINTEXT, frags[i].len);
        TEST_ASSERT_EQUAL(i, frags[i].data[0]);
        TEST_ASSERT_EQUAL(4, frags[i].data[1]);
    }
    /* Last fragment: 500 - 3*154 = 38 bytes */
    TEST_ASSERT_EQUAL(FRAG_HEADER_SIZE + 38, frags[3].len);
}

/* Test 3: Split 616B → 4 fragments (max) */
void test_split_max(void) {
    uint8_t data[616];
    memset(data, 0xCC, sizeof(data));
    fragment_t frags[4];
    int n = fragment_split(data, 616, 99, frags, 4);
    TEST_ASSERT_EQUAL(4, n);
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL(i, frags[i].data[0]);
        TEST_ASSERT_EQUAL(4, frags[i].data[1]);
    }
}

/* Test 4: Reject >616B */
void test_split_too_large(void) {
    uint8_t data[617];
    memset(data, 0, sizeof(data));
    fragment_t frags[4];
    int n = fragment_split(data, 617, 1, frags, 4);
    TEST_ASSERT_EQUAL(-1, n);
}

/* Test 5: Reassembly in order */
void test_reassembly_in_order(void) {
    reassembly_ctx_t ctx;
    reassembly_init(&ctx);

    uint8_t orig[300];
    for (int i = 0; i < 300; i++) orig[i] = (uint8_t)(i & 0xFF);
    fragment_t frags[4];
    int n = fragment_split(orig, 300, 0x5678, frags, 4);
    TEST_ASSERT_EQUAL(2, n);

    for (int i = 0; i < n; i++) {
        frag_header_t hdr = {
            .frag_index = frags[i].data[0],
            .frag_total = frags[i].data[1],
            .message_id = (uint16_t)(frags[i].data[2] | (frags[i].data[3] << 8))
        };
        int rc = reassembly_add(&ctx, &hdr,
                                frags[i].data + FRAG_HEADER_SIZE,
                                frags[i].len - FRAG_HEADER_SIZE,
                                1000, 0x1000 + i);
        if (i < n - 1) TEST_ASSERT_EQUAL(0, rc);
        else TEST_ASSERT_EQUAL(1, rc);
    }

    uint8_t out[616];
    int len = reassembly_collect(&ctx, 0x5678, out, sizeof(out));
    TEST_ASSERT_EQUAL(300, len);
    TEST_ASSERT_EQUAL_MEMORY(orig, out, 300);
}

/* Test 6: Reassembly out of order */
void test_reassembly_out_of_order(void) {
    reassembly_ctx_t ctx;
    reassembly_init(&ctx);

    uint8_t orig[400];
    for (int i = 0; i < 400; i++) orig[i] = (uint8_t)i;
    fragment_t frags[4];
    int n = fragment_split(orig, 400, 100, frags, 4);
    TEST_ASSERT_EQUAL(3, n);

    /* Add in reverse order */
    int order[] = {2, 0, 1};
    for (int k = 0; k < n; k++) {
        int i = order[k];
        frag_header_t hdr = {
            .frag_index = frags[i].data[0],
            .frag_total = frags[i].data[1],
            .message_id = (uint16_t)(frags[i].data[2] | (frags[i].data[3] << 8))
        };
        int rc = reassembly_add(&ctx, &hdr,
                                frags[i].data + FRAG_HEADER_SIZE,
                                frags[i].len - FRAG_HEADER_SIZE,
                                2000, 0x2000 + i);
        if (k < n - 1) TEST_ASSERT_EQUAL(0, rc);
        else TEST_ASSERT_EQUAL(1, rc);
    }

    uint8_t out[616];
    int len = reassembly_collect(&ctx, 100, out, sizeof(out));
    TEST_ASSERT_EQUAL(400, len);
    TEST_ASSERT_EQUAL_MEMORY(orig, out, 400);
}

/* Test 7: Duplicate fragment ignored */
void test_reassembly_duplicate(void) {
    reassembly_ctx_t ctx;
    reassembly_init(&ctx);

    frag_header_t hdr = {.frag_index = 0, .frag_total = 2, .message_id = 50};
    uint8_t data[10] = {1,2,3,4,5,6,7,8,9,10};

    int rc = reassembly_add(&ctx, &hdr, data, 10, 1000, 0);
    TEST_ASSERT_EQUAL(0, rc);
    /* Add same fragment again */
    rc = reassembly_add(&ctx, &hdr, data, 10, 1000, 0);
    TEST_ASSERT_EQUAL(0, rc); /* duplicate ignored, still not complete */
}

/* Test 8: Timeout → purged */
void test_reassembly_timeout(void) {
    reassembly_ctx_t ctx;
    reassembly_init(&ctx);

    frag_header_t hdr = {.frag_index = 0, .frag_total = 2, .message_id = 77};
    uint8_t data[10] = {0};
    reassembly_add(&ctx, &hdr, data, 10, 1000, 0);

    /* Purge after timeout */
    reassembly_purge(&ctx, 1000 + FRAG_REASSEMBLY_TIMEOUT_MS + 1);

    /* Slot should be freed — try adding frag 1, should start new slot */
    hdr.frag_index = 1;
    int rc = reassembly_add(&ctx, &hdr, data, 10, 32000, 0);
    TEST_ASSERT_EQUAL(0, rc); /* new slot, not complete (missing frag 0) */
}

/* Test 9: 4 concurrent messages */
void test_reassembly_concurrent(void) {
    reassembly_ctx_t ctx;
    reassembly_init(&ctx);

    /* Start 4 messages */
    for (int m = 0; m < 4; m++) {
        frag_header_t hdr = {.frag_index = 0, .frag_total = 2, .message_id = (uint16_t)(m + 10)};
        uint8_t data[10];
        memset(data, (uint8_t)m, 10);
        int rc = reassembly_add(&ctx, &hdr, data, 10, 5000, 0);
        TEST_ASSERT_EQUAL(0, rc);
    }

    /* 5th message should fail (no slots) */
    frag_header_t hdr5 = {.frag_index = 0, .frag_total = 2, .message_id = 99};
    uint8_t data5[10] = {0};
    int rc = reassembly_add(&ctx, &hdr5, data5, 10, 5000, 0);
    TEST_ASSERT_EQUAL(-1, rc);

    /* Complete message 10 */
    frag_header_t hdr_fin = {.frag_index = 1, .frag_total = 2, .message_id = 10};
    uint8_t data_fin[10];
    memset(data_fin, 0x10, 10);
    rc = reassembly_add(&ctx, &hdr_fin, data_fin, 10, 5000, 0);
    TEST_ASSERT_EQUAL(1, rc);

    uint8_t out[20];
    int len = reassembly_collect(&ctx, 10, out, sizeof(out));
    TEST_ASSERT_EQUAL(20, len);
}

/* Test 10: Reject messages exceeding 616 bytes (4 * 154) */
void test_reject_over_616(void) {
    uint8_t data[617];
    memset(data, 0xDD, sizeof(data));
    fragment_t frags[4];
    int n = fragment_split(data, 617, 200, frags, 4);
    TEST_ASSERT_EQUAL(-1, n);

    /* 616 should succeed */
    uint8_t data_ok[616];
    memset(data_ok, 0xEE, sizeof(data_ok));
    n = fragment_split(data_ok, 616, 201, frags, 4);
    TEST_ASSERT_EQUAL(4, n);
}


/* Test 11: first_packet_id stored on first fragment received (in-order) */
void test_first_packet_id_in_order(void) {
    reassembly_ctx_t ctx;
    reassembly_init(&ctx);

    uint8_t orig[300];
    memset(orig, 0x42, sizeof(orig));
    fragment_t frags[4];
    int n = fragment_split(orig, 300, 0xABCD, frags, 4);
    TEST_ASSERT_EQUAL(2, n);

    /* Add frag 0 with packet_id 0xDEAD0001 */
    frag_header_t hdr0 = {
        .frag_index = 0, .frag_total = 2, .message_id = 0xABCD
    };
    int rc = reassembly_add(&ctx, &hdr0,
                            frags[0].data + FRAG_HEADER_SIZE,
                            frags[0].len - FRAG_HEADER_SIZE,
                            1000, 0xDEAD0001);
    TEST_ASSERT_EQUAL(0, rc);

    /* first_packet_id should be frag 0's packet_id */
    uint32_t first_id = reassembly_get_first_packet_id(&ctx, 0xABCD);
    TEST_ASSERT_EQUAL_HEX32(0xDEAD0001, first_id);

    /* Add frag 1 with a different packet_id */
    frag_header_t hdr1 = {
        .frag_index = 1, .frag_total = 2, .message_id = 0xABCD
    };
    rc = reassembly_add(&ctx, &hdr1,
                        frags[1].data + FRAG_HEADER_SIZE,
                        frags[1].len - FRAG_HEADER_SIZE,
                        1000, 0xDEAD0002);
    TEST_ASSERT_EQUAL(1, rc); /* complete */

    /* first_packet_id should still be the first fragment's */
    first_id = reassembly_get_first_packet_id(&ctx, 0xABCD);
    TEST_ASSERT_EQUAL_HEX32(0xDEAD0001, first_id);
}

/* Test 12: first_packet_id with out-of-order reception */
void test_first_packet_id_out_of_order(void) {
    reassembly_ctx_t ctx;
    reassembly_init(&ctx);

    uint8_t orig[400];
    memset(orig, 0x55, sizeof(orig));
    fragment_t frags[4];
    int n = fragment_split(orig, 400, 200, frags, 4);
    TEST_ASSERT_EQUAL(3, n);

    /* Receive frag 2 first (packet_id 0xBEEF0003) */
    frag_header_t hdr2 = { .frag_index = 2, .frag_total = 3, .message_id = 200 };
    int rc = reassembly_add(&ctx, &hdr2,
                            frags[2].data + FRAG_HEADER_SIZE,
                            frags[2].len - FRAG_HEADER_SIZE,
                            1000, 0xBEEF0003);
    TEST_ASSERT_EQUAL(0, rc);

    /* first_packet_id should be frag 2's (the first received) */
    uint32_t first_id = reassembly_get_first_packet_id(&ctx, 200);
    TEST_ASSERT_EQUAL_HEX32(0xBEEF0003, first_id);

    /* Receive frag 0 (packet_id 0xBEEF0001) */
    frag_header_t hdr0 = { .frag_index = 0, .frag_total = 3, .message_id = 200 };
    rc = reassembly_add(&ctx, &hdr0,
                        frags[0].data + FRAG_HEADER_SIZE,
                        frags[0].len - FRAG_HEADER_SIZE,
                        1000, 0xBEEF0001);
    TEST_ASSERT_EQUAL(0, rc);

    /* first_packet_id should still be frag 2's */
    first_id = reassembly_get_first_packet_id(&ctx, 200);
    TEST_ASSERT_EQUAL_HEX32(0xBEEF0003, first_id);

    /* Receive frag 1 to complete */
    frag_header_t hdr1 = { .frag_index = 1, .frag_total = 3, .message_id = 200 };
    rc = reassembly_add(&ctx, &hdr1,
                        frags[1].data + FRAG_HEADER_SIZE,
                        frags[1].len - FRAG_HEADER_SIZE,
                        1000, 0xBEEF0002);
    TEST_ASSERT_EQUAL(1, rc); /* complete */

    /* first_packet_id is still from the first-received fragment */
    first_id = reassembly_get_first_packet_id(&ctx, 200);
    TEST_ASSERT_EQUAL_HEX32(0xBEEF0003, first_id);
}

/* Test 13: reassembly_get_first_packet_id returns 0 for unknown message_id */
void test_first_packet_id_unknown(void) {
    reassembly_ctx_t ctx;
    reassembly_init(&ctx);

    uint32_t first_id = reassembly_get_first_packet_id(&ctx, 9999);
    TEST_ASSERT_EQUAL_HEX32(0, first_id);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_split_small);
    RUN_TEST(test_split_500);
    RUN_TEST(test_split_max);
    RUN_TEST(test_split_too_large);
    RUN_TEST(test_reassembly_in_order);
    RUN_TEST(test_reassembly_out_of_order);
    RUN_TEST(test_reassembly_duplicate);
    RUN_TEST(test_reassembly_timeout);
    RUN_TEST(test_reassembly_concurrent);
    RUN_TEST(test_reject_over_616);
    RUN_TEST(test_first_packet_id_in_order);
    RUN_TEST(test_first_packet_id_out_of_order);
    RUN_TEST(test_first_packet_id_unknown);
    return UNITY_END();
}
