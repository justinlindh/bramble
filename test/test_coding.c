#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "coding.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void name(void)
#define RUN(name) do { \
    printf("  %-50s", #name); \
    name(); \
    printf("PASS\n"); \
    tests_passed++; \
} while(0)

TEST(test_coding_encode_decode) {
    uint8_t pkt_a[] = "Hello, World!";
    uint8_t pkt_b[] = "Goodbye World";
    uint16_t len_a = 13, len_b = 13;
    uint32_t id_a = 100, id_b = 200;

    uint8_t coded[512];
    uint16_t coded_len;
    assert(coding_encode(pkt_a, len_a, id_a, pkt_b, len_b, id_b, coded, &coded_len) == 0);

    /* Decode B using known A */
    coded_header_t hdr;
    assert(coded_header_deserialize(coded, coded_len, &hdr) > 0);

    uint8_t decoded[256];
    uint16_t decoded_len;
    assert(coding_decode(coded, coded_len, &hdr, pkt_a, len_a, id_a, decoded, &decoded_len) == 0);
    assert(decoded_len == len_b);
    assert(memcmp(decoded, pkt_b, len_b) == 0);

    /* Decode A using known B */
    assert(coding_decode(coded, coded_len, &hdr, pkt_b, len_b, id_b, decoded, &decoded_len) == 0);
    assert(decoded_len == len_a);
    assert(memcmp(decoded, pkt_a, len_a) == 0);
}

TEST(test_coding_different_lengths) {
    uint8_t pkt_a[] = "Short";
    uint8_t pkt_b[] = "This is a much longer packet";
    uint16_t len_a = 5, len_b = 28;
    uint32_t id_a = 1, id_b = 2;

    uint8_t coded[512];
    uint16_t coded_len;
    assert(coding_encode(pkt_a, len_a, id_a, pkt_b, len_b, id_b, coded, &coded_len) == 0);

    coded_header_t hdr;
    coded_header_deserialize(coded, coded_len, &hdr);

    uint8_t decoded[256];
    uint16_t decoded_len;

    /* Decode B from known A */
    assert(coding_decode(coded, coded_len, &hdr, pkt_a, len_a, id_a, decoded, &decoded_len) == 0);
    assert(decoded_len == len_b);
    assert(memcmp(decoded, pkt_b, len_b) == 0);

    /* Decode A from known B */
    assert(coding_decode(coded, coded_len, &hdr, pkt_b, len_b, id_b, decoded, &decoded_len) == 0);
    assert(decoded_len == len_a);
    assert(memcmp(decoded, pkt_a, len_a) == 0);
}

TEST(test_coding_reception_cache) {
    coding_engine_t engine;
    coding_init(&engine);

    coding_record_packet(&engine, 42);
    coding_record_packet(&engine, 99);
    coding_record_packet(&engine, 7);

    /* can_decode uses my_cache internally; test via coding_can_decode */
    coded_header_t hdr = { .num_components = 2, .component_ids = {42, 55}, .component_lens = {10, 10} };
    uint32_t known_id;
    assert(coding_can_decode(&engine, &hdr, &known_id) == true);
    assert(known_id == 42);

    coded_header_t hdr2 = { .num_components = 2, .component_ids = {111, 222}, .component_lens = {10, 10} };
    assert(coding_can_decode(&engine, &hdr2, &known_id) == false);
}

TEST(test_coding_neighbor_knowledge) {
    coding_engine_t engine;
    coding_init(&engine);

    uint32_t ids[] = {10, 20, 30};
    coding_record_neighbor_reception(&engine, 0xAABB, ids, 3);

    assert(coding_neighbor_has_packet(&engine, 0xAABB, 10) == true);
    assert(coding_neighbor_has_packet(&engine, 0xAABB, 20) == true);
    assert(coding_neighbor_has_packet(&engine, 0xAABB, 30) == true);
    assert(coding_neighbor_has_packet(&engine, 0xAABB, 40) == false);
    assert(coding_neighbor_has_packet(&engine, 0xCCDD, 10) == false);
}

TEST(test_coding_find_opportunity) {
    coding_engine_t engine;
    coding_init(&engine);

    /* Packet A (id=1) going to neighbor X (addr=0x10)
       Packet B (id=2) going to neighbor Y (addr=0x20) */
    uint8_t data_a[] = "AAA";
    uint8_t data_b[] = "BBB";
    coding_queue_packet(&engine, data_a, 3, 1, 0x10, 100);
    coding_queue_packet(&engine, data_b, 3, 2, 0x20, 100);

    /* X knows about packet 2, Y knows about packet 1 */
    uint32_t x_knows[] = {2};
    uint32_t y_knows[] = {1};
    coding_record_neighbor_reception(&engine, 0x10, x_knows, 1);
    coding_record_neighbor_reception(&engine, 0x20, y_knows, 1);

    int a, b;
    assert(coding_find_opportunity(&engine, &a, &b) == 0);
    assert(a == 0 && b == 1);
}

TEST(test_coding_no_opportunity) {
    coding_engine_t engine;
    coding_init(&engine);

    uint8_t data_a[] = "AAA";
    uint8_t data_b[] = "BBB";
    coding_queue_packet(&engine, data_a, 3, 1, 0x10, 100);
    coding_queue_packet(&engine, data_b, 3, 2, 0x20, 100);

    /* Neighbors don't have cross-knowledge */
    int a, b;
    assert(coding_find_opportunity(&engine, &a, &b) == -1);
}

TEST(test_coding_flush_expired) {
    coding_engine_t engine;
    coding_init(&engine);

    uint8_t data[] = "test";
    coding_queue_packet(&engine, data, 4, 1, 0x10, 100);
    coding_queue_packet(&engine, data, 4, 2, 0x20, 500);

    /* At t=601, first packet expired (100+500=600), second still active */
    coding_flush_expired(&engine, 601);
    assert(engine.queue[0].active == false);
    assert(engine.queue[1].active == true);

    /* At t=1001, second also expired */
    coding_flush_expired(&engine, 1001);
    assert(engine.queue[1].active == false);
}

TEST(test_coding_header_roundtrip) {
    coded_header_t hdr = {
        .num_components = 2,
        .component_ids = {0xDEADBEEF, 0xCAFEBABE},
        .component_lens = {100, 200}
    };

    uint8_t buf[64];
    int len = coded_header_serialize(&hdr, buf, sizeof(buf));
    assert(len == 13);

    coded_header_t hdr2;
    int consumed = coded_header_deserialize(buf, (size_t)len, &hdr2);
    assert(consumed == 13);
    assert(hdr2.num_components == 2);
    assert(hdr2.component_ids[0] == 0xDEADBEEF);
    assert(hdr2.component_ids[1] == 0xCAFEBABE);
    assert(hdr2.component_lens[0] == 100);
    assert(hdr2.component_lens[1] == 200);
}

int main(void) {
    printf("Network coding tests:\n");
    RUN(test_coding_encode_decode);
    RUN(test_coding_different_lengths);
    RUN(test_coding_reception_cache);
    RUN(test_coding_neighbor_knowledge);
    RUN(test_coding_find_opportunity);
    RUN(test_coding_no_opportunity);
    RUN(test_coding_flush_expired);
    RUN(test_coding_header_roundtrip);
    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
