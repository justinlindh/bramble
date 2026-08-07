/* Map zoom ladder: the arithmetic and the readout behind the Map screen's
 * zoom controls. scr_map.c is LVGL and not host-linkable, so the pure part
 * lives in map_zoom.c and is tested here (same split as test_chat_message_ui). */
#include "unity.h"
#include "map_zoom.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* Unity ships with double comparisons excluded, so levels are asserted in
 * whole metres, which every rung is. */
static int level_m(int idx) { return (int)(map_zoom_level_km(idx) * 1000.0 + 0.5); }

static const int LADDER_M[MAP_ZOOM_LEVEL_COUNT] = {500, 1000, 2000, 5000, 10000, 25000, 50000};

void test_levels_ascend_and_clamp(void) {
    for (int i = 0; i < MAP_ZOOM_LEVEL_COUNT; i++) {
        TEST_ASSERT_EQUAL_INT(LADDER_M[i], level_m(i));
    }
    /* Out of range reads the nearest end, never off the array. */
    TEST_ASSERT_EQUAL_INT(LADDER_M[0], level_m(-1));
    TEST_ASSERT_EQUAL_INT(LADDER_M[0], level_m(-99));
    TEST_ASSERT_EQUAL_INT(LADDER_M[MAP_ZOOM_LEVEL_COUNT - 1], level_m(MAP_ZOOM_LEVEL_COUNT));
}

void test_index_for_km_picks_the_first_level_that_fits(void) {
    TEST_ASSERT_EQUAL_INT(0, map_zoom_index_for_km(0.1));
    TEST_ASSERT_EQUAL_INT(0, map_zoom_index_for_km(0.5)); /* exact match, not the next rung */
    TEST_ASSERT_EQUAL_INT(1, map_zoom_index_for_km(0.6));
    TEST_ASSERT_EQUAL_INT(3, map_zoom_index_for_km(4.9));
    TEST_ASSERT_EQUAL_INT(6, map_zoom_index_for_km(50.0));
    /* Nothing on the ladder is wide enough: pin to the widest. */
    TEST_ASSERT_EQUAL_INT(MAP_ZOOM_LEVEL_COUNT - 1, map_zoom_index_for_km(500.0));
}

void test_step_walks_one_rung_each_way(void) {
    TEST_ASSERT_EQUAL_INT(2, map_zoom_step(3, -1, 5.0));
    TEST_ASSERT_EQUAL_INT(4, map_zoom_step(3, +1, 5.0));
}

void test_step_clamps_at_both_ends(void) {
    TEST_ASSERT_EQUAL_INT(0, map_zoom_step(0, -1, 5.0));
    TEST_ASSERT_EQUAL_INT(MAP_ZOOM_LEVEL_COUNT - 1,
                          map_zoom_step(MAP_ZOOM_LEVEL_COUNT - 1, +1, 5.0));
    TEST_ASSERT_FALSE(map_zoom_can_step(0, -1, 5.0));
    TEST_ASSERT_FALSE(map_zoom_can_step(MAP_ZOOM_LEVEL_COUNT - 1, +1, 5.0));
    TEST_ASSERT_TRUE(map_zoom_can_step(0, +1, 5.0));
    TEST_ASSERT_TRUE(map_zoom_can_step(MAP_ZOOM_LEVEL_COUNT - 1, -1, 5.0));
}

void test_leaving_auto_steps_one_rung_from_where_auto_sits(void) {
    /* Auto resolving to 5 km is index 3, so the first press must land on 2 km
     * (in) or 10 km (out), not at the end of the ladder. */
    TEST_ASSERT_EQUAL_INT(2, map_zoom_step(MAP_ZOOM_AUTO, -1, 5.0));
    TEST_ASSERT_EQUAL_INT(4, map_zoom_step(MAP_ZOOM_AUTO, +1, 5.0));
    TEST_ASSERT_EQUAL_INT(2000, level_m(map_zoom_step(MAP_ZOOM_AUTO, -1, 5.0)));

    /* Auto sitting on a rung end still steps to a real index: leaving auto
     * pins the window, which is a state change even when the level holds. */
    TEST_ASSERT_EQUAL_INT(0, map_zoom_step(MAP_ZOOM_AUTO, -1, 0.5));
    TEST_ASSERT_TRUE(map_zoom_can_step(MAP_ZOOM_AUTO, -1, 0.5));
    TEST_ASSERT_EQUAL_INT(MAP_ZOOM_LEVEL_COUNT - 1, map_zoom_step(MAP_ZOOM_AUTO, +1, 50.0));
    TEST_ASSERT_TRUE(map_zoom_can_step(MAP_ZOOM_AUTO, +1, 50.0));
}

void test_readout_names_the_mode_and_the_level(void) {
    char buf[24];

    map_zoom_format(buf, sizeof(buf), MAP_ZOOM_AUTO, 5.0);
    TEST_ASSERT_EQUAL_STRING("auto 5 km", buf);

    map_zoom_format(buf, sizeof(buf), 2, 2.0);
    TEST_ASSERT_EQUAL_STRING("manual 2 km", buf);

    /* Sub-kilometre reads in metres, like the scale bar. */
    map_zoom_format(buf, sizeof(buf), 0, 0.5);
    TEST_ASSERT_EQUAL_STRING("manual 500 m", buf);
    map_zoom_format(buf, sizeof(buf), MAP_ZOOM_AUTO, 0.5);
    TEST_ASSERT_EQUAL_STRING("auto 500 m", buf);

    /* Every rung has a readout that fits the label buffer the screen uses. */
    for (int i = 0; i < MAP_ZOOM_LEVEL_COUNT; i++) {
        map_zoom_format(buf, sizeof(buf), i, map_zoom_level_km(i));
        TEST_ASSERT_TRUE(strlen(buf) > 0 && strlen(buf) < sizeof(buf) - 1);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_levels_ascend_and_clamp);
    RUN_TEST(test_index_for_km_picks_the_first_level_that_fits);
    RUN_TEST(test_step_walks_one_rung_each_way);
    RUN_TEST(test_step_clamps_at_both_ends);
    RUN_TEST(test_leaving_auto_steps_one_rung_from_where_auto_sits);
    RUN_TEST(test_readout_names_the_mode_and_the_level);
    return UNITY_END();
}
