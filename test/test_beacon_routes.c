#include "unity.h"
#include "../components/routing/beacon_routes.c"

// We need routing table functions — include the implementation
#include "../components/routing/routing.c"

void setUp(void) {}
void tearDown(void) {}

void test_select_from_empty_table(void) {
    routing_table_t table;
    route_init(&table);
    beacon_route_ads_t ads;
    beacon_select_route_ads(&table, &ads);
    TEST_ASSERT_EQUAL(0, ads.count);
}

void test_select_two_active_routes(void) {
    routing_table_t table;
    route_init(&table);
    route_install(&table, 0x1000, 0x2000, 2, 10, ROUTE_ACTIVE, 1000);
    route_install(&table, 0x3000, 0x4000, 3, 20, ROUTE_ACTIVE, 1000);

    beacon_route_ads_t ads;
    beacon_select_route_ads(&table, &ads);
    TEST_ASSERT_EQUAL(2, ads.count);
    // Should be sorted by metric: 10 first, then 20
    TEST_ASSERT_EQUAL_UINT32(0x1000, ads.routes[0].dest_addr);
    TEST_ASSERT_EQUAL(10, ads.routes[0].metric);
    TEST_ASSERT_EQUAL_UINT32(0x3000, ads.routes[1].dest_addr);
    TEST_ASSERT_EQUAL(20, ads.routes[1].metric);
}

void test_select_max_four_from_ten(void) {
    routing_table_t table;
    route_init(&table);
    for (int i = 0; i < 10; i++) {
        route_install(&table, 0x1000 + (uint32_t)i, 0x2000, 2,
                     (uint8_t)(50 - i), ROUTE_ACTIVE, 1000);
    }

    beacon_route_ads_t ads;
    beacon_select_route_ads(&table, &ads);
    TEST_ASSERT_EQUAL(BEACON_MAX_ROUTE_ADS, ads.count);
    // Best metrics should be selected (lowest values = 41,42,43,44)
    for (int i = 0; i < BEACON_MAX_ROUTE_ADS - 1; i++) {
        TEST_ASSERT_TRUE(ads.routes[i].metric <= ads.routes[i + 1].metric);
    }
}

void test_excludes_broken_and_discovering(void) {
    routing_table_t table;
    route_init(&table);
    route_install(&table, 0x1000, 0x2000, 2, 10, ROUTE_BROKEN, 1000);
    route_install(&table, 0x2000, 0x3000, 2, 15, ROUTE_DISCOVERING, 1000);
    route_install(&table, 0x3000, 0x4000, 2, 20, ROUTE_ACTIVE, 1000);

    beacon_route_ads_t ads;
    beacon_select_route_ads(&table, &ads);
    TEST_ASSERT_EQUAL(1, ads.count);
    TEST_ASSERT_EQUAL_UINT32(0x3000, ads.routes[0].dest_addr);
}

void test_serialize_deserialize_roundtrip(void) {
    beacon_route_ads_t ads = {0};
    ads.count = 2;
    ads.routes[0] = (beacon_route_ad_t){.dest_addr = 0xAABBCCDD, .metric = 5, .hop_count = 2, .flags = 1};
    ads.routes[1] = (beacon_route_ad_t){.dest_addr = 0x11223344, .metric = 10, .hop_count = 3, .flags = 0};

    uint8_t buf[64];
    int written = beacon_route_ads_serialize(&ads, buf, sizeof(buf));
    TEST_ASSERT_EQUAL(1 + 2 * BEACON_ROUTE_AD_SIZE, written);

    beacon_route_ads_t parsed;
    int consumed = beacon_route_ads_deserialize(&parsed, buf, (size_t)written);
    TEST_ASSERT_EQUAL(written, consumed);
    TEST_ASSERT_EQUAL(2, parsed.count);
    TEST_ASSERT_EQUAL_UINT32(0xAABBCCDD, parsed.routes[0].dest_addr);
    TEST_ASSERT_EQUAL(5, parsed.routes[0].metric);
    TEST_ASSERT_EQUAL_UINT32(0x11223344, parsed.routes[1].dest_addr);
    TEST_ASSERT_EQUAL(10, parsed.routes[1].metric);
}

void test_process_ads_installs_unverified(void) {
    routing_table_t table;
    route_init(&table);

    beacon_route_ads_t ads = {0};
    ads.count = 1;
    ads.routes[0] = (beacon_route_ad_t){.dest_addr = 0xAAAA, .metric = 5, .hop_count = 2, .flags = 0};

    beacon_process_route_ads(&table, 0xBBBB, &ads, 5000);

    route_entry_t *r = route_lookup(&table, 0xAAAA);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL(ROUTE_UNVERIFIED, r->state);
    TEST_ASSERT_EQUAL_UINT32(0xBBBB, r->next_hop);
    TEST_ASSERT_EQUAL(3, r->hop_count);  // 2 + 1
    TEST_ASSERT_EQUAL(6, r->metric);     // 5 + 1
}

void test_process_ads_no_overwrite_better(void) {
    routing_table_t table;
    route_init(&table);
    // Install existing good route
    route_install(&table, 0xAAAA, 0xCCCC, 1, 3, ROUTE_ACTIVE, 1000);

    beacon_route_ads_t ads = {0};
    ads.count = 1;
    ads.routes[0] = (beacon_route_ad_t){.dest_addr = 0xAAAA, .metric = 5, .hop_count = 2, .flags = 0};

    beacon_process_route_ads(&table, 0xBBBB, &ads, 5000);

    route_entry_t *r = route_lookup(&table, 0xAAAA);
    TEST_ASSERT_NOT_NULL(r);
    // Should keep existing better route (metric 3 < 6)
    TEST_ASSERT_EQUAL_UINT32(0xCCCC, r->next_hop);
    TEST_ASSERT_EQUAL(3, r->metric);
}

void test_empty_ads_serialize(void) {
    beacon_route_ads_t ads = {0};
    ads.count = 0;

    uint8_t buf[8];
    int written = beacon_route_ads_serialize(&ads, buf, sizeof(buf));
    TEST_ASSERT_EQUAL(1, written);
    TEST_ASSERT_EQUAL(0, buf[0]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_select_from_empty_table);
    RUN_TEST(test_select_two_active_routes);
    RUN_TEST(test_select_max_four_from_ten);
    RUN_TEST(test_excludes_broken_and_discovering);
    RUN_TEST(test_serialize_deserialize_roundtrip);
    RUN_TEST(test_process_ads_installs_unverified);
    RUN_TEST(test_process_ads_no_overwrite_better);
    RUN_TEST(test_empty_ads_serialize);
    return UNITY_END();
}
